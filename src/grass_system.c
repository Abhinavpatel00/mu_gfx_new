// Grass system implementation for stylized cartoon grass

#include "grass_system.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "external/mu/mu.h"

// Function implementations for grass system

// Create and initialize a grass system
GrassSystem* grass_system_create(void) {
    GrassSystem *sys = (GrassSystem*)mu_alloc(sizeof(GrassSystem));
    if (!sys) {
        return NULL;
    }
    memset(sys, 0, sizeof(GrassSystem));
    sys->max_instances = GRASS_MAX_INSTANCES;
    return sys;
}

// Destroy the grass system
void grass_system_destroy(GrassSystem *sys) {
    if (!sys) {
        return;
    }
    mu_free(sys);
}

// Add a grass blade instance
int grass_system_add(GrassSystem *sys, const GrassInst *inst) {
    if (!sys || !inst || sys->count >= sys->max_instances) {
        return -1;
    }
    sys->instances[sys->count++] = *inst;
    return (int)sys->count - 1;
}

// Remove a grass blade instance by index
void grass_system_remove(GrassSystem *sys, uint32_t idx) {
    if (!sys || idx >= sys->count) {
        return;
    }
    sys->instances[idx] = sys->instances[sys->count - 1];
    sys->count--;
}

// Get a grass blade instance by index
const GrassInst* grass_system_get(GrassSystem *sys, uint32_t idx) {
    if (!sys || idx >= sys->count) {
        return NULL;
    }
    return &sys->instances[idx];
}

// Generate a field of grass with specified density and colors
void grass_system_generate_field(GrassSystem *sys, float width, float depth,
                                 float density, uint32_t seed) {
    if (!sys) {
        return;
    }
    
    // Colors for stylized cartoon grass
    float base_r = 0.15f, base_g = 0.35f, base_b = 0.1f;   // Dark green base
    float top_r = 0.4f, top_g = 0.85f, top_b = 0.25f;      // Bright green tip
    
    // Daisy colors
    float daisy_petal_r = 1.0f, daisy_petal_g = 1.0f, daisy_petal_b = 1.0f;  // White petals
    float daisy_center_r = 1.0f, daisy_center_g = 0.9f, daisy_center_b = 0.0f; // Yellow center
    
    // Simplified grass generation algorithm
    float spacing = 1.0f / density;
    int grid_x = (int)(width * density);
    int grid_z = (int)(depth * density);
    
    for (int x = 0; x < grid_x && sys->count < sys->max_instances; x++) {
        for (int z = 0; z < grid_z && sys->count < sys->max_instances; z++) {
            // Calculate random position with variations
            float px = ((float)x + 0.5f + ((seed * 0.1343f + (float)x * 0.0378f) - (float)int((seed * 0.5678f + (float)z * 0.2345f)))) * spacing - width * 0.5f;
            float pz = ((float)z + 0.5f + ((seed * 0.9876f + (float)z * 0.6543f) - (float)int((seed * 0.8765f + (float)x * 0.4532f)))) * spacing - depth * 0.5f;
            float py = 0.0f;  // Ground level
            
            // Random blade properties
            float scale_x = 0.8f + sin((float)x * 0.3f + (float)z * 0.7f) * 0.4f;
            float scale_y = 0.8f + sin((float)x * 0.5f + (float)z * 0.3f) * 0.4f;
            
            // Random wind properties for animation
            float wind_speed = 0.5f + sin((float)seed + (float)x * 0.5f) * 0.5f;
            float phase = sin((float)x * 0.2f + (float)z * 0.7f) * 3.14f;
            
            // Determine if this is a daisy (about 5% chance)
            bool is_daisy = sin((float)seed * 0.137f + (float)x * 0.673f + (float)z * 0.218f) > 0.95f;
            
            // Set grass instance
            GrassInst inst;
            inst.position_x = px;
            inst.position_y = py;
            inst.position_z = pz;
            inst.pad0 = 0.0f;
            inst.rotation_x = 0.0f;
            inst.rotation_y = 0.0f;
            inst.rotation_z = 0.0f;
            inst.rotation_w = 1.0f;
            inst.scale_x = scale_x;
            inst.scale_y = scale_y;
            inst.pad1 = 0.0f;
            inst.color_base_r = is_daisy ? daisy_petal_r : base_r;
            inst.color_base_g = is_daisy ? daisy_petal_g : base_g;
            inst.color_base_b = is_daisy ? daisy_petal_b : base_b;
            inst.pad2 = 0.0f;
            inst.color_top_r = is_daisy ? daisy_center_r : top_r;
            inst.color_top_g = is_daisy ? daisy_center_g : top_g;
            inst.color_top_b = is_daisy ? daisy_center_b : top_b;
            inst.pad3 = is_daisy ? 1.0f : 0.0f;  // Mark as daisy with pad3
            inst.wind_speed = wind_speed;
            inst.phase = phase;
            inst.pad4 = 0.0f;
            inst.pad5 = 0.0f;
            
            // Add to system
            grass_system_add(sys, &inst);
        }
    }
}

// Update wind animation for all grass blades
void grass_system_update_wind(GrassSystem *sys, float dt, float time) {
    if (!sys) {
        return;
    }
    
    for (uint32_t i = 0; i < sys->count; i++) {
        GrassInst *inst = &sys->instances[i];
        
        // Simple wind animation: rotate blade based on time and phase
        float wind_angle = sin(time * inst->wind_speed + inst->phase) * 0.15f;
        
        // Convert to quaternion for rotation
        float half_angle = wind_angle * 0.5f;
        inst->rotation_z = sin(half_angle);
        inst->rotation_w = cos(half_angle);
        inst->rotation_x = 0.0f;
        inst->rotation_y = 0.0f;
    }
}