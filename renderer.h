#ifndef MU_GFX_RENDERER_H
#define MU_GFX_RENDERER_H
#include <stdbool.h>
#include <stdint.h>

#include "src/input.h"
#include "src/two_d/sprite.h"

typedef struct Renderer Renderer;

/* One frame of game work, handed over just before the sprite flush. */
typedef struct GameFrame {
    Renderer     *renderer;
    SpriteSystem *sprites;
    const Input  *input;
    float         dt;
    uint32_t      viewport_w;
    uint32_t      viewport_h;
} GameFrame;

/* The game lives outside the renderer — main.c owns the state and registers
   these two entry points. Both are optional; a NULL hook is skipped. */
typedef struct GameHooks {
    void *user;
    void (*start)(void *user, SpriteSystem *sprites);  /* once, after sprite_system_init */
    void (*frame)(void *user, const GameFrame *frame); /* every frame, before the flush */
} GameHooks;

Renderer *renderer_create(bool use_wayland, GameHooks game);
void      renderer_destroy(Renderer *renderer);
bool      renderer_frame(Renderer *renderer);

/* Queue a line for the on-screen Game window. The queue clears every frame, so
   call it once per frame for each line you want. Silently drops overflow. */
void renderer_hud(Renderer *renderer, const char *fmt, ...);

#endif
