/* sim.c — the fixed-step simulation.
 *
 * Everything here runs at 60 Hz from (state, input, dt) and nothing else. The
 * order is the contract: flow field, player, enemies, combat, then the drains.
 * Nothing allocates, nothing reads the clock, and the only randomness is the
 * seeded pcg32 the level generator and the pickup rolls draw from.
 *
 * Existence, not flags: `alive` and `dying` are the sets a system walks, and a
 * slot is a slot because it sits in one of them. Enemies are bucketed into
 * their grid cell once per step, so separation reads the 3x3 neighbourhood
 * instead of the whole table. */

#include "crypt.h"

#include <math.h>
#include <string.h>

const char *crypt_character_path[CHARACTER_COUNT] = {
    "assets/Knight.mua", "assets/Rogue.mua",  "assets/Rogue_Hooded.mua",
    "assets/Mage.mua",   "assets/Ranger.mua", "assets/Barbarian.mua",
};

const GameAsset crypt_weapon_for_character[CHARACTER_COUNT] = {
    A_SWORD_1H, A_AXE_1H, A_SWORD_1H, A_WAND, A_SWORD_2H, A_AXE_1H,
};

const Upgrade crypt_upgrades[] = {
    {"WHETSTONE", "swing damage +30%", .damage = 0.30f},
    {"LONGSWORD", "swing arc +35%", .swing = 0.35f},
    {"SWIFT BOOTS", "move speed +18%", .speed = 0.18f},
    {"LEECHING", "25% of damage returns as health", .lifesteal = 0.25f},
    {"IRON PLATE", "take 25% less damage", .armor = 0.25f},
    {"THIEF'S EYE", "gold found +60%", .greed = 0.60f},
    {"BLOODLUST", "damage +18%, arc +15%", .damage = 0.18f, .swing = 0.15f},
    {"SECOND WIND", "full heal every floor"},
};
const uint32_t crypt_upgrade_count = (uint32_t)(sizeof(crypt_upgrades) / sizeof(crypt_upgrades[0]));

#define SIM_DT (1.0f / 60.0f)
#define HALF_PI 1.5707963f

static const vec3 kYawAxis = {0.0f, 1.0f, 0.0f};
static const vec3 kLift   = {0.0f, 0.22f, 0.0f};

/* Tuning, in one place: every number a designer would want to reach for. */
#define PLAYER_RADIUS 0.42f
#define PLAYER_SPEED 4.6f
#define PLAYER_ACCEL 30.0f
#define SWING_TIME 0.40f
#define SWING_CD 0.50f
#define SWING_REACH 2.2f
#define SWING_ARC 2.0f /* radians across the whole cone */
#define SWING_DAMAGE 26.0f
#define DASH_TIME 0.20f
#define DASH_CD 0.90f
#define DASH_SPEED 14.0f
#define ENEMY_RADIUS 0.40f
#define DEATH_TIME 0.8f
#define FLOW_HZ 8.0f

#define WAVE_GAP_TICKS 150
#define SPAWN_GAP_TICKS 26

/* ------------------------------------------------------------------ grid -- */

/* One span test per axis. A circle against axis-aligned cells reduces to its
   AABB for a body that only ever moves along one axis at a time, which is
   exactly what the axis-separated move below does. */
static bool span_blocked(float a, float b, float r, bool along_x) {
    int lo = crypt_world_x(a - r), hi = crypt_world_x(a + r);
    int y0 = crypt_world_z(b - r), y1 = crypt_world_z(b + r);
    if (along_x) {
        for (int y = y0; y <= y1; ++y)
            if (!crypt_walkable(hi, y) || !crypt_walkable(lo, y))
                return true;
    } else {
        for (int x = lo; x <= hi; ++x)
            if (!crypt_walkable(x, hi) || !crypt_walkable(x, lo))
                return true;
    }
    return false;
}

static void move_body(vec3 p, float dx, float dz, float r) {
    if (dx != 0.0f && !span_blocked(p[0] + dx, p[2], r, true))
        p[0] += dx;
    if (dz != 0.0f && !span_blocked(p[0], p[2] + dz, r, false))
        p[2] += dz;
}

/* ------------------------------------------------------------- instances -- */

static uint32_t make_instance(Game *g, uint32_t mesh) {
    return scene_instance_create(g->scene, &(SceneInstanceDesc){.mesh = mesh, .scale = 1.0f});
}

static void kill_instance(Game *g, uint32_t slot) {
    if (slot != UINT32_MAX)
        scene_instance_destroy(g->scene, slot);
}

/* --------------------------------------------------------------- spawning -- */

static void enemy_spawn(Game *g, EnemyKind kind, float x, float z) {
    if (!g->e_free_count)
        return;
    uint32_t e = g->e_free[--g->e_free_count];
    mu_sparse_set_insert(&g->e_alive, e);

    g->e_pos[e][0] = x;
    g->e_pos[e][1] = 0.0f;
    g->e_pos[e][2] = z;
    glm_vec3_zero(g->e_vel[e]);
    g->e_yaw[e]     = 0.0f;
    g->e_anim[e]    = (float)e * 1.7f;
    g->e_health[e]  = 42.0f + (float)g->depth * 10.0f;
    g->e_speed[e]   = (kind == EK_SHIELD) ? 2.0f : (kind == EK_CROSSBOW ? 2.4f : 2.75f);
    g->e_flash[e]   = 0.0f;
    g->e_death[e]   = 0.0f;
    g->e_attack[e]  = 0.0f;
    g->e_reach[e]   = (kind == EK_SHIELD) ? 1.6f : 1.45f;
    g->e_kind[e]    = kind;
    g->e_slot[e]    = make_instance(g, g->asset_first[A_SKEL_STAFF + kind]);
}

/* The mix is a table read per spawn, not a per-enemy enum switch. */
static const EnemyKind kWaveMix[] = {EK_STAFF, EK_STAFF, EK_STAFF, EK_AXE, EK_SHIELD, EK_CROSSBOW};
#define WAVE_MIX_COUNT ((uint32_t)(sizeof(kWaveMix) / sizeof(kWaveMix[0])))

static void wave_spawn_one(Game *g) {
    if (!g->wave_to_spawn || !g->spawn_room_count)
        return;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Room *r = &g->spawn_room[mu_pcg32_next_u32(&g->rng) % g->spawn_room_count];
        float fx = crypt_cell_x(r->x + (int)(mu_pcg32_next_u32(&g->rng) % (uint32_t)r->w));
        float fz = crypt_cell_z(r->y + (int)(mu_pcg32_next_u32(&g->rng) % (uint32_t)r->h));
        float dx = fx - g->pos[0], dz = fz - g->pos[2];
        if (dx * dx + dz * dz < 42.0f)
            continue; /* the wave arrives around the player, never inside them */
        enemy_spawn(g, kWaveMix[mu_pcg32_next_u32(&g->rng) % WAVE_MIX_COUNT], fx, fz);
        --g->wave_to_spawn;
        return;
    }
}

/* ------------------------------------------------------------------ reset -- */

void crypt_sim_reset(Game *g, uint32_t seed) {
    g->seed        = seed;
    g->depth       = 0;
    g->run_time    = 0.0f;
    g->kills       = 0;
    g->gold        = 0;
    g->state       = GS_PLAYING;
    g->health_max  = 100.0f;
    g->character   = seed % CHARACTER_COUNT;
    memset(&g->roll, 0, sizeof(g->roll));
    memset(g->offer, 0, sizeof(g->offer));
}

void crypt_floor_start(Game *g) {
    /* Everything the last floor left behind is retired here, at a load
       boundary, which is what keeps the destroy queue out of the step. */
    for (uint32_t i = 0; i < g->e_alive.count; ++i)
        kill_instance(g, g->e_slot[mu_sparse_set_at(&g->e_alive, i)]);
    for (uint32_t i = 0; i < g->e_dying.count; ++i)
        kill_instance(g, g->e_slot[mu_sparse_set_at(&g->e_dying, i)]);
    for (uint32_t i = 0; i < g->p_alive.count; ++i)
        kill_instance(g, g->p_slot[mu_sparse_set_at(&g->p_alive, i)]);
    mu_sparse_set_clear(&g->e_alive);
    mu_sparse_set_clear(&g->e_dying);
    mu_sparse_set_clear(&g->p_alive);
    for (uint32_t i = 0; i < MAX_ENEMIES; ++i)
        g->e_free[i] = MAX_ENEMIES - 1u - i;
    g->e_free_count = MAX_ENEMIES;
    for (uint32_t i = 0; i < MAX_PICKUPS; ++i)
        g->p_free[i] = MAX_PICKUPS - 1u - i;
    g->p_free_count = MAX_PICKUPS;

    crypt_level_generate(g->seed, g->depth);
    crypt_level_choose_ends(g);
    crypt_level_build(g);

    g->pos[0]    = crypt_cell_x(g->start_x);
    g->pos[1]    = 0.0f;
    g->pos[2]    = crypt_cell_z(g->start_y);
    glm_vec3_zero(g->vel);
    g->yaw       = 0.0f;
    g->anim      = 0.0f;
    g->health    = g->health_max;
    g->iframes   = 0.0f;
    g->attack_cd = 0.0f;
    g->attack_t  = 0.0f;
    g->dash_cd   = 0.0f;
    g->dash_t    = 0.0f;
    g->dash_dir[0] = g->dash_dir[2] = 0.0f;

    g->waves_per_floor = 2u + MU_MIN(g->depth, 4u);
    g->wave            = 0;
    g->wave_phase      = WS_WAIT;
    g->wave_timer      = 90;
    g->wave_to_spawn   = 0;
    g->exit_open       = false;
    g->state           = GS_PLAYING;
    g->flow_timer      = 0.0f;
    crypt_flow_rebuild(g);
}

static void start_wave(Game *g) {
    g->wave_to_spawn = 4u + g->wave * 3u + g->depth * 2u;
    g->wave_phase    = WS_SPAWN;
    g->wave_timer    = 0;
}

/* ---------------------------------------------------------------- player -- */

static void player_damage(Game *g, float amount) {
    if (g->iframes > 0.0f || g->state != GS_PLAYING)
        return;
    g->health -= amount * (1.0f - MU_CLAMP(g->roll.armor, 0.0f, 0.85f));
    g->iframes = 0.50f;
    g->cam_shake = MU_MAX(g->cam_shake, 0.5f);
    if (g->health <= 0.0f) {
        g->health = 0.0f;
        g->state  = GS_DEAD;
    }
}

static void player_step(Game *g, float in_x, float in_z) {
    float len = sqrtf(in_x * in_x + in_z * in_z);
    if (len > 1.0f) {
        in_x /= len;
        in_z /= len;
        len    = 1.0f;
    }

    /* The dash owns the velocity for its duration and carries its own
       i-frames, so the two never fight over the same term. */
    if (g->dash_t > 0.0f) {
        g->dash_t -= SIM_DT;
        g->iframes = MU_MAX(g->iframes, 0.12f);
        g->vel[0]  = g->dash_dir[0] * DASH_SPEED;
        g->vel[2]  = g->dash_dir[2] * DASH_SPEED;
    } else {
        float speed = PLAYER_SPEED * (1.0f + g->roll.speed);
        float k      = MU_CLAMP(PLAYER_ACCEL * SIM_DT, 0.0f, 1.0f);
        g->vel[0] += (in_x * speed - g->vel[0]) * k;
        g->vel[2] += (in_z * speed - g->vel[2]) * k;
        if (len < 0.01f) {
            g->vel[0] *= 0.78f;
            g->vel[2] *= 0.78f;
        }
    }

    move_body(g->pos, g->vel[0] * SIM_DT, g->vel[2] * SIM_DT, PLAYER_RADIUS);

    /* Facing follows the camera, not the motion: strafing keeps the character
       square to where the player is looking, which is what makes the swing arc
       land where the reticle is. */
    g->yaw = g->cam_yaw;

    float speed2 = g->vel[0] * g->vel[0] + g->vel[2] * g->vel[2];
    g->anim += (3.5f + 6.0f * MU_CLAMP(speed2 / 20.0f, 0.0f, 1.0f)) * SIM_DT;

    g->attack_cd -= SIM_DT;
    g->dash_cd   -= SIM_DT;
    g->iframes   -= SIM_DT;
    if (g->attack_t > 0.0f)
        g->attack_t -= SIM_DT;
}

void crypt_player_attack(Game *g) {
    if (g->state != GS_PLAYING || g->attack_cd > 0.0f || g->dash_t > 0.0f)
        return;
    g->attack_t  = SWING_TIME;
    g->attack_cd = SWING_CD;
}

void crypt_player_dash(Game *g) {
    if (g->state != GS_PLAYING || g->dash_cd > 0.0f || g->dash_t > 0.0f)
        return;
    g->dash_cd = DASH_CD;
    g->dash_t  = DASH_TIME;
    float len  = sqrtf(g->vel[0] * g->vel[0] + g->vel[2] * g->vel[2]);
    if (len > 0.1f) {
        g->dash_dir[0] = g->vel[0] / len;
        g->dash_dir[2] = g->vel[2] / len;
    } else {
        g->dash_dir[0] = sinf(g->yaw);
        g->dash_dir[2] = cosf(g->yaw);
    }
}

/* --------------------------------------------------------------- enemies -- */

static void bucket_rebuild(Game *g) {
    memset(g->bucket_head, -1, GRID_W * GRID_H * sizeof(int32_t));
    for (uint32_t i = 0; i < g->e_alive.count; ++i) {
        uint32_t e    = mu_sparse_set_at(&g->e_alive, i);
        int      cx   = crypt_world_x(g->e_pos[e][0]);
        int      cy   = crypt_world_z(g->e_pos[e][2]);
        if (!crypt_cell_inside(cx, cy))
            continue;
        int32_t c       = crypt_cell_index(cx, cy);
        g->bucket_next[e] = g->bucket_head[c];
        g->bucket_head[c] = (int32_t)e;
    }
}

static const int kNeighbour8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};

static void enemies_step(Game *g) {
    int pcx = crypt_world_x(g->pos[0]), pcy = crypt_world_z(g->pos[2]);

    for (uint32_t i = 0; i < g->e_alive.count; ++i) {
        uint32_t e = mu_sparse_set_at(&g->e_alive, i);
        vec3    *p = &g->e_pos[e];

        float to_x   = g->pos[0] - (*p)[0], to_z = g->pos[2] - (*p)[2];
        float dist   = sqrtf(to_x * to_x + to_z * to_z);
        float want_x = 0.0f, want_z = 0.0f;

        /* Inside a short leash it walks straight at the player; outside, it
           reads the shared field. Distance is the state, so there is no
           per-unit machine to branch on. */
        if (dist < 7.0f) {
            want_x = to_x;
            want_z = to_z;
        } else {
            int cx = crypt_world_x((*p)[0]), cy = crypt_world_z((*p)[2]);
            int bcx = cx, bcy = cy;
            int16_t best = g->flow[crypt_cell_index(cx, cy)];
            for (int d = 0; d < 8; ++d) {
                int nx = cx + kNeighbour8[d][0], ny = cy + kNeighbour8[d][1];
                if (!crypt_walkable(nx, ny))
                    continue;
                int16_t v = g->flow[crypt_cell_index(nx, ny)];
                if (v < best) {
                    best = v;
                    bcx  = nx;
                    bcy  = ny;
                }
            }
            if (bcx != cx || bcy != cy) {
                /* Aim at the target cell's centre, not at the step code: a
                   diagonal step taken literally lands on the corner and the
                   unit sticks there. */
                want_x = crypt_cell_x(bcx) - (*p)[0];
                want_z = crypt_cell_z(bcy) - (*p)[2];
            }
        }

        float want_len = sqrtf(want_x * want_x + want_z * want_z);
        if (want_len > 0.001f) {
            want_x /= want_len;
            want_z /= want_len;
        }

        /* Separation from the bucket, so a wave fills a doorway instead of
           collapsing into one point. */
        float sep_x = 0.0f, sep_z = 0.0f;
        int   ccx = crypt_world_x((*p)[0]), ccy = crypt_world_z((*p)[2]);
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox) {
                int nx = ccx + ox, ny = ccy + oy;
                if (!crypt_cell_inside(nx, ny))
                    continue;
                for (int32_t n = g->bucket_head[crypt_cell_index(nx, ny)]; n >= 0; n = g->bucket_next[n]) {
                    if ((uint32_t)n == e)
                        continue;
                    float dx = (*p)[0] - g->e_pos[n][0], dz = (*p)[2] - g->e_pos[n][2];
                    float d2 = dx * dx + dz * dz;
                    if (d2 > 1e-4f && d2 < 0.64f * 0.64f) {
                        float inv = 1.0f / sqrtf(d2);
                        sep_x += dx * inv;
                        sep_z += dz * inv;
                    }
                }
            }

        float vx = want_x * g->e_speed[e] + sep_x * 2.6f;
        float vz = want_z * g->e_speed[e] + sep_z * 2.6f;
        move_body(g->e_pos[e], vx * SIM_DT, vz * SIM_DT, ENEMY_RADIUS);

        float speed2 = vx * vx + vz * vz;
        g->e_anim[e] += (3.0f + 5.0f * MU_CLAMP(speed2 / 8.0f, 0.0f, 1.0f)) * SIM_DT;
        if (dist < 7.0f)
            g->e_yaw[e] = atan2f(to_x, to_z);
        else if (want_len > 0.001f)
            g->e_yaw[e] = atan2f(want_x, want_z);

        if (g->e_attack[e] > 0.0f)
            g->e_attack[e] -= SIM_DT;
        if (g->e_flash[e] > 0.0f)
            g->e_flash[e] -= SIM_DT;

        if (dist < g->e_reach[e] + PLAYER_RADIUS && g->e_attack[e] <= 0.0f) {
            g->e_attack[e] = 1.15f;
            player_damage(g, 8.0f + (float)g->depth * 1.5f);
        }
    }
    (void)pcx;
    (void)pcy;
}

/* ---------------------------------------------------------------- combat -- */

static void pickup_spawn(Game *g, PickupKind kind, float x, float z) {
    if (!g->p_free_count)
        return;
    uint32_t p = g->p_free[--g->p_free_count];
    mu_sparse_set_insert(&g->p_alive, p);
    g->p_pos[p][0] = x;
    g->p_pos[p][1] = 0.35f;
    g->p_pos[p][2] = z;
    g->p_life[p]   = 60.0f;
    g->p_kind[p]   = kind;
    g->p_slot[p]   = make_instance(g, g->asset_first[kind == PK_GOLD ? A_COIN : A_CHEST_GOLD]);
}

static void combat_step(Game *g) {
    /* The swing resolves in one window as the blade crosses the front. Testing
       the whole animation would let a slow walk connect from further away than
       the animation ever shows. */
    if (g->attack_t <= 0.0f || g->state != GS_PLAYING)
        return;

    float reach = SWING_REACH * (1.0f + g->roll.swing);
    float half  = SWING_ARC * 0.5f * (1.0f + g->roll.swing);
    float fx    = sinf(g->yaw), fz = cosf(g->yaw);
    float lifesteal = g->roll.lifesteal;

    for (uint32_t i = 0; i < g->e_alive.count; ++i) {
        uint32_t e   = mu_sparse_set_at(&g->e_alive, i);
        vec3    *p   = &g->e_pos[e];
        float    dx  = (*p)[0] - g->pos[0], dz = (*p)[2] - g->pos[2];
        float    d2  = dx * dx + dz * dz;
        if (d2 > reach * reach)
            continue;
        float d   = sqrtf(d2) + 1e-4f;
        float dot = (dx / d) * fx + (dz / d) * fz;
        if (dot < cosf(half))
            continue;

        float damage = SWING_DAMAGE * (1.0f + g->roll.damage);
        g->e_health[e] -= damage;
        g->e_flash[e]   = 0.18f;
        move_body(g->e_pos[e], (dx / d) * 0.5f, (dz / d) * 0.5f, ENEMY_RADIUS);
        if (lifesteal > 0.0f)
            g->health = MU_MIN(g->health_max, g->health + damage * lifesteal);
        if (g->e_health[e] <= 0.0f) {
            mu_sparse_set_remove(&g->e_alive, e);
            mu_sparse_set_insert(&g->e_dying, e);
            g->e_death[e] = DEATH_TIME;
            ++g->kills;
            uint32_t roll = mu_pcg32_next_u32(&g->rng);
            if (roll % 4u == 0u)
                pickup_spawn(g, (g->health < g->health_max * 0.6f) ? PK_HEART : PK_GOLD, (*p)[0], (*p)[2]);
        }
    }
}

static void pickups_step(Game *g) {
    for (uint32_t i = 0; i < g->p_alive.count; ++i) {
        uint32_t p = mu_sparse_set_at(&g->p_alive, i);
        g->p_life[p] -= SIM_DT;
        float dx = g->pos[0] - g->p_pos[p][0], dz = g->pos[2] - g->p_pos[p][2];
        float d2 = dx * dx + dz * dz;

        bool collect = (g->p_kind[p] == PK_GOLD) ? (d2 < 1.4f * 1.4f)
                                                 : (d2 < 1.4f * 1.4f && g->health < g->health_max);
        if (!collect && g->p_life[p] <= 0.0f)
            collect = true;
        if (!collect)
            continue;

        if (g->p_kind[p] == PK_GOLD)
            g->gold += (uint32_t)(7.0f * (1.0f + g->roll.greed));
        else
            g->health = MU_MIN(g->health_max, g->health + 35.0f);

        kill_instance(g, g->p_slot[p]);
        g->p_slot[p] = UINT32_MAX;
        mu_sparse_set_remove(&g->p_alive, p);
        g->p_free[g->p_free_count++] = p;
    }
}

/* The drain: deaths resolve here, after every system that walks `alive` is done
   with it, so a unit can never be stepped and freed in the same tick. */
static void deaths_step(Game *g) {
    for (uint32_t i = 0; i < g->e_dying.count;) {
        uint32_t e = mu_sparse_set_at(&g->e_dying, i);
        g->e_death[e] -= SIM_DT;
        if (g->e_death[e] > 0.0f) {
            ++i;
            continue;
        }
        kill_instance(g, g->e_slot[e]);
        g->e_slot[e] = UINT32_MAX;
        mu_sparse_set_remove(&g->e_dying, e);
        g->e_free[g->e_free_count++] = e;
    }
}

static void waves_step(Game *g) {
    if (g->state != GS_PLAYING)
        return;
    switch (g->wave_phase) {
        case WS_WAIT:
            if (--g->wave_timer <= 0)
                start_wave(g);
            break;
        case WS_SPAWN:
            if (--g->wave_timer <= 0) {
                wave_spawn_one(g);
                g->wave_timer = SPAWN_GAP_TICKS;
                if (!g->wave_to_spawn)
                    g->wave_phase = WS_CLEAR;
            }
            break;
        case WS_CLEAR:
            if (g->e_alive.count)
                break;
            ++g->wave;
            if (g->wave >= g->waves_per_floor) {
                g->wave_phase = WS_DONE;
                g->exit_open  = true;
                g->state      = GS_CLEARED;
            } else {
                g->wave_phase = WS_WAIT;
                g->wave_timer = WAVE_GAP_TICKS;
            }
            break;
        case WS_DONE:
            break;
    }
}

bool crypt_player_alive(const Game *g) { return g->state != GS_DEAD; }

/* ------------------------------------------------------------------ view -- */

/* scene_instance_set takes a whole row, so the mesh travels with the
   transform: writing pos/quat without it would silently redraw every dynamic
   instance as mesh 0. */
static void put_instance(Scene *s, uint32_t slot, uint32_t mesh, vec3 p, versor q, float scale) {
    if (slot == UINT32_MAX)
        return;
    scene_instance_set(s, slot, &(SceneInstanceDesc){.pos   = {p[0], p[1], p[2]},
                                                     .quat  = {q[0], q[1], q[2], q[3]},
                                                     .scale = scale,
                                                     .mesh  = mesh});
}

/* The player's part instances and the weapon riding the right hand. Written once
   per rendered frame from the latest simulation state — there is no
   interpolation buffer because at a 60 Hz sim against a 60 Hz-or-faster display
   the residual is under a frame of motion. */
void crypt_render_sync(Game *g) {
    const RiggedModel *rig = &crypt_character[g->character];
    versor               root_rot;
    glm_quatv(root_rot, g->yaw, kYawAxis);

    if (g->attack_t > 0.0f) {
        crypt_pose_swing(g->joint_now, MU_CLAMP(1.0f - g->attack_t / SWING_TIME, 0.0f, 1.0f));
    } else {
        float speed2 = g->vel[0] * g->vel[0] + g->vel[2] * g->vel[2];
        crypt_pose_idle(g->joint_now, g->anim, MU_CLAMP(speed2 / 16.0f, 0.0f, 1.0f));
    }

    static vec3   part_pos[MAX_ASSET_MESHES];
    static versor part_quat[MAX_ASSET_MESHES];
    crypt_rig_solve(rig, g->pos, root_rot, g->joint_now, part_pos, part_quat);
    for (uint32_t p = 0; p < rig->part_count; ++p)
        put_instance(g->scene, g->part_slot[p], rig->parts[p].mesh, part_pos[p], part_quat[p], 1.0f);

    /* Weapon in the right hand: the hand is the end of the right arm, so the
       blade's transform is that part's transform pushed down its own axis. */
    for (uint32_t p = 0; p < rig->part_count; ++p) {
        if (rig->parts[p].joint != J_ARM_R || !rig->parts[p].rotate)
            continue;
        vec3 down;
        glm_quat_rotatev(part_quat[p], (vec3){0.0f, -0.85f, 0.0f}, down);
        vec3 hand = {part_pos[p][0] + down[0], part_pos[p][1] + down[1], part_pos[p][2] + down[2]};
        put_instance(g->scene, g->weapon_slot, g->asset_first[crypt_weapon_for_character[g->character]], hand, part_quat[p], 1.0f);
        break;
    }

    for (uint32_t i = 0; i < g->e_alive.count; ++i) {
        uint32_t e = mu_sparse_set_at(&g->e_alive, i);
        versor     q;
        glm_quatv(q, g->e_yaw[e], kYawAxis);
        /* The hit flash rides the instance's bloom weight, the only per-instance
           channel the shader reads, so a struck unit lights up without a second
           material or a second draw. */
        scene_set_instance_bloom(g->scene, g->e_slot[e], g->e_flash[e] > 0.0f ? 2.5f : 0.0f);
        put_instance(g->scene, g->e_slot[e], g->asset_first[A_SKEL_STAFF + g->e_kind[e]], g->e_pos[e], q, 1.0f);
    }

    /* Dying units topple: a rotation about the foot rather than a fade, so the
       kill reads without any transparency in the opaque pipeline. */
    for (uint32_t i = 0; i < g->e_dying.count; ++i) {
        uint32_t e = mu_sparse_set_at(&g->e_dying, i);
        float    t = 1.0f - MU_CLAMP(g->e_death[e] / DEATH_TIME, 0.0f, 1.0f);
        versor     yaw_q, fall, q;
        glm_quatv(yaw_q, g->e_yaw[e], kYawAxis);
        glm_quatv(fall, HALF_PI * MU_CLAMP(t * 1.4f, 0.0f, 1.0f), (vec3){1.0f, 0.0f, 0.0f});
        glm_quat_mul(q, yaw_q, fall);
        vec3 lift;
        glm_quat_rotatev(q, kLift, lift);
        vec3 at = {g->e_pos[e][0] + lift[0], g->e_pos[e][1] + lift[1], g->e_pos[e][2] + lift[2]};
        scene_set_instance_bloom(g->scene, g->e_slot[e], 0.0f);
        put_instance(g->scene, g->e_slot[e], g->asset_first[A_SKEL_STAFF + g->e_kind[e]], at, q, MU_CLAMP(1.0f - t * 0.3f, 0.6f, 1.0f));
    }

    for (uint32_t i = 0; i < g->p_alive.count; ++i) {
        uint32_t p  = mu_sparse_set_at(&g->p_alive, i);
        vec3     at = {g->p_pos[p][0], g->p_pos[p][1] + sinf(g->run_time * 3.0f + (float)p) * 0.09f,
                       g->p_pos[p][2]};
        versor q;
        glm_quat_identity(q);
        put_instance(g->scene, g->p_slot[p], g->asset_first[g->p_kind[p] == PK_GOLD ? A_COIN : A_CHEST_GOLD], at, q, 1.0f);
    }
}

void crypt_sim_step(Game *g, float in_x, float in_z) {
    if (g->state != GS_DEAD)
        g->run_time += SIM_DT;

    if (g->state == GS_CHOOSING)
        return;

    g->flow_timer -= SIM_DT;
    if (g->flow_timer <= 0.0f) {
        g->flow_timer = 1.0f / FLOW_HZ;
        crypt_flow_rebuild(g);
    }

    bucket_rebuild(g);
    player_step(g, in_x, in_z);
    enemies_step(g);
    combat_step(g);
    pickups_step(g);
    deaths_step(g);
    waves_step(g);

    if (g->state == GS_CLEARED && crypt_world_x(g->pos[0]) == g->exit_x &&
        crypt_world_z(g->pos[2]) == g->exit_y)
        g->state = GS_CHOOSING;
}

void crypt_next_floor(Game *g) {
    ++g->depth;
    crypt_floor_start(g);
}

/* Three distinct offers, drawn without replacement from the pool. */
void crypt_roll_offers(Game *g) {
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t pick = i;
        for (uint32_t tries = 0; tries < 8; ++tries) {
            pick = mu_pcg32_next_u32(&g->rng) % crypt_upgrade_count;
            bool clash = false;
            for (uint32_t j = 0; j < i; ++j)
                if (memcmp(&g->offer[j], &crypt_upgrades[pick], sizeof(Upgrade)) == 0)
                    clash = true;
            if (!clash)
                break;
        }
        g->offer[i] = crypt_upgrades[pick];
    }
}

void crypt_take_offer(Game *g, uint32_t index) {
    if (index > 2)
        return;
    const Upgrade *u = &g->offer[index];
    g->roll.damage += u->damage;
    g->roll.swing += u->swing;
    g->roll.speed += u->speed;
    g->roll.lifesteal += u->lifesteal;
    g->roll.armor += u->armor;
    g->roll.greed += u->greed;
    crypt_next_floor(g);
}