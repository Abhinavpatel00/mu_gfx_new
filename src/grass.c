#include "grass_system.h"

#include <stdio.h>
#include <stdlib.h>

// xorshift32: deterministic scatter, no library RNG state.
static uint32_t grass_rand(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float grass_rand_unit(uint32_t *state) {
    return (float)(grass_rand(state) >> 8) * (1.0f / 16777216.0f);
}

GrassSystem *grass_system_create(void) { return calloc(1, sizeof(GrassSystem)); }

void grass_system_destroy(GrassSystem *sys) {
    if (!sys)
        return;
    free(sys->instances);
    free(sys);
}

void grass_system_generate_field(GrassSystem *sys, float width, float depth, float spacing, uint32_t seed) {
    if (!sys || spacing <= 0.0f || width <= 0.0f || depth <= 0.0f)
        return;

    uint32_t cols     = (uint32_t)(width / spacing) + 1u;
    uint32_t rows     = (uint32_t)(depth / spacing) + 1u;
    uint32_t capacity = cols * rows;
    struct GrassInst *instances = realloc(sys->instances, capacity * sizeof(*instances));

    uint32_t state  = seed | 1u;
    float    half_w = width * 0.5f;
    float    half_d = depth * 0.5f;
    uint32_t count  = 0;

    // Shag-carpet field: dense fat lobes. Height 0.12..0.20 m, half-width
    // 0.035..0.055 m — roughly 2.5:1 height:width so each lobe reads as a fat
    // capsule. Spacing below full width makes neighbours overlap into a
    // solid carpet with no ground showing through.
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t col = 0; col < cols; ++col) {
            float height_var = grass_rand_unit(&state);
            float width_var  = grass_rand_unit(&state);

            struct GrassInst *inst = &instances[count++];
            inst->x      = -half_w + (float)col * spacing + (grass_rand_unit(&state) - 0.5f) * spacing;
            inst->y      = 0.0f;
            inst->z      = -half_d + (float)row * spacing + (grass_rand_unit(&state) - 0.5f) * spacing;
            inst->height = 0.12f + 0.08f * height_var;
            inst->width  = 0.035f + 0.020f * width_var;
            // low 16 bits: yaw, high 16 bits: tint jitter, both consumed in the shader.
            inst->seed   = grass_rand(&state);
        }
    }

    sys->instances = instances;
    sys->count     = count;
    printf("[grass] %u blades over %.0fx%.0f m (spacing %.2f)\n", count, width, depth, spacing);
}
