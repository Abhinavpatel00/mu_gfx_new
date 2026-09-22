#ifndef MU_GFX_RENDERER_H
#define MU_GFX_RENDERER_H
#include <stdbool.h>
typedef struct Renderer Renderer;

// Forward declaration
struct GrassSystem;

Renderer *renderer_create(bool use_wayland);
void renderer_destroy(Renderer *renderer);
bool renderer_frame(Renderer *renderer);

// Grass system accessor
struct GrassSystem* renderer_get_grass_system(Renderer *renderer);

#endif
