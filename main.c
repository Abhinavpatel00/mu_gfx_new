#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    Renderer *renderer = renderer_create(use_wayland);
    if (!renderer)
        return EXIT_FAILURE;
    while (renderer_frame(renderer)) {}
    renderer_destroy(renderer);
    return EXIT_SUCCESS;
}
