#ifndef MU_GFX_RENDERER_H
#define MU_GFX_RENDERER_H
#include <stdbool.h>
#include <stdint.h>

#include "src/input.h"
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

/* Queue a line for the on-screen Game window. The queue clears every frame, so
   call it once per frame for each line you want. Silently drops overflow. */
void renderer_hud(Renderer *renderer, const char *fmt, ...);

#endif
