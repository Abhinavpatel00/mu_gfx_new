#ifndef MU_GFX_RENDERER_H
#define MU_GFX_RENDERER_H
#include <stdbool.h>
typedef struct Renderer Renderer;
Renderer *renderer_create(bool use_wayland);
void renderer_destroy(Renderer *renderer);
bool renderer_frame(Renderer *renderer);
#endif
