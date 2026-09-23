#ifndef GRASS_SYSTEM_H
#define GRASS_SYSTEM_H

#include <stdint.h>

// One blade. Mirrored by GrassInst in shaders/grass.slang; keep the layout in sync.
// seed: low 16 bits = yaw, high 16 bits = tint jitter (-1..1 in the shader).
struct GrassInst {
    float    x, y, z; // root position in world space
    float    height;
    float    width;  // half-cylinder radius at the root; tapers toward the tip
    uint32_t seed;
};

_Static_assert(sizeof(struct GrassInst) == 24, "GrassInst must match the Slang struct");

typedef struct GrassSystem {
    struct GrassInst *instances;
    uint32_t          count;
} GrassSystem;

GrassSystem *grass_system_create(void);
void         grass_system_destroy(GrassSystem *sys);

// Deterministic jittered-grid scatter over a width x depth field centred on the
// origin. spacing is the mean distance between blades in metres.
void grass_system_generate_field(GrassSystem *sys, float width, float depth, float spacing, uint32_t seed);

#endif
