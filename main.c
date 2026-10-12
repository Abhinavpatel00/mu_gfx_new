#include "renderer.h"

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
   a scene with none still pays for its ceiling and nothing else. */
#define MEADOW_MOVING     16

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
    void      *renderer; /* TEMP: for the auto-screenshot probe */

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

static void pets_start(void *user, Renderer *renderer) {
    CubePets *p   = (CubePets *)user;
    p->vk         = renderer_vk(renderer);
    p->renderer   = renderer; /* TEMP */
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
        renderer_hud(frame->renderer, "scene: %u submitted  %u frustum-culled  %u drawn", c.submitted, c.culled_frustum,
                     c.drawn);
        renderer_hud(frame->renderer, "groups %u  dropped %u  lod %u/%u/%u/%u", c.draws, c.dropped, c.lod[0], c.lod[1],
                     c.lod[2], c.lod[3]);

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
    renderer_hud(frame->renderer, "flycam: %.1f %.1f %.1f  yaw %.2f pitch %.2f  speed %.0f", p->cam.pos[0],
                 p->cam.pos[1], p->cam.pos[2], p->cam.yaw, p->cam.pitch, p->cam.speed);
    renderer_hud(frame->renderer, "WASD move  Q/E down-up  Shift boost  RMB look  wheel speed");
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
        uint32_t max_lod_rows = max_meshes * max_rungs;
        uint32_t total = pets_instance_total(asset_meshes);
        /* Everything here is static, so the static region holds the whole
           clearing and the dynamic region is only the reserve: the split is
           what makes the per-frame upload touch nothing at all. */
        uint32_t slots = total + MEADOW_MOVING;
        Scene   *scene = scene_create(p->vk, &(SceneDesc){.max_instances    = slots,
                                                         .dynamic_capacity = MEADOW_MOVING,
                                                         .static_capacity  = total,
                                                         .max_meshes       = max_meshes,
                                                         .max_lod_rows     = max_lod_rows,
                                                         .max_materials    = max_materials ? max_materials : 4,
                                                         .max_survivors    = slots});
        if (!scene)
            return;
        if (pets_build_assets(scene) == UINT32_MAX) {
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

    /* TEMP instrumentation: auto-screenshot once the scene has rendered. */
    {
        bool       capture_take_screenshot(void *renderer, const char *path);
        static int shot = 0;
        if (++shot == 18)
            capture_take_screenshot(p->renderer, "shot.png");
    }

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
