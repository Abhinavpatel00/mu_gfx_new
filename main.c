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
 * Stress scene replacing the farm: a many-instance grid of mixed cube models
 * under a GPU frustum cull, drawn with one indexed indirect draw per mesh
 * slot. The camera is a mouse/keyboard orbit around the grid origin. */

/* The grid exists to make the compaction pass do real work: PETS_MESHES is the
   number of distinct (mesh,lod) groups, so PETS_MESHES above SCENE_SCAN_BLOCK
   (1024) is what forces the two-level scan to use more than one block. A
   single-group scene would prove the parallel scan nothing. */
#define PETS_GRID     3u
#define PETS_SPACING  3.0f
#define PETS_MESHES   1200u
#define PETS_PER_CELL 4u
#define PETS_MODELS   6u

static const char *const pets_models[PETS_MODELS] = {
    "data/threedassets/kaykitadventure/Characters/gltf/Barbarian.glb",
    "data/threedassets/kaykitadventure/Characters/gltf/Knight.glb",
    "data/threedassets/kaykitadventure/Characters/gltf/Mage.glb",
    "data/threedassets/kaykitadventure/Characters/gltf/Ranger.glb",
    "data/threedassets/kaykitadventure/Characters/gltf/Rogue.glb",
    "data/threedassets/kaykitadventure/Characters/gltf/Rogue_Hooded.glb",
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

/* One unit cube: 24 packed vertices (per-face normals) + 36 u16 indices. */
/* Builds PETS_MESHES distinct meshes out of one cube topology. They differ only
   in local radius and material, which is enough: what the compaction pass sees
   is one group per mesh, so this is what makes G large. */
static void pets_build_cubes(Scene *scene) {
    static const float face_n[6][3]    = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const float face_v[6][4][3] = {
        {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
        {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},
        {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}},
        {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}},
        {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}},
    };
    static const float face_uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

    struct ScenePackedVertex verts[24];
    uint16_t                 indices[36];
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t v = 0; v < 4; ++v) {
            scene_pack_vertex(&verts[f * 4 + v], face_v[f][v], face_n[f], face_uv[v]);
        }
        uint16_t base      = (uint16_t)(f * 4);
        indices[f * 6 + 0] = base + 0;
        indices[f * 6 + 1] = base + 1;
        indices[f * 6 + 2] = base + 2;
        indices[f * 6 + 3] = base + 0;
        indices[f * 6 + 4] = base + 2;
        indices[f * 6 + 5] = base + 3;
    }

    for (uint32_t m = 0; m < PETS_MESHES; ++m) {
        /* All cubes share one topology; groups differ by index, which is all
           the compaction pass needs. (The old k variation was dead code.) */
        scene_mesh_add(scene, &(SceneMeshDesc){.vertices     = verts,
                                               .vertex_count = 24,
                                               .indices      = indices,
                                               .index_count  = 36,
                                               .local_center = {0, 0, 0},
                                                  .local_radius = 0.8660254f,
                                               .material     = 0});
    }
    log_info("[cubepets] %u meshes registered", PETS_MESHES);
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

    if (!p->scene) {
        /* Half the cells are static (written once, never again) and half are
           dynamic (rewritten every frame). That split is what the C6 partition
           exists for, and both kinds must land in the same slot space. */
        uint32_t cells    = PETS_GRID * PETS_GRID;
        uint32_t per_cell = PETS_PER_CELL;
        uint32_t total    = cells * per_cell;
        uint32_t half     = total / 2;
        Scene   *scene    = scene_create(p->vk, &(SceneDesc){.max_instances    = total,
                                                             .dynamic_capacity = half,
                                                             .static_capacity  = total - half,
                                                             .max_meshes       = PETS_MESHES,
                                                             .max_lod_rows     = PETS_MESHES,
                                                             .max_materials    = 4,
                                                             .max_survivors    = total});
        if (!scene)
            return;
        pets_build_cubes(scene);

        /* One unique mesh per instance so every group the scan sees is
           populated: draws == instances instead of 4. Inside a cell the 4
           cubes sit on a 2x2 sub-grid at +/-0.75: 1.5 pitch vs 1.0 size
           leaves a 0.5 gap, so no two cubes in the same cell intersect.
           The old jx/jz walked 0..~2 diagonally, stacking 1.0-wide cubes
           ~0.3 apart — that overlap is the staircase. */
        const float cell_half = (float)PETS_GRID * PETS_SPACING * 0.5f;
        uint32_t    inst      = 0;
        for (uint32_t z = 0; z < PETS_GRID; ++z) {
            for (uint32_t x = 0; x < PETS_GRID; ++x) {
                for (uint32_t m = 0; m < PETS_MESHES; ++m) {
                    if (m >= per_cell)
                        break;
                    float             ox = ((m & 1u) != 0u) ? 0.75f : -0.75f;
                    float             oz = ((m & 2u) != 0u) ? 0.75f : -0.75f;
                    SceneInstanceDesc d  = {.pos   = {(float)x * PETS_SPACING - cell_half + ox, 0.0f,
                                                      (float)z * PETS_SPACING - cell_half + oz},
                                            .quat  = {0, 0, 0, 0},
                                            .scale = 1.0f,
                                            .mesh  = inst % PETS_MESHES};
                    /* Alternate so both regions are populated in every row. */
                    if (((x + z + m) & 1u) == 0u)
                        scene_instance_create(scene, &d);
                    else
                        scene_instance_create_static(scene, &d);
                    ++inst;
                }
            }
        }

        if (!scene_upload_scene(scene, cmd)) {
            scene_destroy(scene);
            return;
        }
        scene_set_sun(scene, (const float[3]){0.4f, 0.8f, 0.3f}, 0.18f);
        p->scene = scene;

        /* Destroy every other instance and reclaim the holes. Exercises the
           existence-based removal path (candidate rows go away, slots retire,
           no death flag is ever tested) and proves compaction restores a dense
           slot space afterwards. */
               log_info("[cubepets] after destroy+compact: %u instances, %u dynamic, %u static", total / 2, total / 4,
                 total / 4);
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
        proj[1][1] = t;
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
    vd.lod_target    = 1.0f;
    vd.near_z        = 0.1f;
    vd.far_z         = 400.0f;
    scene_view_set(p->scene, 0, &vd);

    /* TEMP instrumentation: auto-screenshot once the scene has rendered. */
    {
        bool capture_take_screenshot(void *renderer, const char *path);
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
