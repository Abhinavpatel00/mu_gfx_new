#include "src/renderer.h"

#include "src/input.h"
#include "src/three_d/scene.h"

#include "src/two_d/sprite.h" /* the farm demo below is the 2D stage's client */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* =========================================================== 3D: cubepets
 *
 * Stress scene replacing the farm: real cooked assets in a grid, under a GPU
 * frustum cull, drawn with one indexed indirect draw per (mesh, lod) group.
 * The camera is a mouse/keyboard flycam. */

/* Room scene: static interior built from cooked KayKit pieces, not the old
   character grid. The compaction comment that lived here went with it. */

/* Meadow test scene: an outdoor clearing cooked out of the nature packs by
   scripts/cook_meadow_assets.sh. Indices match that roster, and the table is
   ordered so a species and its slot can be read together. */
enum {
    A_GROUND,
    A_GROUND_ROCKY,
    A_TREE_COMMON_A,
    A_TREE_COMMON_B,
    A_TREE_COMMON_C,
    A_PINE_A,
    A_PINE_B,
    A_TREE_TWISTED_A,
    A_TREE_TWISTED_B,
    A_TREE_BIRCH_A,
    A_TREE_BIRCH_B,
    A_TREE_OAK_A,
    A_TREE_OAK_B,
    A_TREE_BARE_A,
    A_BUSH_A,
    A_BUSH_FLOWERS,
    A_BUSH_B,
    A_BUSH_C,
    A_GRASS_TALL,
    A_GRASS_WISPY,
    A_GRASS_B,
    A_GRASS_C,
    A_ROCK_A,
    A_ROCK_B,
    A_ROCK_C,
    A_PEBBLE_A,
    A_ROCKPATH,
    A_MUSHROOM,
    A_FERN,
    A_FLOWER_GROUP,
    A_CLOVER,
    A_COUNT
};
static const char *const pets_assets[A_COUNT] = {
    [A_GROUND]         = "assets/md_ground.mua",
    [A_GROUND_ROCKY]   = "assets/md_ground_rocky.mua",
    [A_TREE_COMMON_A]  = "assets/md_tree_common_a.mua",
    [A_TREE_COMMON_B]  = "assets/md_tree_common_b.mua",
    [A_TREE_COMMON_C]  = "assets/md_tree_common_c.mua",
    [A_PINE_A]         = "assets/md_pine_a.mua",
    [A_PINE_B]         = "assets/md_pine_b.mua",
    [A_TREE_TWISTED_A] = "assets/md_tree_twisted_a.mua",
    [A_TREE_TWISTED_B] = "assets/md_tree_twisted_b.mua",
    [A_TREE_BIRCH_A]   = "assets/md_tree_birch_a.mua",
    [A_TREE_BIRCH_B]   = "assets/md_tree_birch_b.mua",
    [A_TREE_OAK_A]     = "assets/md_tree_oak_a.mua",
    [A_TREE_OAK_B]     = "assets/md_tree_oak_b.mua",
    [A_TREE_BARE_A]    = "assets/md_tree_bare_a.mua",
    [A_BUSH_A]         = "assets/md_bush_a.mua",
    [A_BUSH_FLOWERS]   = "assets/md_bush_flowers.mua",
    [A_BUSH_B]         = "assets/md_bush_b.mua",
    [A_BUSH_C]         = "assets/md_bush_c.mua",
    [A_GRASS_TALL]     = "assets/md_grass_tall.mua",
    [A_GRASS_WISPY]    = "assets/md_grass_wispy.mua",
    [A_GRASS_B]        = "assets/md_grass_b.mua",
    [A_GRASS_C]        = "assets/md_grass_c.mua",
    [A_ROCK_A]         = "assets/md_rock_a.mua",
    [A_ROCK_B]         = "assets/md_rock_b.mua",
    [A_ROCK_C]         = "assets/md_rock_c.mua",
    [A_PEBBLE_A]       = "assets/md_pebble_a.mua",
    [A_ROCKPATH]       = "assets/md_rockpath.mua",
    [A_MUSHROOM]       = "assets/md_mushroom.mua",
    [A_FERN]           = "assets/md_fern.mua",
    [A_FLOWER_GROUP]   = "assets/md_flower_group.mua",
    [A_CLOVER]         = "assets/md_clover.mua",
};

/* Ground tiles and the path that crosses them. The tile is 4x4, so spacing is
   the tile size: any other number opens seams at the joins. */
#define MEADOW_TILES      20
#define MEADOW_TILE_STEP  4.0f
#define MEADOW_PATH_STEPS 24
/* Room left for props the game will move; the region is a fixed slot range, so
   a scene with none still pays for its ceiling and nothing else. The slime
   crowd is 2 instances per slime (body, SDF face quad) x 12. */
#define MEADOW_MOVING     24

/* A species' planting rule. Radial bands rather than a hand-placed list: a
   couple of thousand props is past the point a table stays readable, and a
   scatter retunes with one number instead of a text edit. min_gap is the
   rejection distance against everything planted before it; 0 lets a species
   overlap, which is what ground cover should do. */
typedef struct Scatter {
    uint32_t asset;
    uint32_t count;
    float    r_min, r_max; /* annulus around the clearing centre */
    float    s_min, s_max; /* uniform scale band */
    float    min_gap;
} Scatter;

/* Trees and their seedings, then rocks, then bushes, then ground cover. Order
   is load-bearing: the rejection list is filled as this is walked, so anything
   that must stand clear of a trunk has to be planted after one. */
static const Scatter meadow_scatter[] = {
    /* --- trees: the clearing's wall --------------------------------------- */
    {A_TREE_COMMON_A, 16, 12.0f, 34.0f, 0.85f, 1.15f, 6.0f},
    {A_TREE_COMMON_B, 14, 12.0f, 34.0f, 0.85f, 1.15f, 6.0f},
    {A_TREE_COMMON_C, 14, 12.0f, 34.0f, 0.80f, 1.10f, 6.0f},
    {A_PINE_A, 20, 14.0f, 36.0f, 0.85f, 1.20f, 5.5f},
    {A_PINE_B, 20, 14.0f, 36.0f, 0.85f, 1.20f, 5.5f},
    /* The twisted pair are the 16m ancients: kept large but rare and off centre. */
    {A_TREE_TWISTED_A, 5, 17.0f, 32.0f, 0.50f, 0.70f, 9.0f},
    {A_TREE_TWISTED_B, 5, 17.0f, 32.0f, 0.50f, 0.70f, 9.0f},
    {A_TREE_BIRCH_A, 22, 11.0f, 34.0f, 0.90f, 1.30f, 4.5f},
    {A_TREE_BIRCH_B, 20, 11.0f, 34.0f, 0.90f, 1.30f, 4.5f},
    {A_TREE_OAK_A, 18, 12.0f, 34.0f, 0.90f, 1.25f, 5.0f},
    {A_TREE_OAK_B, 16, 12.0f, 34.0f, 0.90f, 1.25f, 5.0f},
    {A_TREE_BARE_A, 9, 13.0f, 33.0f, 1.10f, 1.60f, 6.0f},
    /* --- rocks ------------------------------------------------------------ */
    {A_ROCK_A, 12, 6.0f, 30.0f, 0.70f, 1.20f, 5.0f},
    {A_ROCK_B, 12, 6.0f, 30.0f, 0.70f, 1.20f, 5.0f},
    {A_ROCK_C, 26, 4.0f, 32.0f, 1.20f, 2.40f, 2.5f},
    /* --- bushes ----------------------------------------------------------- */
    {A_BUSH_A, 34, 5.0f, 30.0f, 0.80f, 1.30f, 2.5f},
    {A_BUSH_FLOWERS, 22, 5.0f, 28.0f, 0.80f, 1.30f, 2.5f},
    {A_BUSH_B, 40, 4.0f, 30.0f, 1.00f, 1.80f, 0.0f},
    {A_BUSH_C, 34, 4.0f, 30.0f, 1.20f, 2.20f, 0.0f},
    /* --- ground cover: dense, and free to overlap itself ------------------ */
    {A_GRASS_TALL, 260, 2.0f, 34.0f, 0.70f, 1.40f, 0.0f},
    {A_GRASS_WISPY, 220, 2.0f, 34.0f, 0.70f, 1.40f, 0.0f},
    {A_GRASS_B, 300, 1.5f, 34.0f, 1.40f, 3.00f, 0.0f},
    {A_GRASS_C, 260, 1.5f, 34.0f, 1.20f, 2.60f, 0.0f},
    {A_PEBBLE_A, 60, 2.0f, 30.0f, 0.80f, 1.60f, 0.0f},
    {A_CLOVER, 90, 2.0f, 28.0f, 0.80f, 1.40f, 0.0f},
    {A_MUSHROOM, 44, 3.0f, 26.0f, 0.90f, 1.60f, 0.0f},
    {A_FERN, 18, 6.0f, 28.0f, 0.35f, 0.60f, 3.0f},
    {A_FLOWER_GROUP, 46, 2.0f, 26.0f, 0.80f, 1.30f, 0.0f},
};
#define MEADOW_SCATTER_COUNT (sizeof(meadow_scatter) / sizeof(meadow_scatter[0]))

/* Cooked .mua room pieces (see the table above). Node transforms are baked
   into the vertices, so a model is a group of meshes in one space: every mesh
   of a piece gets its own instance at the same transform. */

/* Flycam state. Position is authoritative and freely movable; yaw/pitch are a
   derived view direction built from it, not a separate orbit around a pivot.
   Speed is exponential in the key's hold time so a tap is a nudge and a hold
   crosses the scene, without needing an acceleration curve or a tuning knob. */
typedef struct FlyCam {
    vec3  pos;     /* eye position, authoritative */
    vec3  forward; /* derived from yaw/pitch each frame */
    vec3  right;
    vec3  up;
    float yaw;
    float pitch;
    float speed;
    float dt;
} FlyCam;

typedef struct CubePets {
    VkBackend *vk;
    Scene     *scene;
    void      *renderer;

    FlyCam cam;
    float  yaw;
    float  dist;
    float  dt;
} CubePets;

static CubePets g_pets;

/* Deterministic xorshift. A layout that reshuffles every launch cannot be
   compared against a screenshot, and this is the whole cost of that property. */
static uint32_t g_seed = 0x9E3779B9u;
static float    frand(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return (float)(g_seed >> 8) * (1.0f / 16777216.0f);
}
static float frange(float lo, float hi) { return lo + (hi - lo) * frand(); }

/* XZ of everything planted with a min_gap, as flat pairs. A few hundred entries
   makes the linear scan cheaper than a grid would cost to build and update. */
#define MEADOW_PLACED_MAX 1024
static float    g_placed_xz[MEADOW_PLACED_MAX * 2];
static uint32_t g_placed_count;

static bool meadow_clear(float x, float z, float gap) {
    if (gap <= 0.0f)
        return true; /* ground cover is allowed to grow through itself */
    for (uint32_t i = 0; i < g_placed_count; ++i) {
        float dx = x - g_placed_xz[i * 2 + 0];
        float dz = z - g_placed_xz[i * 2 + 1];
        if (dx * dx + dz * dz < gap * gap)
            return false;
    }
    return true;
}

static void meadow_record(float x, float z) {
    if (g_placed_count >= MEADOW_PLACED_MAX)
        return;
    g_placed_xz[g_placed_count * 2 + 0] = x;
    g_placed_xz[g_placed_count * 2 + 1] = z;
    g_placed_count++;
}

/* A prop is a group of meshes in one space, so every mesh of the asset gets an
   instance at the same transform. */
static void plant(Scene *scene, const SceneAssetLoad *ld, float x, float y, float z, float yaw, float scale) {
    float             half = yaw * 0.5f;
    SceneInstanceDesc d    = {.pos = {x, y, z}, .quat = {0.0f, sinf(half), 0.0f, cosf(half)}, .scale = scale};
    for (uint32_t i = 0; i < ld->mesh_count; ++i) {
        d.mesh = ld->first_mesh + i;
        scene_instance_create_static(scene, &d);
    }
}

/* The instance total the scene must be created with, derived from the same two
   tables the builder walks so the capacity and the planting cannot disagree. */
static uint32_t pets_instance_total(const uint32_t asset_meshes[A_COUNT]) {
    uint32_t total = MEADOW_TILES * MEADOW_TILES * asset_meshes[A_GROUND];
    for (uint32_t i = 0; i < MEADOW_SCATTER_COUNT; ++i)
        total += meadow_scatter[i].count * asset_meshes[meadow_scatter[i].asset];
    return total + MEADOW_PATH_STEPS * asset_meshes[A_ROCKPATH];
}

/* Loads the roster, then plants the clearing.

   Each prop contributes SceneAssetLoad.mesh_count instances — a prop is a group
   of meshes in one space, not one model — so the instance total is a
   mesh-weighted sum, not a prop count. Returns the meshes registered, or
   UINT32_MAX on a hard load failure; the caller has already created the scene
   with matching capacities and cannot proceed without them. */
static uint32_t pets_build_assets(Scene *scene) {
    uint32_t       total_meshes = 0;
    SceneAssetLoad loads[A_COUNT];
    for (uint32_t m = 0; m < A_COUNT; ++m) {
        char err[256];
        if (!scene_asset_load(scene, pets_assets[m], &loads[m], err, sizeof(err))) {
            log_error("[pets] %s: %s", pets_assets[m], err);
            return UINT32_MAX;
        }
        log_info("[pets] %s: %u meshes, %u verts, %u indices, %u rungs, radius %.2f", pets_assets[m],
                 loads[m].mesh_count, loads[m].vertex_count, loads[m].index_count, loads[m].lod_count,
                 loads[m].radius);
        total_meshes += loads[m].mesh_count;
    }

    g_seed         = 0x9E3779B9u;
    g_placed_count = 0;
    uint32_t planted = 0;

    /* Ground: one tile per cell at exactly the tile pitch — any other spacing
       opens seams at the joins. Every fifth cell takes the rocky variant, a
       deterministic wear pattern so the same tiles are worn every run. */
    for (uint32_t z = 0; z < MEADOW_TILES; ++z) {
        for (uint32_t x = 0; x < MEADOW_TILES; ++x) {
            float    px    = ((float)x - (float)(MEADOW_TILES - 1) * 0.5f) * MEADOW_TILE_STEP;
            float    pz    = ((float)z - (float)(MEADOW_TILES - 1) * 0.5f) * MEADOW_TILE_STEP;
            uint32_t asset = ((x * 7u + z * 13u) % 5u == 0u) ? A_GROUND_ROCKY : A_GROUND;
            plant(scene, &loads[asset], px, 0.0f, pz, 0.0f, 1.0f);
            planted++;
        }
    }

    /* Species, in table order. Drawing the radius from the band's squared range
       keeps the density uniform over the annulus instead of piling up middle. */
    for (uint32_t s = 0; s < MEADOW_SCATTER_COUNT; ++s) {
        const Scatter        *sp     = &meadow_scatter[s];
        const SceneAssetLoad *ld     = &loads[sp->asset];
        uint32_t              placed = 0;
        for (uint32_t tries = 0; tries < sp->count * 32u && placed < sp->count; ++tries) {
            float ang = frand() * 6.2831853f;
            float rad = sqrtf(frange(sp->r_min * sp->r_min, sp->r_max * sp->r_max));
            float x   = cosf(ang) * rad;
            float z   = sinf(ang) * rad;
            if (!meadow_clear(x, z, sp->min_gap))
                continue;
            if (sp->min_gap > 0.0f)
                meadow_record(x, z);
            plant(scene, ld, x, 0.0f, z, frand() * 6.2831853f, frange(sp->s_min, sp->s_max));
            planted++;
            placed++;
        }
        if (placed < sp->count)
            log_info("[pets] %s: placed %u of %u", pets_assets[sp->asset], placed, sp->count);
    }

    /* A worn path from the spawn into the trees, wobbling as it goes. */
    for (uint32_t i = 0; i < MEADOW_PATH_STEPS; ++i) {
        float t = (float)i / (float)(MEADOW_PATH_STEPS - 1u);
        plant(scene, &loads[A_ROCKPATH], sinf(t * 5.0f) * 1.4f, 0.02f, 8.0f - t * 36.0f, t * 3.0f,
              frange(0.9f, 1.15f));
        planted++;
    }

    log_info("[pets] %u meshes registered, %u props planted", total_meshes, planted);
    return total_meshes;
}

/* ---- slime crowd ----------------------------------------------------------- */
/* Twelve expressive gel slimes that bounce instead of walk: each one is a
   dynamic instance pair (body + SDF face quad) whose pos/scale the CPU
   rewrites every frame from a cheap hop integrator, while the GPU adds idle
   jiggle plus air/land squash. Rounded procedural body: squashed UV-sphere
   with stubby horn/ear cones, gel material, vibrant base colors. The face is
   one Y-billboarded quad sampling the shared SDF face atlas (see
   kSlimeFaceFiles); the per-instance face_id tile changes only on expression
   change, never per frame. Placeholder tiles are the Slime Rancher rips in
   assets/slime_faces/ — see TODO(faces) in todo.txt. */

#define SLIME_COUNT 12u
#define SLIME_GRAV  14.0f /* world units / s^2: snappy, cartoon falls */
#define SLIME_GROUND_Y 0.0f

/* Lat/long UV-sphere: 2 poles + (SLIME_RING-1) rings of SLIME_SEG verts. */
#define SLIME_SEG   16u
#define SLIME_RING   9u
/* One sphere of seg x ring divisions: 2 poles + (ring-1) interior rows. */
#define SPHERE_VERTS(seg, ring) (2u + ((ring) - 1u) * (seg))
#define SPHERE_IDXS(seg, ring)  ((seg) * 3u + ((ring) - 2u) * (seg) * 6u + (seg) * 3u)
#define SLIME_VERTS SPHERE_VERTS(SLIME_SEG, SLIME_RING)
#define SLIME_INDXS SPHERE_IDXS(SLIME_SEG, SLIME_RING)

/* Face atlas roster, row-major into the 8x5 grid. Home-cooked SDF faces
   (scripts/author_faces.py): standard polarity on every channel, R = eyes,
   A = mouth, B = eye whites, G = eye highlight ramp. */
static const char *const kSlimeFaceFiles[] = {
    "assets/slime_faces_ours/face_mood_happy.png",  /* 0 FACE_HAPPY */
    "assets/slime_faces_ours/face_mood_blink.png",  /* 1 FACE_BLINK */
    "assets/slime_faces_ours/face_mood_elated.png", /* 2 FACE_ELATED */
    "assets/slime_faces_ours/face_mood_scared.png", /* 3 FACE_SCARED */
    "assets/slime_faces_ours/face_mood_hungry.png", /* 4 FACE_HUNGRY */
    "assets/slime_faces_ours/face_mood_sad.png",    /* 5 FACE_SAD */
    "assets/slime_faces_ours/face_mood_angry.png",  /* 6 FACE_ANGRY */
    "assets/slime_faces_ours/face_mood_sleepy.png", /* 7 FACE_SLEEPY */
};
#define SLIME_FACE_COUNT (sizeof(kSlimeFaceFiles) / sizeof(kSlimeFaceFiles[0]))

typedef enum SlimeFace {
    FACE_HAPPY  = 0,
    FACE_BLINK  = 1,
    FACE_ELATED = 2,
    FACE_SCARED = 3,
    FACE_HUNGRY = 4,
    FACE_SAD    = 5,
    FACE_ANGRY  = 6,
    FACE_SLEEPY = 7,
} SlimeFace;

typedef struct Slime {
    uint32_t slot;      /* body instance slot (dynamic) */
    uint32_t slot_face; /* face quad instance: shares the body transform */
    uint32_t tint;      /* 0..2: body mesh/material variant */
    uint32_t face_id;   /* atlas tile; written to the face table on change */
    float    home[2];   /* wander anchor (x, z) */
    float    pos[3];    /* contact point (bottom) in world space */
    float    vel[3];    /* hop velocity while airborne */
    float    scale;     /* uniform body scale */
    float    dwell;     /* seconds on the ground until the next hop */
    float    blink;     /* seconds until the next blink starts */
    float    blink_len; /* remaining blink time; 0 = eyes open */
    float    land_t;    /* remaining landing-reaction time; 0 = none */
    bool     land_hard; /* landing was fast: show scared, else happy */
    float    event_t;   /* remaining random-mood time; 0 = none */
    uint32_t event_id;  /* FACE_SAD or FACE_ANGRY while event_t runs */
    float    yaw;       /* facing, smoothed toward travel */
    float    yaw_tgt;
    float    squash;    /* current vertical squash, 1 = rest */
    bool     air;       /* true while the hop arc is above ground */
} Slime;

typedef struct SlimeCrowd {
    Slime    slimes[SLIME_COUNT];
    uint32_t body_mesh[3]; /* one per tint: a mesh owns one material */
    uint32_t face_mesh;    /* shared SDF face quad */
    uint32_t mat_body[3];
    uint32_t mat_face;
    uint32_t face_atlas;   /* bindless texture id */
} SlimeCrowd;

static SlimeCrowd g_slimes;

/* Vibrant gel base colors, opaque. */
static const uint32_t kSlimeTint[3] = {
    240u | (120u << 8) | (190u << 16) | (255u << 24), /* berry pink  */
    130u | (230u << 8) | (170u << 16) | (255u << 24), /* mint        */
    255u | (160u << 8) | ( 90u << 16) | (255u << 24), /* tangerine   */
};

static void slime_pack(struct ScenePackedVertex *out, float x, float y, float z, float u, float v) {
    float inv = 1.0f / sqrtf(x * x + y * y + z * z);
    float pos[3] = {x, y, z};
    float nrm[3] = {x * inv, y * inv, z * inv};
    float uv[2]  = {u, v};
    scene_pack_vertex(out, pos, nrm, uv);
}

/* Appends a UV-sphere centred at (cx,cy,cz) with the same pole/ring layout the
   body builder uses. Normals radiate from the sphere's own centre, so features
   (ears, eyes, smile, glints) shade as round solids rather than as bumps on the
   body. *nv/*ni advance; idxs are relative to the array base. */
static void slime_sphere(struct ScenePackedVertex *verts, uint16_t *idxs, uint32_t *nv, uint32_t *ni, float cx, float cy,
                         float cz, float r, uint32_t seg, uint32_t ring) {
    uint32_t b = *nv, k = *ni;

    uint32_t top = b;
    ++b;
    for (uint32_t ri = 1; ri < ring; ++ri) {
        float theta = 3.14159265f * (float)ri / (float)ring;
        float py    = cosf(theta) * r;
        float pr    = sinf(theta) * r;
        for (uint32_t s = 0; s < seg; ++s) {
            float phi = 6.2831853f * (float)s / (float)seg;
            float dx  = cosf(phi) * pr, dy = py, dz = sinf(phi) * pr;
            float inv = 1.0f / sqrtf(dx * dx + dy * dy + dz * dz);
            float pos[3] = {cx + dx, cy + dy, cz + dz};
            float nrm[3] = {dx * inv, dy * inv, dz * inv};
            float uv[2]  = {(float)s / (float)seg, (float)ri / (float)ring};
            scene_pack_vertex(&verts[b++], pos, nrm, uv);
        }
    }
    uint32_t bot = b;
    ++b;

    for (uint32_t s = 0; s < seg; ++s) {
        idxs[k++] = (uint16_t)top;
        idxs[k++] = (uint16_t)(top + 1u + s);
        idxs[k++] = (uint16_t)(top + 1u + (s + 1u) % seg);
    }
    for (uint32_t ri = 1; ri + 1u < ring; ++ri) {
        uint32_t a0 = top + 1u + (ri - 1u) * seg;
        uint32_t b0 = top + 1u + ri * seg;
        for (uint32_t s = 0; s < seg; ++s) {
            uint32_t sn = (s + 1u) % seg;
            idxs[k++] = (uint16_t)(a0 + s);
            idxs[k++] = (uint16_t)(b0 + s);
            idxs[k++] = (uint16_t)(b0 + sn);
            idxs[k++] = (uint16_t)(a0 + s);
            idxs[k++] = (uint16_t)(b0 + sn);
            idxs[k++] = (uint16_t)(a0 + sn);
        }
    }
    uint32_t ring0 = top + 1u + (ring - 2u) * seg;
    for (uint32_t s = 0; s < seg; ++s) {
        idxs[k++] = (uint16_t)bot;
        idxs[k++] = (uint16_t)(ring0 + (s + 1u) % seg);
        idxs[k++] = (uint16_t)(ring0 + s);
    }

    *nv = b;
    *ni = k;
}

/* Stages the slime crowd: three tinted body meshes (sphere + ear nubs), the
   shared SDF face quad and its atlas material, and 12 dynamic instance
   pairs. Returns false on failure. Runs before scene_upload_scene. */
static bool slime_build(Scene *scene) {
    const float radius = 1.0f, squash = 0.8f;

    static struct ScenePackedVertex verts[SLIME_VERTS + 2u * SPHERE_VERTS(10u, 6u)];
    static uint16_t                  idxs[SLIME_INDXS + 2u * SPHERE_IDXS(10u, 6u)];

    uint32_t n = 0;
    slime_pack(&verts[n++], 0.0f, radius * squash, 0.0f, 0.5f, 0.0f); /* top pole */
    for (uint32_t r = 1; r < SLIME_RING; ++r) {
        float theta = 3.14159265f * (float)r / (float)SLIME_RING;
        float ry    = cosf(theta) * radius * squash;
        float rr    = sinf(theta) * radius;
        for (uint32_t s = 0; s < SLIME_SEG; ++s) {
            float phi = 6.2831853f * (float)s / (float)SLIME_SEG;
            slime_pack(&verts[n++], cosf(phi) * rr, ry, sinf(phi) * rr, (float)s / SLIME_SEG,
                       (float)r / SLIME_RING);
        }
    }
    slime_pack(&verts[n++], 0.0f, -radius * squash, 0.0f, 0.5f, 1.0f); /* bottom pole */
    uint32_t bottom = SLIME_VERTS - 1u;

    uint32_t k = 0;
    /* top cap */
    for (uint32_t s = 0; s < SLIME_SEG; ++s) {
        idxs[k++] = 0u;
        idxs[k++] = (uint16_t)(1u + s);
        idxs[k++] = (uint16_t)(1u + (s + 1u) % SLIME_SEG);
    }
    /* middle quads */
    for (uint32_t r = 1; r + 1u < SLIME_RING; ++r) {
        uint32_t a0 = 1u + (r - 1u) * SLIME_SEG;
        uint32_t b0 = 1u + r * SLIME_SEG;
        for (uint32_t s = 0; s < SLIME_SEG; ++s) {
            uint32_t sn = (s + 1u) % SLIME_SEG;
            idxs[k++] = (uint16_t)(a0 + s);
            idxs[k++] = (uint16_t)(b0 + s);
            idxs[k++] = (uint16_t)(b0 + sn);
            idxs[k++] = (uint16_t)(a0 + s);
            idxs[k++] = (uint16_t)(b0 + sn);
            idxs[k++] = (uint16_t)(a0 + sn);
        }
    }
    /* bottom cap */
    {
        uint32_t ring = 1u + (SLIME_RING - 2u) * SLIME_SEG;
        for (uint32_t s = 0; s < SLIME_SEG; ++s) {
            idxs[k++] = (uint16_t)bottom;
            idxs[k++] = (uint16_t)(ring + (s + 1u) % SLIME_SEG);
            idxs[k++] = (uint16_t)(ring + s);
        }
    }

    /* Ear nubs: small spheres half-buried in the crown, sharing the body
       material so they take the tint, the jiggle and the squash. */
    slime_sphere(verts, idxs, &n, &k, -0.47f, 0.70f, 0.0f, 0.19f, 10u, 6u);
    slime_sphere(verts, idxs, &n, &k, 0.47f, 0.70f, 0.0f, 0.19f, 10u, 6u);

    SceneLodDesc body_lod = {.first_vertex = 0, .first_index = 0, .index_count = k, .error = 1.0e9f};
    SceneMeshDesc md = {.vertices     = verts,
                        .vertex_count = n,
                        .indices      = idxs,
                        .index_count  = k,
                        .lods         = &body_lod,
                        .lod_count    = 1,
                        .local_center = {0.0f, 0.0f, 0.0f},
                        .local_radius = radius,
                        .material     = 0};

    /* One mesh owns one material, so the three tints are three meshes over the
       same staged geometry. */
    for (uint32_t t = 0; t < 3u; ++t) {
        struct SceneGpuMaterial mat = {0};
        mat.base_color     = kSlimeTint[t];
        mat.texture        = 0xFFFFFFFFu;
        mat.normal_texture = 0xFFFFFFFFu;
        mat.orm_texture    = 0xFFFFFFFFu;
        mat.flags          = SCENE_MAT_GEL;
        mat.roughness      = 0.25f; /* wet: tight, sharp specular */
        uint32_t mi = scene_material_add(scene, &mat);
        if (mi == UINT32_MAX)
            return false;
        md.material           = mi;
        uint32_t mesh         = scene_mesh_add(scene, &md);
        if (mesh == UINT32_MAX)
            return false;
        g_slimes.mat_body[t]  = mi;
        g_slimes.body_mesh[t] = mesh;
    }

    /* SDF face atlas: placeholder rips resampled into one 8x5 texture. Linear
       UNORM (never sRGB): the channels carry distance, not color. */
    char face_err[256];
    g_slimes.face_atlas = scene_face_atlas_load(scene, kSlimeFaceFiles, SLIME_FACE_COUNT, face_err, sizeof(face_err));
    if (g_slimes.face_atlas == UINT32_MAX) {
        log_error("[slime] %s", face_err);
        return false;
    }

    /* Face quad: one unit quad in the XY plane, +/-0.5 in x/y. The VS pins it
       to the body's front (+Z in instance space) like a decal, so it shares
       the body yaw, squash and jiggle; authored positions only set the size
       ratio the shader scales. Normals face +Z. */
    static struct ScenePackedVertex qverts[4];
    static uint16_t                  qidxs[6] = {0, 1, 2, 0, 2, 3};
    {
        float nrm[3] = {0.0f, 0.0f, 1.0f};
        float p[4][3] = {{-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}, {-0.5f, 0.5f, 0.0f}};
        /* Vulkan v=0 is the image top row: top verts (+0.5) take v=0 so the
           face top (eyes) samples the tile top. */
        float t[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
        for (uint32_t v = 0; v < 4u; ++v)
            scene_pack_vertex(&qverts[v], p[v], nrm, t[v]);
    }
    struct SceneGpuMaterial face_mat = {0};
    face_mat.base_color              = 255u | (255u << 8) | (255u << 16) | (255u << 24);
    face_mat.texture                 = g_slimes.face_atlas;
    face_mat.normal_texture          = 0xFFFFFFFFu;
    face_mat.orm_texture             = 0xFFFFFFFFu;
    face_mat.flags                   = SCENE_MAT_FACE | SCENE_MAT_HAS_ALBEDO;
    uint32_t fmi                     = scene_material_add(scene, &face_mat);
    if (fmi == UINT32_MAX)
        return false;
    SceneLodDesc face_lod = {.first_vertex = 0, .first_index = 0, .index_count = 6, .error = 1.0e9f};
    /* Cull sphere covers the face quad wherever the body yaws: centre
       at face height with a radius past the quad's reach. */
    g_slimes.face_mesh = scene_mesh_add(scene, &(SceneMeshDesc){.vertices     = qverts,
                                                                 .vertex_count = 4,
                                                                 .indices      = qidxs,
                                                                 .index_count  = 6,
                                                                 .lods         = &face_lod,
                                                                 .lod_count    = 1,
                                                                 .local_center = {0.0f, 0.12f, 0.0f},
                                                                 .local_radius = 1.6f,
                                                                 .material     = fmi});
    if (g_slimes.face_mesh == UINT32_MAX)
        return false;
    g_slimes.mat_face = fmi;

    /* Spawn the crowd as contiguous slot pairs: the VS phases its jiggle off
       slot/2, so body and face of one slime must share a pair. The face table
       carries the same slot's expression tile, so indirect draws, firstInstance
       and instance-buffer indexing agree — that is what keeps slimes from
       wearing each other's faces. */
    for (uint32_t i = 0; i < SLIME_COUNT; ++i) {
        Slime *s = &g_slimes.slimes[i];
        s->tint  = i % 3u;
        float ang = frand() * 6.2831853f;
        float rad = 1.5f + frand() * 5.5f;
        s->home[0] = cosf(ang) * rad;
        s->home[1] = 1.5f + sinf(ang) * rad * 0.6f;
        s->pos[0]  = s->home[0];
        s->pos[1]  = SLIME_GROUND_Y;
        s->pos[2]  = s->home[1];
        s->vel[0] = s->vel[1] = s->vel[2] = 0.0f;
        s->scale                          = 0.9f + frand() * 0.5f;
        s->yaw = s->yaw_tgt               = frand() * 6.2831853f;
        s->squash                          = 1.0f;
        s->air                             = false;
        s->dwell                           = frand() * 1.2f + (float)i * 0.09f;
        s->face_id                         = FACE_HAPPY;
        s->land_t                          = 0.0f;
        s->land_hard                       = false;
        s->event_t                         = 0.0f;
        s->event_id                        = FACE_HAPPY;
        /* MU_BLINK_NOW staggers the first blink ~at once for verification. */
        s->blink = getenv("MU_BLINK_NOW") ? (0.05f + (float)i * 0.03f)
                                          : (1.5f + frand() * 2.5f + (float)i * 0.21f);
        s->blink_len                       = 0.0f;

        SceneInstanceDesc d = {.pos   = {s->pos[0], 0.8f * s->scale, s->pos[2]},
                               .scale = s->scale};
        d.mesh            = g_slimes.body_mesh[s->tint];
        s->slot           = scene_instance_create(scene, &d);
        d.mesh            = g_slimes.face_mesh;
        s->slot_face      = scene_instance_create(scene, &d);
        if (s->slot == UINT32_MAX || s->slot_face == UINT32_MAX) {
            log_error("[slime] out of dynamic slots at %u", i);
            return false;
        }
        if (s->slot_face != s->slot + 1u || (s->slot % 2u) != 0u) {
            log_error("[slime] slot pair not contiguous: %u/%u", s->slot, s->slot_face);
            return false;
        }
        scene_set_instance_wiggle(scene, s->slot, 0.06f);
        scene_set_instance_wiggle(scene, s->slot_face, 0.06f);
        scene_set_instance_face(scene, s->slot_face, s->face_id);
    }
    log_info("[slime] %u slimes: 3 body meshes + SDF face quad (%u tiles), %u dynamic slots", SLIME_COUNT,
             SLIME_FACE_COUNT, SLIME_COUNT * 2u);
    return true;
}

/* Per-frame hop sim: gravity, ground contact, wander, the squash channel the
   VS reads, and expression changes. Writes only the crowd's dynamic
   instances; the static meadow is never touched. */
static void slime_frame(Scene *scene, float dt) {
    if (dt <= 0.0f || dt > 0.1f)
        dt = 0.1f;
    /* Every few seconds one grounded slime gets the grumps (sad or angry) for
       a couple of seconds, so the crowd shows more than hop-cycle moods. */
    static float grump_in = 3.0f;
    grump_in -= dt;
    if (grump_in <= 0.0f) {
        grump_in = 4.0f + frand() * 3.0f;
        Slime *s = &g_slimes.slimes[(uint32_t)(frand() * SLIME_COUNT) % SLIME_COUNT];
        if (!s->air && s->event_t <= 0.0f) {
            s->event_id = frand() < 0.5f ? FACE_SAD : FACE_ANGRY;
            s->event_t  = 2.5f;
        }
    }
    for (uint32_t i = 0; i < SLIME_COUNT; ++i) {
        Slime *s         = &g_slimes.slimes[i];
        float  squash_tgt = 1.0f;

        if (s->air) {
            s->vel[1] -= SLIME_GRAV * dt;
            s->pos[0] += s->vel[0] * dt;
            s->pos[1] += s->vel[1] * dt;
            s->pos[2] += s->vel[2] * dt;
            if (s->pos[1] <= SLIME_GROUND_Y) {
                s->pos[1] = SLIME_GROUND_Y;
                s->air    = false;
                s->squash = 0.55f; /* landing pancake; springs back below */
                /* Hard falls read as scared, soft ones as happy, for 0.6s. */
                s->land_hard = s->vel[1] < -9.0f;
                s->land_t    = 0.6f;
                s->vel[0] = s->vel[2] = 0.0f;
                s->dwell  = 0.4f + frand() * 2.2f; /* long waits drift hungry, then sleepy */
            } else {
                /* stretch with vertical speed: fast fall = long slime */
                squash_tgt = 1.0f + fabsf(s->vel[1]) * 0.030f;
                if (squash_tgt > 1.35f)
                    squash_tgt = 1.35f;
            }
        } else {
            s->dwell -= dt;
            if (s->dwell <= 0.0f) {
                float ang = frand() * 6.2831853f;
                float rad = 1.5f + frand() * 5.5f;
                float tx  = s->home[0] + cosf(ang) * rad;
                float tz  = s->home[1] + sinf(ang) * rad * 0.6f;
                float dx  = tx - s->pos[0], dz = tz - s->pos[2];
                float dl  = sqrtf(dx * dx + dz * dz);
                if (dl > 0.001f) {
                    dx /= dl;
                    dz /= dl;
                } else {
                    dx = dz = 0.0f;
                }
                float sp    = 2.0f + frand() * 2.0f;
                s->vel[0]   = dx * sp;
                s->vel[2]   = dz * sp;
                s->vel[1]   = 6.0f + frand() * 2.5f;
                s->air      = true;
                s->squash   = 0.78f; /* coiled push-off */
                if (dl > 0.001f)
                    s->yaw_tgt = atan2f(dx, dz); /* +Z is the face */
            }
        }

        /* One fast ease toward the target so the pancake reads as a single
           impact rather than a lerped sag. */
        float ease = dt * 14.0f;
        if (ease > 1.0f)
            ease = 1.0f;
        s->squash += (squash_tgt - s->squash) * ease;

        float dyaw = s->yaw_tgt - s->yaw;
        while (dyaw > 3.14159265f)
            dyaw -= 6.2831853f;
        while (dyaw < -3.14159265f)
            dyaw += 6.2831853f;
        float yase = dt * 6.0f;
        if (yase > 1.0f)
            yase = 1.0f;
        s->yaw += dyaw * yase;

        /* Moods, highest priority first: blink beats everything, then the
           landing reaction, airborne elation, random grumps, and finally the
           dwell drift (hungry, then sleepy). face_id changes only on
           expression change, so the face table re-uploads rarely. */
        if (s->land_t > 0.0f)
            s->land_t -= dt;
        if (s->event_t > 0.0f)
            s->event_t -= dt;
        uint32_t face_want = FACE_HAPPY;
        if (s->blink_len > 0.0f) {
            s->blink_len -= dt;
            face_want = FACE_BLINK;
            if (s->blink_len <= 0.0f)
                s->blink = 1.5f + frand() * 3.0f;
        } else {
            s->blink -= dt;
            if (s->blink <= 0.0f)
                s->blink_len = 0.12f;
            else if (s->land_t > 0.0f)
                face_want = s->land_hard ? FACE_SCARED : FACE_HAPPY;
            else if (s->air)
                face_want = FACE_ELATED;
            else if (s->event_t > 0.0f)
                face_want = s->event_id;
            else if (s->dwell > 2.0f)
                face_want = FACE_SLEEPY;
            else if (s->dwell > 1.0f)
                face_want = FACE_HUNGRY;
        }
        if (face_want != s->face_id) {
            s->face_id = face_want;
            scene_set_instance_face(scene, s->slot_face, s->face_id);
        }

        /* Contact point stays planted: the body spans +/-0.8*scale in local y,
           scaled by squash in the VS, so the centre rides at bottom + that.
           The face quad shares the transform; its VS billboards from it. */
        float cy  = s->pos[1] + 0.8f * s->squash * s->scale;
        float hy  = sinf(s->yaw * 0.5f), hw = cosf(s->yaw * 0.5f);
        SceneInstanceDesc d = {.pos   = {s->pos[0], cy, s->pos[2]},
                               .quat  = {0.0f, hy, 0.0f, hw},
                               .scale = s->scale};
        d.mesh = g_slimes.body_mesh[s->tint];
        scene_instance_set(scene, s->slot, &d);
        d.mesh = g_slimes.face_mesh;
        scene_instance_set(scene, s->slot_face, &d);
        scene_set_instance_squash(scene, s->slot, s->squash);
        scene_set_instance_squash(scene, s->slot_face, s->squash);
    }
}

static void pets_start(void *user, Renderer *renderer) {
    CubePets *p   = (CubePets *)user;
    p->vk         = renderer_vk(renderer);
    p->renderer   = renderer;
    p->dist       = 14.0f;
    /* Standing at the clearing's south edge, looking north across it along the
       path, so the first frame already frames the thing the scene is for. */
    p->cam.pos[0] = 0.0f;
    p->cam.pos[1] = 2.6f;
    p->cam.pos[2] = 14.0f;
    p->cam.yaw    = 0.0f;
    p->cam.pitch  = -0.10f;
    p->cam.speed  = 8.0f;
    glm_vec3_zero(p->cam.forward);
    glm_vec3_zero(p->cam.right);
    glm_vec3_zero(p->cam.up);
    p->cam.forward[2] = -1.0f;
    p->cam.right[0]   = 1.0f;
    p->cam.up[1]      = 1.0f;
}

/* Flycam integration. Reads only the Input snapshot, writes only FlyCam, and
   does no allocation — it runs every frame, so it stays a flat function over
   two small structs. Mouse look needs a captured cursor; without capture the
   deltas are still usable but the cursor leaves the window.
     WASD / arrows  move along the view basis (Q/E strafe vertically)
     Space/Ctrl     up/down
     Shift          4x boost
     hold RMB       mouse look
     wheel          adjust speed
*/
static void flycam_update(FlyCam *c, const Input *in, float dt) {
    if (dt > 0.1f)
        dt = 0.1f;
    c->dt = dt;

    if (mouse_down(in, MOUSE_RIGHT)) {
        c->yaw -= (float)mouse_dx(in) * 0.0025f;
        c->pitch -= (float)mouse_dy(in) * 0.0025f;
        /* Clamp below the horizon: with an infinite reverse-Z far plane there is
           no far clipping to hide a singularity, but an inverted basis makes the
           handedness of the projection flip and geometry culls inside out. */
        c->pitch = CLAMP(c->pitch, -1.5533f, 1.5533f);
    }

    c->speed = CLAMP(c->speed * (1.0f - (float)scroll_y(in) * 0.12f), 0.5f, 400.0f);

    vec3 fwd, right, up;
    fwd[0]   = cosf(c->pitch) * sinf(c->yaw);
    fwd[1]   = sinf(c->pitch);
    fwd[2]   = -cosf(c->pitch) * cosf(c->yaw);
    right[0] = cosf(c->yaw);
    right[1] = 0.0f;
    right[2] = sinf(c->yaw);
    up[0]    = 0.0f;
    up[1]    = 1.0f;
    up[2]    = 0.0f;

    float boost = key_down(in, KEY_LEFT_SHIFT) ? 4.0f : 1.0f;
    float move = 0.0f, strafe = 0.0f, lift = 0.0f;
    if (key_down(in, KEY_W) || key_down(in, KEY_UP))
        move += 1.0f;
    if (key_down(in, KEY_S) || key_down(in, KEY_DOWN))
        move -= 1.0f;
    if (key_down(in, KEY_D) || key_down(in, KEY_RIGHT))
        strafe += 1.0f;
    if (key_down(in, KEY_A) || key_down(in, KEY_LEFT))
        strafe -= 1.0f;
    if (key_down(in, KEY_E) || key_down(in, KEY_SPACE))
        lift += 1.0f;
    if (key_down(in, KEY_Q) || key_down(in, KEY_LEFT_CTRL))
        lift -= 1.0f;

    float step = c->speed * boost * dt;
    c->pos[0] += (fwd[0] * move + right[0] * strafe) * step;
    c->pos[1] += (fwd[1] * move + up[1] * lift) * step;
    c->pos[2] += (fwd[2] * move + right[2] * strafe) * step;

    glm_vec3_copy(fwd, c->forward);
    glm_vec3_copy(right, c->right);
    glm_vec3_copy(up, c->up);
}

static void pets_frame(void *user, const GameFrame *frame) {
    CubePets *p = (CubePets *)user;
    if (frame->dt > 0.1f)
        p->dt = 0.1f;
    else
        p->dt = frame->dt;

    flycam_update(&p->cam, frame->input, frame->dt);

    if (p->scene)
        slime_frame(p->scene, p->dt);

    /* Scripted screenshot for headless verification: MU_SHOT=<frames> writes
       one screenshot after that many frames, then keeps running. */
    {
        const char *shot = getenv("MU_SHOT");
        if (shot) {
            static int n = 0;
            if (++n == atoi(shot))
                renderer_screenshot(frame->renderer, "captures/screenshots/mu_shot.png");
        }
    }

    /* Headless perf dump: MU_PERF=<frames> writes perf_report.txt after that
       many frames (plus a settle window so the EMA converges), then exits. */
    {
        const char *perf = getenv("MU_PERF");
        if (perf) {
            static int n = 0;
            int        at = atoi(perf);
            if (++n == at) {
                renderer_write_perf(frame->renderer);
                exit(EXIT_SUCCESS);
            }
        }
    }

    /* TEMP: scripted flycam sweep for verification. Removed after the shot. */
    if (getenv("MU_FLYCAM_TEST")) {
        static int n = 0;
        p->cam.yaw += 0.02f;
        p->cam.pitch      = -0.45f + sinf(n * 0.01f) * 0.25f;
        p->cam.pos[0]     = sinf(n * 0.004f) * 26.0f;
        p->cam.pos[1]     = 10.0f;
        p->cam.pos[2]     = cosf(n * 0.004f) * 26.0f;
        p->cam.forward[0] = cosf(p->cam.pitch) * sinf(p->cam.yaw);
        p->cam.forward[1] = sinf(p->cam.pitch);
        p->cam.forward[2] = -cosf(p->cam.pitch) * cosf(p->cam.yaw);
        if (++n % 120 == 0)
            log_info("[flycam] pos %.1f %.1f %.1f yaw %.2f pitch %.2f", p->cam.pos[0], p->cam.pos[1], p->cam.pos[2],
                     p->cam.yaw, p->cam.pitch);
    }

    SceneCounters c;

    if (p->scene && scene_counters(p->scene, &c)) {
        // renderer_hud(frame->renderer, "scene: %u submitted  %u frustum-culled  %u drawn", c.submitted, c.culled_frustum,
        //              c.drawn);
        // renderer_hud(frame->renderer, "groups %u  dropped %u  lod %u/%u/%u/%u", c.draws, c.dropped, c.lod[0], c.lod[1],
        //              c.lod[2], c.lod[3]);
        //
        // log_info("[cubepets] counters: submitted=%u frustum_culled=%u drawn=%u draws=%u dropped=%u", c.submitted,
        //                    c.culled_frustum, c.drawn, c.draws, c.dropped);
        //

        static bool logged = false;
        if (key_down(frame->input, KEY_L)) {
            logged = !logged;
        }

        static int dbg = 0;
        if (++dbg % 120 == 0)
            log_info("[pets] sub=%u frustum=%u drawn=%u draws=%u dropped=%u lod=%u/%u/%u", c.submitted, c.culled_frustum,
                     c.drawn, c.draws, c.dropped, c.lod[0], c.lod[1], c.lod[2]);
        if (!logged && c.drawn > 0) {
            logged = true;
            log_info("[cubepets] counters: submitted=%u frustum_culled=%u drawn=%u draws=%u dropped=%u", c.submitted,
                     c.culled_frustum, c.drawn, c.draws, c.dropped);
        }
    }
    // renderer_hud(frame->renderer, "flycam: %.1f %.1f %.1f  yaw %.2f pitch %.2f  speed %.0f", p->cam.pos[0],
    //              p->cam.pos[1], p->cam.pos[2], p->cam.yaw, p->cam.pitch, p->cam.speed);
    // renderer_hud(frame->renderer, "WASD move  Q/E down-up  Shift boost  RMB look  wheel speed");
}

/* GameHooks.render: lazy-init (needs a command buffer for upload + the pass
   formats for the pipeline), then set the view and record the scene. */
static void pets_render(void *user, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth) {
    CubePets *p = (CubePets *)user;

    /* The Color Grade window owns the toon switch; mirror it onto the scene. */
    if (p->scene)
        scene_set_toon(p->scene, renderer_toon(p->renderer));

    if (!p->scene) {
        /* Capacities come from the assets themselves rather than from a
           constant: mesh and LOD capacities are fixed at scene_create and
           cannot grow, so a hardcoded number is a number that is wrong the
           first time somebody adds a prop. */
        /* Capacities from the cooked assets: one instance per mesh of every
           prop, all placed static. */
        uint32_t asset_meshes[A_COUNT] = {0};
        uint32_t max_meshes = 0, max_materials = 0, max_rungs = 1;
        for (uint32_t m = 0; m < A_COUNT; ++m) {
            SceneAssetProbe probe;
            char            err[256];
            if (!scene_asset_probe(pets_assets[m], &probe, err, sizeof(err))) {
                log_error("[pets] %s: %s", pets_assets[m], err);
                return;
            }
            max_meshes += probe.mesh_count;
            max_materials += probe.material_count;
            if (probe.max_lod_count > max_rungs)
                max_rungs = probe.max_lod_count;
            asset_meshes[m] = probe.mesh_count;
        }
        /* A mesh is charged the scene-wide rung count, not its own, because a
           group id is mesh * lod_count + lod. So a roster that mixes a 1-rung
           tile with a 2-rung tree needs max_meshes * 2 rows, not the sum of the
           assets' own ladders — sizing it from the sum runs out mid-roster. */
        uint32_t total = pets_instance_total(asset_meshes);
        /* +4 meshes and +4 materials for the procedural slime crowd staged after
           the cooked assets: three tinted body variants and the shared SDF face
           quad. Mesh and material capacities are fixed at create; the instances
           themselves are dynamic and draw from the MEADOW_MOVING reserve, so
           the static count is unchanged. */
        max_meshes += 4u;
        max_materials += 4u;
        /* A mesh is charged the scene-wide rung count, not its own, because a
           group id is mesh * lod_count + lod. So a roster that mixes a 1-rung
           tile with a 2-rung tree needs max_meshes * 2 rows, not the sum of the
           assets' own ladders — sizing it from the sum runs out mid-roster. The
           slime mesh is counted here too, so it gets its rungs as well. */
        uint32_t max_lod_rows = max_meshes * max_rungs;
        /* Everything here is static, so the static region holds the whole
           clearing and the dynamic region is only the reserve: the split is
           what makes the per-frame upload touch nothing at all. */
        uint32_t slots = total + MEADOW_MOVING;
        Scene   *scene = scene_create(p->vk, &(SceneDesc){.max_instances    = slots,
                                                         .dynamic_capacity = MEADOW_MOVING,
                                                         .static_capacity  = total,
                                                         .max_meshes       = max_meshes,
                                                         .max_lod_rows     = max_lod_rows,
                                                         .lod_count        = max_rungs,
                                                         .max_materials    = max_materials ? max_materials : 4,
                                                         .max_survivors    = slots});
        if (!scene)
            return;
        if (pets_build_assets(scene) == UINT32_MAX) {
            scene_destroy(scene);
            return;
        }

        /* Slime crowd: 12 dynamic pairs (body, SDF face quad) out of
           the MEADOW_MOVING reserve, staged before the upload with the rest. */
        if (!slime_build(scene)) {
            scene_destroy(scene);
            return;
        }

        if (!scene_upload_scene(scene, cmd)) {
            scene_destroy(scene);
            return;
        }
        p->scene = scene;
        log_info("[pets] scene live: %u instances, %u meshes, %u rungs, %u groups", total, max_meshes,
                 scene_lod_count(scene), max_meshes * scene_lod_count(scene));
    }

    /* camera: the flycam's eye and forward vector, built during pets_frame */
    float aspect = (float)color->width / (float)color->height;
    vec3  eye;
    glm_vec3_copy(p->cam.pos, eye);
    vec3 center;
    center[0] = eye[0] + p->cam.forward[0];
    center[1] = eye[1] + p->cam.forward[1];
    center[2] = eye[2] + p->cam.forward[2];
    vec3 up;
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;

    mat4 proj, view, vp;
    /* reverse-Z with an infinite far plane, written out rather than taken from
       cglm: the infinite_rh_zo entry point is behind a CLIPSPACE_INCLUDE_RH_ZO
       guard this project does not enable, and it is in any case zero-to-one
       rather than reverse-Z. The depth attachment compares GREATER and clears
       to 0.0 to match — see scene.c. */
    {
        float t = 1.0f / tanf(glm_rad(60.0f) * 0.5f);
        glm_mat4_zero(proj);
        proj[0][0] = t / aspect;
        proj[1][1] = -t;
        /* z_clip = near, w_clip = -z_view, so z_ndc = near/dist: 1.0 at the near
           plane falling to 0 at infinity. That is the reverse-Z mapping, and it
           is why [2][2] is 0 and [3][2] is +near. cglm's *_rh_zo infinite entry
           point is zero-to-one, not reverse-Z — do not substitute it here. */
        proj[2][2] = 0.0f;
        proj[2][3] = -1.0f;
        proj[3][2] = 0.1f; /* nearZ */
    }
    glm_lookat(eye, center, up, view);
    glm_mat4_mul(proj, view, vp);

    /* Hand the real camera to the renderer so GPU effects (SSAO) can
       reconstruct world position and reproject history across frames. */
    renderer_set_camera((Renderer *)p->renderer, (const float *)view, (const float *)proj);

    SceneViewDesc vd = {0};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            vd.clip_rows[r][c] = vp[c][r]; /* cglm is column-major */
    vd.camera_pos[0] = eye[0];
    vd.camera_pos[1] = eye[1];
    vd.camera_pos[2] = eye[2];
/* The cooker writes each rung's error in the mesh's own world units, and the
       cull compares it against dist * lod_target / scale. Making that a real
       screen-space tolerance means solving for the factor: a world length L at
       distance d covers L*H / (2*d*tan(fovy/2)) pixels, so a rung is
       acceptable when its error covers at most T pixels, which is exactly
       error <= T * 2*tan(fovy/2) * d / (H * scale). Hence:

           lod_target = T * 2 * tan(fovy/2) / H

       Anything else compares pixels to metres. T = 2 px hides the pop a
       one-pixel threshold would show. The fovy must be the one the projection
       below actually builds. */
    vd.lod_target    = 2.0f * 2.0f * tanf(glm_rad(60.0f) * 0.5f) / (float)color->height;
    vd.near_z        = 0.1f;
    vd.far_z         = 400.0f;
    scene_view_set(p->scene, 0, &vd);

    scene_frame(p->scene, cmd, color, depth);
}

static void pets_shutdown(void *user) {
    CubePets *p = (CubePets *)user;
    if (p->scene) {
        scene_destroy(p->scene);
        p->scene = NULL;
    }
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

    GameHooks game     = {.user     = &g_pets,
                          .two_d    = false,
                          .start    = pets_start,
                          .frame    = pets_frame,
                          .render   = pets_render,
                          .shutdown = pets_shutdown};
    Renderer *renderer = renderer_create(use_wayland, game);
    if (!renderer)
        return EXIT_FAILURE;
    while (renderer_frame(renderer)) {
    }
    renderer_destroy(renderer);
    return EXIT_SUCCESS;
}
