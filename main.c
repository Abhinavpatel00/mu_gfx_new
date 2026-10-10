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

/* The grid exists to make the compaction pass do real work: the number of
   distinct (mesh,lod) groups must exceed SCENE_SCAN_BLOCK (1024) for the
   two-level scan to use more than one block. A single-group scene would prove
   the parallel scan nothing. */
#define PETS_GRID     6u
#define PETS_SPACING  3.5f
#define PETS_MODELS   1u
#define PETS_COPIES   1u

/* Cooked .mua files. Node transforms are baked into the vertices, so a model is
   a group of meshes in one space: every mesh of a model gets its own instance
   at the same transform. PETS_MODELS is how many of these to load: it is 1 to
   look at Barbarian alone, and raising it brings the rest back in. */
static const char *const pets_assets[] = {
    "assets/Barbarian.mua",
    "assets/Knight.mua",
    "assets/Mage.mua",
    "assets/Ranger.mua",
    "assets/Rogue.mua",
    "assets/Rogue_Hooded.mua",
};

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

/* Cooks every asset and lays the models out over the grid. Each model
   contributes SceneAssetLoad.mesh_count instances — a character's nine parts
   are nine meshes in one space, not nine models — so the instance total is the
   sum over the assets, not PETS_MODELS.
 *
   Returns the number of meshes registered, or UINT32_MAX on a hard load
   failure; the caller has already created the scene with matching capacities
   and cannot proceed without them. */
static uint32_t pets_build_assets(Scene *scene) {
    uint32_t total_meshes = 0;
    for (uint32_t m = 0; m < PETS_MODELS; ++m) {
        SceneAssetLoad load;
        char           err[256];
        if (!scene_asset_load(scene, pets_assets[m], &load, err, sizeof(err))) {
            log_error("[pets] %s: %s", pets_assets[m], err);
            return UINT32_MAX;
        }
        log_info("[pets] %s: %u meshes, %u verts, %u indices, %u rungs, radius %.2f", pets_assets[m], load.mesh_count,
                 load.vertex_count, load.index_count, load.lod_count, load.radius);

        /* One character per cell. A model is a rigid assembly of meshes in one
           space, so every one of its meshes gets the same transform — spreading
           them out would show a disassembled model rather than one. */
        for (uint32_t c = 0; c < PETS_GRID * PETS_GRID * PETS_COPIES; ++c) {
            float x = ((float)(c % PETS_GRID) - (float)PETS_GRID * 0.5f + 0.5f) * PETS_SPACING;
            float z = ((float)((c / PETS_GRID) % PETS_GRID) - (float)PETS_GRID * 0.5f + 0.5f) * PETS_SPACING;

            for (uint32_t i = 0; i < load.mesh_count; ++i) {
                SceneInstanceDesc d = {.pos = {x, 0.0f, z}, .quat = {0, 0, 0, 0}, .scale = 1.0f,
                                       .mesh = load.first_mesh + i};
                /* Alternate so both slot regions are populated in every row. */
                if (((c + i) & 1u) == 0u)
                    scene_instance_create(scene, &d);
                else
                    scene_instance_create_static(scene, &d);
            }
        }
        total_meshes += load.mesh_count;
    }
    log_info("[pets] %u meshes registered from %u models", total_meshes, PETS_MODELS);
    return total_meshes;
}

static void pets_start(void *user, Renderer *renderer) {
    CubePets *p   = (CubePets *)user;
    p->vk         = renderer_vk(renderer);
    p->renderer   = renderer; /* TEMP */
    p->dist       = 34.0f;
    p->cam.pos[0] = 0.0f;
    p->cam.pos[1] = 6.0f;
    p->cam.pos[2] = 22.0f;
    p->cam.yaw    = 0.0f; /* looking down -Z */
    p->cam.pitch  = -0.25f;
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
        uint32_t max_meshes = 0, max_lod_rows = 0, max_materials = 0, total = 0;
        for (uint32_t m = 0; m < PETS_MODELS; ++m) {
            SceneAssetProbe probe;
            char            err[256];
            if (!scene_asset_probe(pets_assets[m], &probe, err, sizeof(err))) {
                log_error("[pets] %s: %s", pets_assets[m], err);
                return;
            }
            max_meshes += probe.mesh_count;
            max_lod_rows += probe.lod_rows;
            max_materials += probe.material_count;
            total += probe.mesh_count * PETS_COPIES * PETS_GRID * PETS_GRID;
        }

        uint32_t half = total / 2;
        Scene   *scene = scene_create(p->vk, &(SceneDesc){.max_instances    = total,
                                                         .dynamic_capacity = half,
                                                         .static_capacity  = total - half,
                                                         .max_meshes       = max_meshes,
                                                         .max_lod_rows     = max_lod_rows,
                                                         .max_materials    = max_materials ? max_materials : 4,
                                                         .max_survivors    = total});
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
        scene_set_sun(scene, (const float[3]){0.4f, 0.8f, 0.3f}, 0.18f);
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
        if (++shot == 180)
            capture_take_screenshot(p->renderer, "/tmp/kilo/cubepets_shot.png");
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
