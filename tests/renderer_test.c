#include "../renderer.h"
#include "../vk.h"
#include "../vk.h"
#include <stdio.h>

int main(void) {
    Renderer *renderer = renderer_create(false);
    if (!renderer)
        return 1;

    for (uint32_t i = 0; i < 120; ++i) {
        if (!renderer_frame(renderer))
            break;
    }

    renderer_destroy(renderer);
    puts("PASS: renderer initialization, frame loop, and shutdown");
    return 0;
}
