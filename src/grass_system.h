#ifndef GRASS_SYSTEM_H
#define GRASS_SYSTEM_H

#include <stdint.h>
#include <stddef.h>

// Forward declaration
struct GrassSystem;

// Grass instance definition - matches GrassInst in the shader
struct GrassInst {
    float position_x;
    float position_y;
    float position_z;
    float pad0;
    float rotation_x;
    float rotation_y;
    float rotation_z;
    float rotation_w;
    float scale_x;
    float scale_y;
    float pad1;
    float color_base_r;
    float color_base_g;
    float color_base_b;
    float pad2;
    float color_top_r;
    float color_top_g;
    float color_top_b;
    float pad3;
    float wind_speed;
    float phase;
    float pad4;
    float pad5;
};

// Maximum number of grass instances
#define GRASS_MAX_INSTANCES 1024

// Grass system - manages grass blade instances
typedef struct {
    GrassInst instances[GRASS_MAX_INSTANCES];
    uint32_t count;
    uint32_t max_instances;
} GrassSystem;

// Create and initialize a grass system
GrassSystem* grass_system_create(void);

// Destroy the grass system
void grass_system_destroy(GrassSystem *sys);

// Add a grass blade instance, returns index or -1 on failure
int grass_system_add(GrassSystem *sys, const GrassInst *inst);

// Remove a grass blade instance by index
void grass_system_remove(GrassSystem *sys, uint32_t idx);

// Get a grass blade instance by index
const GrassInst* grass_system_get(GrassSystem *sys, uint32_t idx);

// Generate a field of grass with specified density and colors
void grass_system_generate_field(GrassSystem *sys, float width, float depth,
                                  float density, uint32_t seed);

// Update wind animation for all grass blades
void grass_system_update_wind(GrassSystem *sys, float dt, float time);

#endif // GRASS_SYSTEM_H
