#ifndef MU_GFX_RENDERER_H
#define MU_GFX_RENDERER_H
#include <stdbool.h>
#include <stdint.h>

#include "input.h"
#include "vk.h"

typedef struct Renderer Renderer;
struct SpriteSystem; /* defined by src/two_d/sprite.h, only for 2D apps */

/* One frame of game work, handed over just before the scene pass. */
typedef struct GameFrame {
    Renderer            *renderer;
    struct SpriteSystem *sprites; /* NULL when the app runs without the 2D stage */
    const Input         *input;
    float                dt;
    uint32_t             viewport_w;
    uint32_t             viewport_h;
} GameFrame;

/* The game lives outside the renderer — main.c owns the state and registers
   these entry points. All are optional; a NULL hook is skipped. */
typedef struct GameHooks {
    void *user;
    /* Opt into the 2D stage. When false the sprite system, its atlases, streams
       and pipelines are never created and the sprite pass never opens — a 3D
       app pays nothing for 2D. */
    bool two_d;
    void (*start)(void *user, Renderer *renderer); /* once, after the stages exist */
    void (*frame)(void *user, const GameFrame *frame); /* every frame, before the passes */
    /* Every frame after the 2D stage, before post-processing. Record GPU
       work here (it has the scene color/depth targets). */
    void (*render)(void *user, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth);
    /* Once at shutdown, after wait_idle and delete-queue drain. */
    void (*shutdown)(void *user);
} GameHooks;

Renderer *renderer_create(bool use_wayland, GameHooks game);
void      renderer_destroy(Renderer *renderer);
bool      renderer_frame(Renderer *renderer);

/* The backend the app allocates its own resources through, and the 2D stage's
   sprite system (NULL when GameHooks.two_d is false). */
VkBackend          *renderer_vk(Renderer *renderer);
struct SpriteSystem *renderer_sprites(Renderer *renderer);

/* Whether the user has switched the scene to the cel-shaded pipeline in the
   Color Grade window. The app mirrors this onto its own scene each frame. */
bool renderer_toon(Renderer *renderer);

/* Feed the renderer this frame's view and projection (column-major, cglm
   layout). The renderer derives view-proj, its inverse, and keeps the previous
   frame's view-proj. Call once per frame before the passes. */
void renderer_set_camera(Renderer *renderer, const float *view, const float *projection);

/* The world the shading paths light against. The renderer owns it — rather than
   each scene pipeline carrying its own copy — so the sky pass and the PBR and
   toon fragment stages cannot disagree about the sun or the ambient. Angles are
   in radians; colors are linear. */
typedef struct RenderEnvironment {
    float sky[3];     /* ambient for up-facing normals; also the sky's zenith */
    float horizon[3]; /* the sky gradient's bottom stop */
    float ground[3];  /* ground bounce for down-facing normals */
    float sun_dir[3]; /* direction toward the sun, normalized by the setter */
    float sun_color[3];
    float sun_intensity;
    float ambient;    /* global scale on the ambient term */
    /* Sky shape. A look knob, not a fact about the world, so it stays out of the
       shading model even though it rides the same block. */
    float sky_exponent;
    float sun_glow;
    float sun_glow_power;
} RenderEnvironment;

/* A bright outdoor standing point: blue sky, warm sun, plenty of bounce. */
RenderEnvironment renderer_environment_default(void);
void              renderer_set_environment(Renderer *renderer, const RenderEnvironment *env);
/* The current environment, for an app that wants to tweak one field. */
RenderEnvironment renderer_environment(const Renderer *renderer);

/* Queue a line for the on-screen Game window. The queue clears every frame, so
   call it once per frame for each line you want. Silently drops overflow. */
void renderer_hud(Renderer *renderer, const char *fmt, ...);

/* Programmatic screenshot (same path as the R hotkey): writes path on the
   next presented frame. Handy for scripted verification without key input. */
bool renderer_screenshot(Renderer *renderer, const char *path);

/* Write the rolling frame + per-pass GPU timings to perf_report.txt. For
   scripted/headless measurement (see MU_PERF in main.c). */
void renderer_write_perf(Renderer *renderer);

#endif
