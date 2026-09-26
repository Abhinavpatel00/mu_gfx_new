#include "renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== game
 * Top-down paddock demo: a dog you steer around a fenced field, livestock
 * that idles, wanders, grazes and sleeps on its own, and feeding as the one
 * interaction. Everything is pushed as sprite instances in a fixed layer
 * order, so the scene collapses into a handful of indirect draws.
 *
 * This file owns the whole game. renderer.c only knows the two hooks in
 * GameHooks: farm_init once after the sprite system exists, farm_update every
 * frame before the flush. ===================================================================== */

#define FARM_WORLD_W 3200.0f
#define FARM_WORLD_H 1800.0f
#define FARM_TILE    128.0f
#define FARM_INSET   176.0f /* fence band; actors stay inside it */

#define FARM_MAX_ANIMALS   48u
#define FARM_MAX_PARTICLES 384u
#define FARM_MAX_SPECIES   8u
#define FARM_MAX_FRAMES    6u
#define FARM_DECOR_COUNT   7u

/* Ascending, so the stable batch order gives this painter order for free. */
#define FARM_LAYER_GROUND 0u
#define FARM_LAYER_DECOR  5u
#define FARM_LAYER_SHADOW 10u
#define FARM_LAYER_FENCE  15u
#define FARM_LAYER_ACTOR  20u
#define FARM_LAYER_FX     30u

typedef enum FarmState {
    FARM_IDLE = 0,
    FARM_WALK,
    FARM_EAT,
    FARM_SLEEP,
    FARM_HAPPY,
} FarmState;

typedef struct FarmSpecies {
    PictureId frames[FARM_MAX_FRAMES];
    uint32_t  frame_count;
    PictureId sleep; /* UINT32_MAX when the sheet has no lying pose */
    float     size;  /* drawn width in world units */
    float     speed; /* wander speed, world units / second */
} FarmSpecies;

typedef struct FarmAnimal {
    float   x, y;
    float   vx, vy;
    float   facing; /* -1 or +1 */
    float   anim;
    float   state_t;
    uint8_t state;
    uint8_t species;
} FarmAnimal;

typedef struct FarmPlayer {
    float x, y;
    float vx, vy;
    float facing;
    float anim;
    float lean; /* -1..1, drives the run squash */
} FarmPlayer;

typedef struct FarmParticle {
    float    x, y;
    float    vx, vy;
    float    t;
    float    life;
    float    size;
    uint32_t color;
    uint8_t  kind; /* 0 = puff, 1 = heart */
} FarmParticle;

typedef struct Farm {
    SpriteCamera camera;

    FarmPlayer player;
    FarmAnimal animals[FARM_MAX_ANIMALS];
    uint32_t   animal_count;

    FarmParticle particles[FARM_MAX_PARTICLES];
    uint32_t     particle_count;

    FarmSpecies species[FARM_MAX_SPECIES];
    uint32_t    species_count;

    PictureId ground[FARM_MAX_FRAMES];
    uint32_t  ground_count;
    PictureId decor[FARM_DECOR_COUNT]; /* 4 grass tufts, 3 mushrooms */
    PictureId fence;
    PictureId shadow;
    PictureId heart;
    PictureId player_frames[FARM_MAX_FRAMES];
    uint32_t  player_frame_count;

    float    dust_timer;
    uint32_t fed; /* feed counter, surfaced on the HUD */
    uint32_t rng; /* spawn layout and herd decisions */
} Farm;

static Farm g_farm;

/* ------------------------------------------------------------- small math */

static uint32_t farm_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t farm_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t farm_rand(Farm *f) {
    f->rng = f->rng * 1664525u + 1013904223u;
    return f->rng;
}

static float farm_randf(Farm *f) { return (float)(farm_rand(f) >> 8) * (1.0f / 16777216.0f); }

static float farm_rand_range(Farm *f, float lo, float hi) { return lo + farm_randf(f) * (hi - lo); }

static float move_toward(float value, float target, float step) {
    if (value < target) {
        value += step;
        return value > target ? target : value;
    }
    if (value > target) {
        value -= step;
        return value < target ? target : value;
    }
    return value;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* -------------------------------------------------------------- pictures */

static PictureId farm_named(SpriteSystem *s, const char *name, uint32_t index) {
    char path[128];
    snprintf(path, sizeof(path), "data/farm/%s_%02u.png", name, index);
    return picture_load(s, path);
}

/* Soft white ellipse; tinted through SpriteDraw.color so one picture serves
   as both the shadow blob and the dust puff. */
static PictureId farm_soft_blob(SpriteSystem *s, uint32_t w, uint32_t h) {
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba)
        return UINT32_MAX;

    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            float u = ((float)px + 0.5f) / (float)w * 2.0f - 1.0f;
            float v = ((float)py + 0.5f) / (float)h * 2.0f - 1.0f;
            float a = 1.0f - (u * u + v * v);
            if (a < 0.0f)
                a = 0.0f;
            a *= a;
            uint8_t *out = rgba + ((size_t)py * w + px) * 4;
            out[0]       = 255;
            out[1]       = 255;
            out[2]       = 255;
            out[3]       = (uint8_t)(a * 255.0f);
        }
    }

    PictureId id = picture_load_rgba(s, rgba, w, h);
    free(rgba);
    return id;
}

/* Heart silhouette from the implicit curve (x^2+y^2-1)^3 - x^2 y^3 <= 0,
   rasterised directly so the demo needs no extra art for feedback. */
static PictureId farm_heart(SpriteSystem *s, uint32_t w, uint32_t h) {
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba)
        return UINT32_MAX;

    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            float    u   = ((float)px + 0.5f) / (float)w * 2.4f - 1.2f;
            float    v   = 1.0f - ((float)py + 0.5f) / (float)h * 2.2f;
            float    k   = u * u + v * v - 1.0f;
            float    f   = k * k * k - u * u * v * v * v;
            uint8_t *out = rgba + ((size_t)py * w + px) * 4;
            uint8_t  on  = f <= 0.0f ? 255 : 0;
            out[0]       = 255;
            out[1]       = 255;
            out[2]       = 255;
            out[3]       = on;
        }
    }

    PictureId id = picture_load_rgba(s, rgba, w, h);
    free(rgba);
    return id;
}

/* ------------------------------------------------------------------ init */

typedef struct FarmSpeciesDef {
    const char *name;
    uint8_t     frames[6];
    uint8_t     frame_count;
    int8_t      sleep; /* -1 when the sheet has no lying pose */
    float       size;
    float       speed;
} FarmSpeciesDef;

static const FarmSpeciesDef k_species[] = {
    {.name = "cow", .frames = {0, 1, 2, 3, 4}, .frame_count = 5, .sleep = 8, .size = 104.0f, .speed = 42.0f},
    {.name = "sheep", .frames = {0, 1, 2, 3, 4, 5}, .frame_count = 6, .sleep = -1, .size = 88.0f, .speed = 48.0f},
    {.name = "pig", .frames = {0, 1, 2}, .frame_count = 3, .sleep = -1, .size = 78.0f, .speed = 56.0f},
    {.name = "chicken", .frames = {0, 1, 2, 3}, .frame_count = 4, .sleep = -1, .size = 46.0f, .speed = 80.0f},
    {.name = "duck", .frames = {0, 1, 2, 3}, .frame_count = 4, .sleep = -1, .size = 48.0f, .speed = 64.0f},
    {.name = "rabbit", .frames = {0, 1, 2, 3}, .frame_count = 4, .sleep = -1, .size = 42.0f, .speed = 94.0f},
    {.name = "goat", .frames = {0, 1, 2, 3}, .frame_count = 4, .sleep = -1, .size = 76.0f, .speed = 62.0f},
};

/* GameHooks.start: called once, after sprite_system_init has built the atlases. */
static void farm_init(void *user, SpriteSystem *sprites) {
    Farm *farm = (Farm *)user;
    memset(farm, 0, sizeof(*farm));
    farm->rng = 0xC0FFEE17u;

    /* Ground and scenery. */
    static const char *k_ground[]                = {"grass_top", "grass_brown"};
    static const char *k_decor[FARM_DECOR_COUNT] = {"grass1",       "grass2",         "grass3",      "grass4",
                                                    "mushroom_red", "mushroom_brown", "mushroom_tan"};

    for (uint32_t i = 0; i < 2; i++) {
        char path[128];
        snprintf(path, sizeof(path), "data/PNG/Tiles/%s.png", k_ground[i]);
        PictureId id = picture_load(sprites, path);
        if (id != UINT32_MAX)
            farm->ground[farm->ground_count++] = id;
    }

    for (uint32_t i = 0; i < FARM_DECOR_COUNT; i++) {
        char path[128];
        snprintf(path, sizeof(path), "data/PNG/Tiles/%s.png", k_decor[i]);
        PictureId id = picture_load(sprites, path);
        if (id == UINT32_MAX) {
            farm->decor[i] = UINT32_MAX;
            continue;
        }
        farm->decor[i] = id;
    }

    PictureId fence = picture_load(sprites, "data/PNG/Tiles/fence_wood.png");
    farm->fence     = fence != UINT32_MAX ? fence : farm->ground[0];

    farm->shadow = farm_soft_blob(sprites, 64, 32);
    farm->heart  = farm_heart(sprites, 24, 22);

    /* Player: the corgi stands facing right, so facing is a plain sign flip. */
    for (uint32_t i = 0; i < 2; i++) {
        PictureId id = farm_named(sprites, "dog", i == 0 ? 4u : 5u);
        if (id != UINT32_MAX)
            farm->player_frames[farm->player_frame_count++] = id;
    }

    /* Livestock. A species whose art failed to load is dropped, so the index
       the herd stores stays dense. */
    for (uint32_t i = 0; i < sizeof(k_species) / sizeof(k_species[0]); i++) {
        const FarmSpeciesDef *def = &k_species[i];
        FarmSpecies           sp  = {.sleep = UINT32_MAX, .size = def->size, .speed = def->speed};

        for (uint32_t k = 0; k < def->frame_count; k++) {
            PictureId id = farm_named(sprites, def->name, def->frames[k]);
            if (id == UINT32_MAX)
                continue;
            if (sp.frame_count < FARM_MAX_FRAMES)
                sp.frames[sp.frame_count++] = id;
        }

        if (def->sleep >= 0) {
            PictureId id = farm_named(sprites, def->name, (uint32_t)def->sleep);
            if (id != UINT32_MAX)
                sp.sleep = id;
        }

        if (sp.frame_count == 0) {
            log_error("[farm] no frames loaded for '%s'", def->name);
            continue;
        }
        farm->species[farm->species_count++] = sp;
    }

    farm->camera.x    = 0.0f;
    farm->camera.y    = 0.0f;
    farm->camera.zoom = 1.0f;
    farm->camera.snap = false;

    if (farm->species_count == 0 || farm->ground_count == 0) {
        log_error("[farm] demo art missing; nothing to draw");
        farm->animal_count = 0;
        return;
    }

    /* The herd starts in the middle of the paddock so the first frame already
       shows livestock; they disperse from there. */
    float centre_x = FARM_WORLD_W * 0.5f;
    float centre_y = FARM_WORLD_H * 0.5f;

    farm->player.x      = centre_x;
    farm->player.y      = centre_y + 240.0f;
    farm->player.facing = 1.0f;

    farm->animal_count = FARM_MAX_ANIMALS;
    for (uint32_t i = 0; i < farm->animal_count; i++) {
        FarmAnimal *a    = &farm->animals[i];
        uint32_t    spec = farm_rand(farm) % farm->species_count;

        a->species = (uint8_t)spec;
        a->x       = centre_x + farm_rand_range(farm, -760.0f, 760.0f);
        a->y       = centre_y + farm_rand_range(farm, -400.0f, 340.0f);
        a->facing  = farm_rand(farm) & 1u ? 1.0f : -1.0f;
        a->state   = (uint8_t)(farm_rand(farm) % FARM_SLEEP);
        a->state_t = farm_rand_range(farm, 0.4f, 3.0f);
        a->anim    = farm_randf(farm) * 6.0f;
    }

    log_info("[farm] %u species, %u animals, %u ground tiles", farm->species_count, farm->animal_count,
             farm->ground_count);
}

/* ------------------------------------------------------------- particles */

static void farm_particle(Farm *f, float x, float y, float vx, float vy, float life, float size, uint32_t color,
                          uint8_t kind) {
    if (f->particle_count >= FARM_MAX_PARTICLES)
        return;

    FarmParticle *p = &f->particles[f->particle_count++];
    *p              = (FarmParticle){.x = x, .y = y, .vx = vx, .vy = vy, .t = 0.0f, .life = life, .size = size};
    p->color        = color;
    p->kind         = kind;
}

static void farm_particles_update(Farm *f, float dt) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < f->particle_count; i++) {
        FarmParticle *p = &f->particles[i];
        p->t += dt;
        if (p->t >= p->life)
            continue;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy += (p->kind == 1 ? 26.0f : -18.0f) * dt;
        f->particles[n++] = *p;
    }
    f->particle_count = n;
}

/* ---------------------------------------------------------------- player */

static void farm_player_update(Farm *f, const Input *input, float dt) {
    FarmPlayer *p = &f->player;

    float dx = 0.0f, dy = 0.0f;
    if (key_down(input, KEY_W) || key_down(input, KEY_UP))
        dy -= 1.0f;
    if (key_down(input, KEY_S) || key_down(input, KEY_DOWN))
        dy += 1.0f;
    if (key_down(input, KEY_A) || key_down(input, KEY_LEFT))
        dx -= 1.0f;
    if (key_down(input, KEY_D) || key_down(input, KEY_RIGHT))
        dx += 1.0f;

    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.0f) {
        dx /= len;
        dy /= len;
    }

    const float max_speed = 300.0f;
    if (len > 0.0f) {
        p->vx     = move_toward(p->vx, dx * max_speed, 3600.0f * dt);
        p->vy     = move_toward(p->vy, dy * max_speed, 3600.0f * dt);
        p->facing = dx != 0.0f ? (dx > 0.0f ? 1.0f : -1.0f) : p->facing;
    } else {
        p->vx = move_toward(p->vx, 0.0f, 3200.0f * dt);
        p->vy = move_toward(p->vy, 0.0f, 3200.0f * dt);
    }

    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->x = clampf(p->x, FARM_INSET, FARM_WORLD_W - FARM_INSET);
    p->y = clampf(p->y, FARM_INSET, FARM_WORLD_H - FARM_INSET);

    float speed = sqrtf(p->vx * p->vx + p->vy * p->vy);
    float run   = speed / max_speed;
    p->anim += dt * (run > 0.05f ? 8.0f : 2.2f);
    p->lean = lerpf(p->lean, run, 1.0f - expf(-10.0f * dt));

    /* Dust under the paws, thickest while actually covering ground. */
    f->dust_timer -= dt;
    if (speed > 90.0f && f->dust_timer <= 0.0f) {
        f->dust_timer = 0.07f;
        farm_particle(f, p->x - p->facing * 14.0f, p->y - 4.0f, -p->vx * 0.16f, -14.0f, 0.34f, 9.0f,
                      farm_rgba(214, 198, 168, 150), 0);
    }
}

static void farm_feed(Farm *f, const Input *input) {
    if (!key_pressed(input, KEY_SPACE) && !key_pressed(input, KEY_E))
        return;

    FarmAnimal *best   = NULL;
    float       best_d = 118.0f * 118.0f;
    for (uint32_t i = 0; i < f->animal_count; i++) {
        FarmAnimal *a  = &f->animals[i];
        float       dx = a->x - f->player.x;
        float       dy = a->y - f->player.y;
        float       d  = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best   = a;
        }
    }
    if (!best)
        return;

    best->state   = FARM_HAPPY;
    best->state_t = 1.7f;
    best->facing  = best->x >= f->player.x ? 1.0f : -1.0f;
    f->fed++;

    for (uint32_t i = 0; i < 5; i++) {
        farm_particle(f, best->x + farm_rand_range(f, -18.0f, 18.0f), best->y - 34.0f - i * 6.0f,
                      farm_rand_range(f, -26.0f, 26.0f), farm_rand_range(f, -66.0f, -34.0f),
                      farm_rand_range(f, 0.7f, 1.15f), farm_rand_range(f, 13.0f, 17.0f), farm_rgba(255, 132, 178, 235),
                      1);
    }
}

/* ------------------------------------------------------------------ herd */

static void farm_animal_state(Farm *f, FarmAnimal *a) {
    float roll = farm_randf(f);

    if (roll < 0.46f) {
        float              angle = farm_rand_range(f, 0.0f, 6.2831853f);
        const FarmSpecies *sp    = &f->species[a->species];
        a->vx                    = cosf(angle) * sp->speed;
        a->vy                    = sinf(angle) * sp->speed * 0.62f;
        a->facing                = a->vx >= 0.0f ? 1.0f : -1.0f;
        a->state                 = FARM_WALK;
        a->state_t               = farm_rand_range(f, 0.9f, 3.4f);
    } else if (roll < 0.70f) {
        a->vx      = 0.0f;
        a->vy      = 0.0f;
        a->state   = FARM_IDLE;
        a->state_t = farm_rand_range(f, 0.7f, 2.6f);
    } else if (roll < 0.90f) {
        a->vx      = 0.0f;
        a->vy      = 0.0f;
        a->state   = FARM_EAT;
        a->state_t = farm_rand_range(f, 1.4f, 4.0f);
    } else {
        a->vx      = 0.0f;
        a->vy      = 0.0f;
        a->state   = FARM_SLEEP;
        a->state_t = farm_rand_range(f, 3.0f, 7.0f);
    }

    a->anim = 0.0f;
}

static void farm_animals_update(Farm *f, float dt) {
    const float lo_x = FARM_INSET, hi_x = FARM_WORLD_W - FARM_INSET;
    const float lo_y = FARM_INSET, hi_y = FARM_WORLD_H - FARM_INSET;

    for (uint32_t i = 0; i < f->animal_count; i++) {
        FarmAnimal *a = &f->animals[i];
        a->state_t -= dt;
        a->anim += dt;

        if (a->state == FARM_HAPPY) {
            if (a->state_t <= 0.0f)
                farm_animal_state(f, a);
            continue;
        }

        if (a->state == FARM_WALK) {
            a->x += a->vx * dt;
            a->y += a->vy * dt;

            if (a->x < lo_x || a->x > hi_x) {
                a->x      = clampf(a->x, lo_x, hi_x);
                a->vx     = -a->vx;
                a->facing = a->vx >= 0.0f ? 1.0f : -1.0f;
            }
            if (a->y < lo_y || a->y > hi_y) {
                a->y  = clampf(a->y, lo_y, hi_y);
                a->vy = -a->vy;
            }

            if (a->state_t <= 0.0f)
                farm_animal_state(f, a);
        } else if (a->state_t <= 0.0f) {
            farm_animal_state(f, a);
        }
    }
}

/* ---------------------------------------------------------------- camera */

static void farm_camera_update(Farm *f, float dt, float viewport_w, float viewport_h) {
    /* Look ahead along the dog's heading so the view opens up in the direction
       of travel instead of trailing behind it. */
    float target_x = f->player.x + f->player.vx * 0.22f - viewport_w * 0.5f;
    float target_y = f->player.y + f->player.vy * 0.22f - viewport_h * 0.5f;

    float max_x = FARM_WORLD_W - viewport_w;
    float max_y = FARM_WORLD_H - viewport_h;
    target_x    = clampf(target_x, 0.0f, max_x > 0.0f ? max_x : 0.0f);
    target_y    = clampf(target_y, 0.0f, max_y > 0.0f ? max_y : 0.0f);

    float k     = 1.0f - expf(-7.0f * dt);
    f->camera.x = lerpf(f->camera.x, target_x, k);
    f->camera.y = lerpf(f->camera.y, target_y, k);
}

/* --------------------------------------------------------------- drawing */

static float farm_aspect(const SpriteSystem *s, PictureId id) {
    const Picture *p = &s->pictures[id];
    return p->w ? (float)p->h / (float)p->w : 1.0f;
}

static void farm_draw_ground(Farm *f, SpriteSystem *s, float vw, float vh) {
    if (f->ground_count == 0)
        return;

    uint32_t x0 = (uint32_t)(f->camera.x / FARM_TILE);
    uint32_t y0 = (uint32_t)(f->camera.y / FARM_TILE);
    uint32_t x1 = (uint32_t)((f->camera.x + vw) / FARM_TILE) + 1u;
    uint32_t y1 = (uint32_t)((f->camera.y + vh) / FARM_TILE) + 1u;
    uint32_t nx = (uint32_t)(FARM_WORLD_W / FARM_TILE);
    uint32_t ny = (uint32_t)(FARM_WORLD_H / FARM_TILE);

    if (x1 > nx)
        x1 = nx;
    if (y1 > ny)
        y1 = ny;

    for (uint32_t gy = y0; gy < y1; gy++) {
        for (uint32_t gx = x0; gx < x1; gx++) {
            uint32_t  h    = farm_hash(gx * 73856093u ^ gy * 19349663u);
            uint32_t  pick = h % 100u < 14u && f->ground_count > 1u ? 1u : 0u;
            PictureId pic  = f->ground[pick];

            sprite_push(s, (SpriteDraw){
                               .picture = pic,
                               .x       = (float)gx * FARM_TILE,
                               .y       = (float)gy * FARM_TILE,
                               .w       = FARM_TILE,
                               .h       = FARM_TILE,
                               .layer   = FARM_LAYER_GROUND,
                               .blend   = SPRITE_BLEND_OPAQUE,
                               .sampler = SPRITE_SAMPLER_NEAREST_CLAMP,
                           });
        }
    }
}

static void farm_draw_decor(Farm *f, SpriteSystem *s, float vw, float vh) {
    const float cell = FARM_TILE;
    const float size = 96.0f;

    uint32_t x0 = (uint32_t)(f->camera.x / cell);
    uint32_t y0 = (uint32_t)(f->camera.y / cell);
    uint32_t x1 = (uint32_t)((f->camera.x + vw) / cell) + 1u;
    uint32_t y1 = (uint32_t)((f->camera.y + vh) / cell) + 1u;
    uint32_t nx = (uint32_t)(FARM_WORLD_W / cell);
    uint32_t ny = (uint32_t)(FARM_WORLD_H / cell);

    if (x1 > nx)
        x1 = nx;
    if (y1 > ny)
        y1 = ny;

    for (uint32_t gy = y0; gy < y1; gy++) {
        for (uint32_t gx = x0; gx < x1; gx++) {
            uint32_t h = farm_hash(gx * 374761393u ^ gy * 668265263u);
            if ((h & 7u) != 0u) /* 1 in 8 cells carries scenery */
                continue;

            uint32_t  pick = (h >> 8) % FARM_DECOR_COUNT;
            PictureId pic  = f->decor[pick];
            if (pic == UINT32_MAX)
                continue;

            float jx = (float)((h >> 16) & 63u) / 64.0f * (cell - size);
            float jy = (float)((h >> 22) & 63u) / 64.0f * (cell - size);

            sprite_push(s, (SpriteDraw){
                               .picture = pic,
                               .x       = (float)gx * cell + jx,
                               .y       = (float)gy * cell + jy,
                               .w       = size,
                               .h       = size,
                               .layer   = FARM_LAYER_DECOR,
                               .blend   = SPRITE_BLEND_ALPHA,
                           });
        }
    }
}

static void farm_draw_fence(Farm *f, SpriteSystem *s, float vw, float vh) {
    const float size = FARM_TILE;
    uint32_t    nx   = (uint32_t)(FARM_WORLD_W / size);
    uint32_t    ny   = (uint32_t)(FARM_WORLD_H / size);

    uint32_t x0 = (uint32_t)(f->camera.x / size);
    uint32_t x1 = (uint32_t)((f->camera.x + vw) / size) + 1u;
    uint32_t y0 = (uint32_t)(f->camera.y / size);
    uint32_t y1 = (uint32_t)((f->camera.y + vh) / size) + 1u;
    if (x1 > nx)
        x1 = nx;
    if (y1 > ny)
        y1 = ny;

    for (uint32_t gx = x0; gx < x1; gx++) {
        float fx = (float)gx * size;
        sprite_push(s, (SpriteDraw){.picture = f->fence,
                                    .x       = fx,
                                    .y       = 0.0f,
                                    .w       = size,
                                    .h       = size,
                                    .layer   = FARM_LAYER_FENCE,
                                    .blend   = SPRITE_BLEND_ALPHA});
        sprite_push(s, (SpriteDraw){.picture = f->fence,
                                    .x       = fx,
                                    .y       = FARM_WORLD_H - size,
                                    .w       = size,
                                    .h       = size,
                                    .layer   = FARM_LAYER_FENCE,
                                    .blend   = SPRITE_BLEND_ALPHA});
    }

    for (uint32_t gy = y0; gy < y1; gy++) {
        float fy = (float)gy * size;
        sprite_push(s, (SpriteDraw){.picture = f->fence,
                                    .x       = 0.0f,
                                    .y       = fy,
                                    .w       = size,
                                    .h       = size,
                                    .layer   = FARM_LAYER_FENCE,
                                    .blend   = SPRITE_BLEND_ALPHA});
        sprite_push(s, (SpriteDraw){.picture = f->fence,
                                    .x       = FARM_WORLD_W - size,
                                    .y       = fy,
                                    .w       = size,
                                    .h       = size,
                                    .layer   = FARM_LAYER_FENCE,
                                    .blend   = SPRITE_BLEND_ALPHA});
    }
}

/* Actors share one key, so the stable batch order is the painter order:
   lowest ground line first. The list is tiny, so an insertion sort beats
   anything with more machinery. */
typedef struct FarmDrawItem {
    float    y;
    uint32_t index; /* UINT32_MAX = the player */
} FarmDrawItem;

static void farm_push_shadow(Farm *f, SpriteSystem *s, float x, float y, float size) {
    if (f->shadow == UINT32_MAX)
        return;

    float w = size * 0.86f;
    float h = size * 0.34f;

    sprite_push(s, (SpriteDraw){.picture = f->shadow,
                                .x       = x - w * 0.5f,
                                .y       = y - h * 0.6f,
                                .w       = w,
                                .h       = h,
                                .layer   = FARM_LAYER_SHADOW,
                                .blend   = SPRITE_BLEND_ALPHA,
                                .color   = farm_rgba(0, 0, 0, 130)});
}

static void farm_push_actor(SpriteSystem *s, float x, float y, float facing, PictureId pic, float size,
                            float width_scale, float height_scale, float bob, uint32_t color) {
    float w = size * width_scale;
    float h = size * farm_aspect(s, pic) * height_scale;

    sprite_push(s, (SpriteDraw){.picture = pic,
                                .x       = x - w * 0.5f,
                                .y       = y - h - bob,
                                .w       = facing < 0.0f ? -w : w,
                                .h       = h,
                                .layer   = FARM_LAYER_ACTOR,
                                .blend   = SPRITE_BLEND_ALPHA,
                                .color   = color});
}

/* Frames per second of the pose cycle for each state. Grazing and idling are
   deliberately slow; a sleeping animal does not advance at all. */
static float farm_anim_fps(uint8_t state) {
    switch (state) {
    case FARM_WALK:
    case FARM_HAPPY:
        return 6.0f;
    case FARM_EAT:
        return 3.0f;
    case FARM_IDLE:
        return 1.6f;
    default:
        return 0.0f;
    }
}

static void farm_draw_actors(Farm *f, SpriteSystem *s) {
    FarmDrawItem order[FARM_MAX_ANIMALS + 1];
    uint32_t     n = 0;

    order[n].y     = f->player.y;
    order[n].index = UINT32_MAX;
    n++;
    for (uint32_t i = 0; i < f->animal_count; i++) {
        order[n].y     = f->animals[i].y;
        order[n].index = i;
        n++;
    }

    for (uint32_t i = 1; i < n; i++) {
        FarmDrawItem value = order[i];
        int32_t      j     = (int32_t)i - 1;
        while (j >= 0 && order[j].y > value.y) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = value;
    }

    const uint32_t white = farm_rgba(255, 255, 255, 255);

    for (uint32_t i = 0; i < n; i++) {
        if (order[i].index == UINT32_MAX) {
            const FarmPlayer *p   = &f->player;
            float             run = p->lean;
            float             bob = sinf(p->anim * 6.2831853f) * 3.0f * run;
            PictureId pic = f->player_frame_count ? f->player_frames[(uint32_t)(p->anim * 2.0f) % f->player_frame_count]
                                                  : UINT32_MAX;
            if (pic == UINT32_MAX)
                continue;

            farm_push_shadow(f, s, p->x, p->y, 74.0f);
            farm_push_actor(s, p->x, p->y, p->facing, pic, 74.0f, 1.0f + 0.10f * run, 1.0f - 0.07f * run, bob, white);
            continue;
        }

        const FarmAnimal  *a  = &f->animals[order[i].index];
        const FarmSpecies *sp = &f->species[a->species];

        PictureId pic;
        if (a->state == FARM_SLEEP && sp->sleep != UINT32_MAX) {
            pic = sp->sleep;
        } else {
            float fps = farm_anim_fps(a->state);
            pic       = sp->frames[(uint32_t)(a->anim * fps) % sp->frame_count];
        }

        float bob = 0.0f;
        if (a->state == FARM_WALK)
            bob = fabsf(sinf(a->anim * 7.0f)) * 3.0f;
        else if (a->state == FARM_HAPPY)
            bob = fabsf(sinf(a->anim * 9.0f)) * 9.0f;

        float squash = a->state == FARM_HAPPY ? 1.0f - 0.10f * fabsf(sinf(a->anim * 9.0f)) : 1.0f;
        float sink   = a->state == FARM_SLEEP ? 0.88f : 1.0f;

        farm_push_shadow(f, s, a->x, a->y, sp->size);
        farm_push_actor(s, a->x, a->y, a->facing, pic, sp->size, 1.0f, squash * sink, bob, white);
    }
}

static void farm_draw_particles(Farm *f, SpriteSystem *s) {
    for (uint32_t i = 0; i < f->particle_count; i++) {
        const FarmParticle *p   = &f->particles[i];
        PictureId           pic = p->kind == 1 ? f->heart : f->shadow;
        if (pic == UINT32_MAX)
            continue;

        float u    = p->t / p->life;
        float size = p->kind == 1 ? p->size * (1.0f - 0.30f * u) : p->size * (1.0f + 1.5f * u);

        uint32_t alpha = (p->color >> 24) & 0xFFu;
        alpha          = (uint32_t)((float)alpha * (1.0f - u));
        uint32_t color = (p->color & 0x00FFFFFFu) | (alpha << 24);

        sprite_push(s, (SpriteDraw){.picture = pic,
                                    .x       = p->x - size * 0.5f,
                                    .y       = p->y - size * farm_aspect(s, pic) * 0.5f,
                                    .w       = size,
                                    .h       = size * farm_aspect(s, pic),
                                    .layer   = FARM_LAYER_FX,
                                    .blend   = SPRITE_BLEND_ALPHA,
                                    .color   = color});
    }
}

/* GameHooks.frame: runs simulation, then emits this frame's instances. */
static void farm_update(void *user, const GameFrame *frame) {
    Farm         *f          = (Farm *)user;
    SpriteSystem *sprites    = frame->sprites;
    const Input  *input      = frame->input;
    float         dt         = frame->dt;
    uint32_t      viewport_w = frame->viewport_w;
    uint32_t      viewport_h = frame->viewport_h;

    if (dt > 0.1f)
        dt = 0.1f;

    bool live = viewport_w != 0 && viewport_h != 0 && f->species_count != 0;

    if (live) {
        farm_player_update(f, input, dt);
        farm_feed(f, input);
        farm_animals_update(f, dt);
        farm_particles_update(f, dt);
        farm_camera_update(f, dt, (float)viewport_w, (float)viewport_h);
    }

    sprite_begin(sprites, &f->camera);

    renderer_hud(frame->renderer, "camera %.0f, %.0f   zoom %.2f", f->camera.x, f->camera.y, f->camera.zoom);
    renderer_hud(frame->renderer, "%u animals   fed %u   particles %u", f->animal_count, f->fed, f->particle_count);

    if (!live)
        return;

    float vw = (float)viewport_w;
    float vh = (float)viewport_h;

    farm_draw_ground(f, sprites, vw, vh);
    farm_draw_decor(f, sprites, vw, vh);
    farm_draw_fence(f, sprites, vw, vh);
    farm_draw_actors(f, sprites);
    farm_draw_particles(f, sprites);
}

/* ================================================================== boot */

int main(int argc, char **argv) {
    bool use_wayland = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "wayland") == 0 || strcmp(argv[i], "--wayland") == 0)
            use_wayland = true;
        else if (strcmp(argv[i], "x11") == 0 || strcmp(argv[i], "--x11") == 0)
            use_wayland = false;
        else {
            fprintf(stderr, "Usage: %s [x11|wayland]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    GameHooks game     = {.user = &g_farm, .start = farm_init, .frame = farm_update};
    Renderer *renderer = renderer_create(use_wayland, game);
    if (!renderer)
        return EXIT_FAILURE;
    while (renderer_frame(renderer)) {
    }
    renderer_destroy(renderer);
    return EXIT_SUCCESS;
}
