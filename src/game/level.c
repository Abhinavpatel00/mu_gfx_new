/* level.c — the floor: grid, generator, and the static geometry it plants.
 *
 * The grid is the level's only authoritative spatial data. Wall pieces, torches,
 * pillars and the stairs are all derived from it, so a floor costs one byte per
 * cell plus one static instance per drawn piece, and nothing about the layout
 * has to be kept in sync with what is rendered.
 *
 * Generation is rooms-into-rock joined by L corridors, each room wired to its
 * nearest already-placed neighbour. Wiring to the nearest keeps the floor
 * connected by construction, which is both cheaper and more reliable than
 * carving broadly and then labelling components and discarding the islands. */

#include "crypt.h"

#include <math.h>
#include <string.h>

uint8_t  crypt_cell[GRID_W * GRID_H];
Room     crypt_room[MAX_ROOMS];
uint32_t crypt_room_count;

int crypt_cell_index(int x, int y) { return y * GRID_W + x; }
bool crypt_cell_inside(int x, int y) { return x >= 0 && y >= 0 && x < GRID_W && y < GRID_H; }
bool crypt_walkable(int x, int y) {
    return crypt_cell_inside(x, y) && (crypt_cell[crypt_cell_index(x, y)] & CELL_FLOOR) != 0u;
}
float crypt_cell_x(int x) { return GRID_X0 + ((float)x + 0.5f) * CELL; }
float crypt_cell_z(int y) { return GRID_Z0 + ((float)y + 0.5f) * CELL; }
int   crypt_world_x(float wx) { return (int)floorf((wx - GRID_X0) / CELL); }
int   crypt_world_z(float wz) { return (int)floorf((wz - GRID_Z0) / CELL); }

static versor kYaw[4];

static void yaw_table_init(void) {
    static const float kYawAngle[4] = {0.0f, 0.5707963f, 1.1415927f, -0.5707963f}; /* 0, 90, 180, -90 */
    static const float kYawAxis[3] = {0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 4; ++i)
        glm_quatv(kYaw[i], kYawAngle[i], (vec3){0.0f, 1.0f, 0.0f});
}

void crypt_level_generate(uint32_t seed, uint32_t depth) {
    memset(crypt_cell, 0, sizeof(crypt_cell));
    crypt_room_count = 0;
    if (kYaw[0][3] == 0.0f)
        yaw_table_init();

    mu_pcg32 rng;
    mu_pcg32_init(&rng, seed ^ (depth * 0x9E3779B9u), 0x5EEDu);

    const int room_count = 7 + (int)MU_MIN(depth, 6u);
    mu_rog_grid_u8 grid  = {GRID_W, GRID_H, crypt_cell};

    for (int attempt = 0; attempt < 500 && (int)crypt_room_count < room_count; ++attempt) {
        int w = 4 + (int)(mu_pcg32_next_u32(&rng) % 5u);
        int h = 4 + (int)(mu_pcg32_next_u32(&rng) % 5u);
        int x = 1 + (int)(mu_pcg32_next_u32(&rng) % (uint32_t)(GRID_W - w - 2));
        int y = 1 + (int)(mu_pcg32_next_u32(&rng) % (uint32_t)(GRID_H - h - 2));
        mu_rog_rect r = {x, y, w, h};

        bool overlap = false;
        for (uint32_t i = 0; i < crypt_room_count && !overlap; ++i) {
            mu_rog_rect grown = {crypt_room[i].x - 1, crypt_room[i].y - 1, crypt_room[i].w + 2, crypt_room[i].h + 2};
            overlap             = mu_rog_rect_intersects(r, grown, 0);
        }
        if (overlap)
            continue;

        crypt_room[crypt_room_count].x = x;
        crypt_room[crypt_room_count].y = y;
        crypt_room[crypt_room_count].w = w;
        crypt_room[crypt_room_count].h = h;
        mu_rog_carve_rect(grid, r, (uint8_t)CELL_FLOOR);
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                crypt_cell[crypt_cell_index(xx, yy)] |= CELL_ROOM;
        ++crypt_room_count;
    }

    mu_rog_rng rog_rng;
    mu_rog_rng_seed(&rog_rng, seed ^ (depth * 0x85EBCA6Bu));
    for (uint32_t i = 1; i < crypt_room_count; ++i) {
        mu_rog_ivec2 a = mu_rog_rect_center(
            (mu_rog_rect){crypt_room[i].x, crypt_room[i].y, crypt_room[i].w, crypt_room[i].h});
        mu_rog_ivec2 b = {0, 0};
        int          best_d = 1 << 30;
        for (uint32_t j = 0; j < i; ++j) {
            mu_rog_ivec2 c = mu_rog_rect_center(
                (mu_rog_rect){crypt_room[j].x, crypt_room[j].y, crypt_room[j].w, crypt_room[j].h});
            int d = (a.x - c.x) * (a.x - c.x) + (a.y - c.y) * (a.y - c.y);
            if (d < best_d) {
                best_d = d;
                b      = c;
            }
        }
        mu_rog_carve_l_corridor(grid, a, b, (uint8_t)CELL_FLOOR, &rog_rng);
    }

    /* A corridor cell that butts against a room wall becomes a doorway: the
       wall edge check below skips flagged cells, so the two spaces read as
       connected instead of as two rooms with masonry between them. */
    for (uint32_t i = 0; i < crypt_room_count; ++i) {
        int x0 = crypt_room[i].x - 1, x1 = crypt_room[i].x + crypt_room[i].w;
        int y0 = crypt_room[i].y - 1, y1 = crypt_room[i].y + crypt_room[i].h;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (!crypt_cell_inside(x, y))
                    continue;
                uint8_t *c = &crypt_cell[crypt_cell_index(x, y)];
                if (*c & CELL_FLOOR)
                    continue;
                if (crypt_walkable(x + 1, y) || crypt_walkable(x - 1, y) || crypt_walkable(x, y + 1) ||
                    crypt_walkable(x, y - 1))
                    *c |= CELL_FLOOR | CELL_DOOR;
            }
    }
}

/* Chooses the entry room and the exit room and marks the exit cell. */
void crypt_level_choose_ends(Game *g) {
    g->start_x = crypt_room[0].x + crypt_room[0].w / 2;
    g->start_y = crypt_room[0].y + crypt_room[0].h / 2;

    int far = 0, far_d = -1;
    for (uint32_t i = 0; i < crypt_room_count; ++i) {
        int cx = crypt_room[i].x + crypt_room[i].w / 2, cy = crypt_room[i].y + crypt_room[i].h / 2;
        int d  = (cx - g->start_x) * (cx - g->start_x) + (cy - g->start_y) * (cy - g->start_y);
        if (d > far_d) {
            far_d = d;
            far   = (int)i;
        }
    }
    g->exit_x = crypt_room[far].x + crypt_room[far].w / 2;
    g->exit_y = crypt_room[far].y + crypt_room[far].h / 2;
    crypt_cell[crypt_cell_index(g->exit_x, g->exit_y)] |= CELL_EXIT;

    g->spawn_room_count = 0;
    for (uint32_t i = 1; i < crypt_room_count; ++i)
        g->spawn_room[g->spawn_room_count++] = crypt_room[i];
}

/* One Dijkstra map from the player, every unit reads it. Rebuilding one field
   for the whole floor costs a grid sweep; the alternative, a path per enemy,
   costs a search per enemy per request and produces the same answers. */
void crypt_flow_rebuild(Game *g) {
    mu_rog_ivec2 goal = {crypt_world_x(g->pos[0]), crypt_world_z(g->pos[2])};
    if (!crypt_walkable(goal.x, goal.y))
        return;
    mu_rog_dijkstra_map8(GRID_W, GRID_H, &goal, 1, NULL, NULL, g->flow, g->flow_queue);
}

/* ------------------------------------------------------------------------- */

static void plant(Game *g, GameAsset asset, float x, float y, float z, versor q, float scale) {
    if (g->dungeon_used >= MAX_DUNGEON_INSTANCES)
        return;
    uint32_t slot = scene_instance_create_static(
        g->scene, &(SceneInstanceDesc){.pos = {x, y, z}, .quat = {q[0], q[1], q[2], q[3]}, .scale = scale,
                                       .mesh = g->asset_first[asset]});
    if (slot != UINT32_MAX)
        g->dungeon_slot[g->dungeon_used++] = slot;
}

static bool cell_is_door(int x, int y) {
    return crypt_cell_inside(x, y) && (crypt_cell[crypt_cell_index(x, y)] & CELL_DOOR) != 0u;
}

void crypt_level_build(Game *g) {
    for (uint32_t i = 0; i < g->dungeon_used; ++i)
        scene_instance_destroy(g->scene, g->dungeon_slot[i]);
    g->dungeon_used = 0;

    /* Floors. A hash of the cell picks stone or dirt, so the pattern costs no
       storage and is identical on every rebuild of the same floor. */
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x) {
            if (!crypt_walkable(x, y))
                continue;
            uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ g->seed;
            GameAsset a = A_FLOOR_TILE;
            if (h % 9u == 0u && (x & 1) == 0 && (y & 1) == 0)
                a = A_FLOOR_DIRT; /* dirt is a 4x4 sheet, so it lands on even cells only */
            plant(g, a, crypt_cell_x(x), 0.0f, crypt_cell_z(y), kYaw[0], 1.0f);
        }

    /* Walls on every floor-to-rock edge. barrier_half runs from its own origin
       along +x for 2 m, so a cell's north edge is yaw 0 at the cell's north
       corner, its south edge yaw 180, its west edge yaw -90 and its east edge
       yaw +90. A doorway cell drops the segment so the two rooms connect. */
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x) {
            if (!crypt_walkable(x, y))
                continue;
            float hx = crypt_cell_x(x) - CELL * 0.5f;
            float hz = crypt_cell_z(y) - CELL * 0.5f;

            if (!crypt_walkable(x, y - 1) && !cell_is_door(x, y - 1))
                plant(g, A_WALL, hx, 0.0f, hz, kYaw[0], 1.0f);
            if (!crypt_walkable(x, y + 1) && !cell_is_door(x, y + 1))
                plant(g, A_WALL, hx, 0.0f, hz + CELL, kYaw[2], 1.0f);
            if (!crypt_walkable(x - 1, y) && !cell_is_door(x - 1, y))
                plant(g, A_WALL, hx, 0.0f, hz, kYaw[3], 1.0f);
            if (!crypt_walkable(x + 1, y) && !cell_is_door(x + 1, y))
                plant(g, A_WALL, hx + CELL, 0.0f, hz, kYaw[1], 1.0f);

            /* Convex corners: rock diagonally outside two blocked edges. */
            if (!crypt_walkable(x, y - 1) && !crypt_walkable(x - 1, y) && !crypt_walkable(x - 1, y - 1) &&
                !cell_is_door(x - 1, y) && !cell_is_door(x, y - 1))
                plant(g, A_WALL_CORNER, hx, 0.0f, hz, kYaw[0], 1.0f);
            if (!crypt_walkable(x, y - 1) && !crypt_walkable(x + 1, y) && !crypt_walkable(x + 1, y - 1) &&
                !cell_is_door(x + 1, y) && !cell_is_door(x, y - 1))
                plant(g, A_WALL_CORNER, hx + CELL, 0.0f, hz, kYaw[1], 1.0f);
            if (!crypt_walkable(x, y + 1) && !crypt_walkable(x - 1, y) && !crypt_walkable(x - 1, y + 1) &&
                !cell_is_door(x - 1, y) && !cell_is_door(x, y + 1))
                plant(g, A_WALL_CORNER, hx, 0.0f, hz + CELL, kYaw[2], 1.0f);
            if (!crypt_walkable(x, y + 1) && !crypt_walkable(x + 1, y) && !crypt_walkable(x + 1, y + 1) &&
                !cell_is_door(x + 1, y) && !cell_is_door(x, y + 1))
                plant(g, A_WALL_CORNER, hx + CELL, 0.0f, hz + CELL, kYaw[3], 1.0f);
        }

    /* Dressing, one cluster per room. The hash keeps a room's furniture fixed
       across rebuilds while still varying between rooms and between runs. */
    for (uint32_t i = 0; i < crypt_room_count; ++i) {
        const Room *r = &crypt_room[i];
        uint32_t    h = i * 2654435761u + g->seed;
        float       cx = crypt_cell_x(r->x + r->w / 2), cz = crypt_cell_z(r->y + r->h / 2);
        /* Nothing goes in the middle: the player spawns at a room's centre and
           fights there, and a pillar between them and the camera is the one
           prop that makes a room unplayable. Rooms that are big enough get a
           second ring instead. */
        if (r->w >= 6 && r->h >= 6)
            plant(g, (h % 3u == 0u) ? A_TABLE : A_CRATES, cx - CELL, 0.0f, cz - CELL, kYaw[h % 4u], 1.0f);

        /* Corners get one prop each; rooms are 4x4 cells or bigger, so four
           corner props never collide with the ring prop above. */
        static const int kCornerX[4] = {0, 1, 0, 1};
        static const int kCornerY[4] = {0, 0, 1, 1};
        static const GameAsset kCornerAsset[4] = {A_CHEST, A_RUBBLE, A_BARREL, A_CHEST_GOLD};
        for (int k = 0; k < 4; ++k) {
            if (((h >> (k * 2)) & 3u) == 3u)
                continue;
            plant(g, kCornerAsset[k], crypt_cell_x(r->x + kCornerX[k] * (r->w - 1)), 0.0f,
                  crypt_cell_z(r->y + kCornerY[k] * (r->h - 1)), kYaw[(h >> k) & 3u], 1.0f);
        }

        /* Torches: the pack's only light, and the bloom pass turns them into
           visible flames. One per room wall cell, hashed so it is stable. */
        for (int k = 0; k < 4; ++k) {
            uint32_t bits = (h >> (k * 4)) & 15u;
            if (bits < 6u)
                continue;
            int tx = r->x + ((k & 1) ? r->w - 1 : 0);
            int ty = r->y + ((k & 2) ? r->h - 1 : 0);
            plant(g, (bits & 1u) ? A_TORCH : A_CANDLE, crypt_cell_x(tx), 0.0f, crypt_cell_z(ty),
                  kYaw[(bits >> 1) & 3u], 1.0f);
        }
    }

    plant(g, A_STAIRS, crypt_cell_x(g->exit_x), 0.0f, crypt_cell_z(g->exit_y), kYaw[0], 1.0f);
}