// GrassSystem — GPU-culled instanced blades + static ground/daisies.
// Included from renderer.c (static functions, like nuklear_pass.inl).

#define GRASS_FIELD_SIZE   100.0f
#define GRASS_BLADE_COUNT  (512u * 1024u)
#define GRASS_DAISY_COUNT  2000u
#define GRASS_FADE_START   35.0f
#define GRASS_FADE_END     55.0f
#define GRASS_BLADE_RADIAL 7u
#define GRASS_BLADE_PROFILE 5u
#define GRASS_BLADE_VERTS  (GRASS_BLADE_RADIAL * GRASS_BLADE_PROFILE)
#define GRASS_BLADE_TRIS   (GRASS_BLADE_RADIAL * (GRASS_BLADE_PROFILE - 1) * 2u)

typedef struct GrassVertex {
    float position[3];
    float v;
    float u;
} GrassVertex;

typedef struct GrassInst {
    float x, y, z;
    float height;
    float width;
    uint32_t seed;
} GrassInst;
_Static_assert(sizeof(GrassInst) == 24, "GrassInst must be 24 bytes");

typedef struct GrassCullPush {
    uint64_t instances;
    uint64_t visible;
    uint64_t args;
    float    clip_rows[4][4];
    uint32_t instance_count;
    uint32_t visible_base;
    uint32_t frame_region;
    uint32_t pad0;
    float    fade_start;
    float    fade_end;
    float    field_radius;
    uint32_t pad1;
} GrassCullPush;

typedef struct GrassDrawPush {
    uint32_t visible_base;
    uint32_t mode;
    uint32_t pad0;
    uint32_t pad1;
} GrassDrawPush;

typedef struct GrassSystem {
    BufferSlice blade_verts;
    BufferSlice blade_indices;
    BufferSlice ground_verts;
    BufferSlice ground_indices;
    BufferSlice daisy_verts;
    BufferSlice daisy_indices;
    BufferSlice instances;
    BufferSlice visible;

    Buffer args[MAX_FRAMES_IN_FLIGHT];

    uint32_t blade_instance_count;
    uint32_t pipeline_gfx;
    uint32_t pipeline_cull;

    vec3  camera_pos;
    float camera_yaw;
    float camera_pitch;
}    GrassSystem ;

static float terrain_height(float x, float z) {
    return 1.5f * sinf(x * 0.08f) * cosf(z * 0.06f) + 0.8f * sinf(x * 0.03f + z * 0.05f);
}

static void grass_fill_blade_mesh(GrassVertex *verts, uint32_t *indices) {
    static const float radial[GRASS_BLADE_RADIAL][2] = {
        {0, 0}, {1, 0}, {0.5f, 0.8660254f}, {-0.5f, 0.8660254f},
        {-1, 0}, {-0.5f, -0.8660254f}, {0.5f, -0.8660254f}
    };
    static const float profile[GRASS_BLADE_PROFILE][2] = {
        {1.0f, 0.0f}, {0.9f, 0.4f}, {0.7f, 0.75f}, {0.4f, 0.95f}, {0.0f, 1.0f}
    };

    for (uint32_t p = 0; p < GRASS_BLADE_PROFILE; p++) {
        for (uint32_t r = 0; r < GRASS_BLADE_RADIAL; r++) {
            GrassVertex *v = &verts[p * GRASS_BLADE_RADIAL + r];
            v->position[0] = radial[r][0] * profile[p][0] * 0.5f;
            v->position[1] = profile[p][1];
            v->position[2] = radial[r][1] * profile[p][0] * 0.5f;
            v->v = profile[p][1];
            v->u = profile[p][0];
        }
    }

    uint32_t idx = 0;
    for (uint32_t p = 0; p + 1 < GRASS_BLADE_PROFILE; p++) {
        for (uint32_t r = 0; r < GRASS_BLADE_RADIAL; r++) {
            uint32_t r1 = (r + 1) % GRASS_BLADE_RADIAL;
            uint32_t a = p * GRASS_BLADE_RADIAL + r;
            uint32_t b = p * GRASS_BLADE_RADIAL + r1;
            uint32_t c = (p + 1) * GRASS_BLADE_RADIAL + r;
            uint32_t d = (p + 1) * GRASS_BLADE_RADIAL + r1;
            indices[idx++] = a; indices[idx++] = c; indices[idx++] = b;
            indices[idx++] = b; indices[idx++] = c; indices[idx++] = d;
        }
    }
}

static void grass_fill_ground_mesh(GrassVertex *verts, uint32_t *indices) {
    const float half = GRASS_FIELD_SIZE * 0.5f;
    const uint32_t grid = 32;
    const float step = GRASS_FIELD_SIZE / (float)grid;
    uint32_t vi = 0;
    for (uint32_t z = 0; z <= grid; z++) {
        for (uint32_t x = 0; x <= grid; x++) {
            float wx = -half + (float)x * step;
            float wz = -half + (float)z * step;
            verts[vi].position[0] = wx;
            verts[vi].position[1] = terrain_height(wx, wz);
            verts[vi].position[2] = wz;
            verts[vi].v = (float)z / (float)grid;
            verts[vi].u = (float)x / (float)grid;
            vi++;
        }
    }
    uint32_t ii = 0;
    for (uint32_t z = 0; z < grid; z++) {
        for (uint32_t x = 0; x < grid; x++) {
            uint32_t a = z * (grid + 1) + x;
            uint32_t b = a + 1;
            uint32_t c = a + (grid + 1);
            uint32_t d = c + 1;
            indices[ii++] = a; indices[ii++] = c; indices[ii++] = b;
            indices[ii++] = b; indices[ii++] = c; indices[ii++] = d;
        }
    }
}

// Daisies: one stem-quad + 8 petals + 8 center tris, all world-baked.
static void grass_fill_daisy_mesh(GrassVertex *verts, uint32_t *indices, uint32_t *out_vert_count, uint32_t *out_index_count) {
    static const float petals[8][2] = {
        {1, 0}, {0.7071f, 0.7071f}, {0, 1}, {-0.7071f, 0.7071f},
        {-1, 0}, {-0.7071f, -0.7071f}, {0, -1}, {0.7071f, -0.7071f}
    };
    const float half = GRASS_FIELD_SIZE * 0.5f;
    uint32_t vi = 0, ii = 0;

    for (uint32_t d = 0; d < GRASS_DAISY_COUNT; d++) {
        float rx = (float)((d * 1664525u + 1013904223u) & 0xFFFFu) / 65535.0f;
        float rz = (float)(((d * 22695477u + 1u) >> 8) & 0xFFFFu) / 65535.0f;
        float wx = (rx - 0.5f) * (GRASS_FIELD_SIZE - 4.0f);
        float wz = (rz - 0.5f) * (GRASS_FIELD_SIZE - 4.0f);
        float wy = terrain_height(wx, wz);
        float size = 0.10f + rx * 0.08f;
        float h = 0.12f + rz * 0.08f;
        uint32_t base = vi;

        // center
        verts[vi++] = (GrassVertex){{wx, wy + h, wz}, 0.5f, 1.0f};
        for (uint32_t p = 0; p < 8; p++) {
            float px = wx + petals[p][0] * size;
            float pz = wz + petals[p][1] * size;
            verts[vi++] = (GrassVertex){{px, wy + h, pz}, 0.0f, 1.0f};
        }
        for (uint32_t p = 0; p < 8; p++) {
            indices[ii++] = base;
            indices[ii++] = base + 1 + ((p + 1) % 8);
            indices[ii++] = base + 1 + p;
        }
    }
    *out_vert_count = vi;
    *out_index_count = ii;
}

static void grass_system_init(Renderer *r) {
    GrassSystem *g = (GrassSystem *)calloc(1, sizeof(GrassSystem));
    r->grass = g;

    g->camera_pos[0] = 0;
    g->camera_pos[1] = 3;
    g->camera_pos[2] = 35;
    g->camera_yaw   = 0;
    g->camera_pitch = -0.15f;

    const float half = GRASS_FIELD_SIZE * 0.5f;
    const uint32_t grid = 32;
    const float step = GRASS_FIELD_SIZE / (float)grid;

    uint32_t blade_vert_count = GRASS_BLADE_VERTS;
    uint32_t blade_index_count = GRASS_BLADE_TRIS * 3;
    GrassVertex *blade_verts = (GrassVertex *)malloc(blade_vert_count * sizeof(GrassVertex));
    uint32_t *blade_indices = (uint32_t *)malloc(blade_index_count * sizeof(uint32_t));
    grass_fill_blade_mesh(blade_verts, blade_indices);

    uint32_t ground_vert_count = (grid + 1) * (grid + 1);
    uint32_t ground_index_count = grid * grid * 6;
    GrassVertex *ground_verts = (GrassVertex *)malloc(ground_vert_count * sizeof(GrassVertex));
    uint32_t *ground_indices = (uint32_t *)malloc(ground_index_count * sizeof(uint32_t));
    grass_fill_ground_mesh(ground_verts, ground_indices);

    uint32_t daisy_vert_count = GRASS_DAISY_COUNT * 9;
    uint32_t daisy_index_count = GRASS_DAISY_COUNT * 24;
    GrassVertex *daisy_verts = (GrassVertex *)malloc(daisy_vert_count * sizeof(GrassVertex));
    uint32_t *daisy_indices = (uint32_t *)malloc(daisy_index_count * sizeof(uint32_t));
    uint32_t dv = 0, di = 0;
    grass_fill_daisy_mesh(daisy_verts, daisy_indices, &dv, &di);
    daisy_vert_count = dv;
    daisy_index_count = di;

    GrassInst *insts = (GrassInst *)malloc(GRASS_BLADE_COUNT * sizeof(GrassInst));
    uint32_t blade_i = 0;
    for (uint32_t cell = 0; cell < GRASS_BLADE_COUNT; cell++) {
        float fx = (float)(cell % 100u) - 50.0f;
        float fz = (float)((cell / 100u) % 100u) - 50.0f;
        float jx = ((float)((cell * 1664525u + 1013904223u) & 0xFFFFu) / 65535.0f - 0.5f) * 0.95f;
        float jz = ((float)(((cell * 22695477u + 1u) >> 8) & 0xFFFFu) / 65535.0f - 0.5f) * 0.95f;
        float wx = fx + jx;
        float wz = fz + jz;
        if (wx < -half || wx > half || wz < -half || wz > half)
            continue;

        GrassInst *inst = &insts[blade_i];
        float rnd = (float)((cell * 1664525u + 1013904223u) & 0xFFu) / 255.0f;
        inst->x = wx;
        inst->y = terrain_height(wx, wz);
        inst->z = wz;
        inst->height = 0.45f + rnd * 0.55f;
        inst->width  = 0.08f + rnd * 0.05f;
        inst->seed   = cell * 747796405u + 2891336453u;
        blade_i++;
        if (blade_i >= GRASS_BLADE_COUNT)
            break;
    }
    g->blade_instance_count = blade_i;

    VkCommandBuffer cmd = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);
    g->blade_verts    = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = blade_verts, .size = blade_vert_count * sizeof(GrassVertex)}, 16);
    g->blade_indices  = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = blade_indices, .size = blade_index_count * sizeof(uint32_t)}, 16);
    g->ground_verts   = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = ground_verts, .size = ground_vert_count * sizeof(GrassVertex)}, 16);
    g->ground_indices = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = ground_indices, .size = ground_index_count * sizeof(uint32_t)}, 16);
    g->daisy_verts    = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = daisy_verts, .size = daisy_vert_count * sizeof(GrassVertex)}, 16);
    g->daisy_indices  = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = daisy_indices, .size = daisy_index_count * sizeof(uint32_t)}, 16);
    g->instances      = renderer_upload_buffer(&r->vk, cmd, (ByteSpan){.data = insts, .size = blade_i * sizeof(GrassInst)}, 16);
    g->visible        = buffer_pool_alloc(&r->vk.gpu_pool, GRASS_BLADE_COUNT * sizeof(uint32_t), 16);
    vk_end_one_time_cmd(r->vk.devc.device, r->vk.devc.graphics_queue, r->vk.one_time_gfx_pool, cmd);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        create_buffer(&r->vk, sizeof(VkDrawIndexedIndirectCommand),
                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VMA_MEMORY_USAGE_CPU_TO_GPU, &g->args[i]);
    }

    GraphicsPipelineConfig gfx = pipeline_config_default();
    gfx.vert_path = "compiledshaders/grass.vert.spv";
    gfx.frag_path = "compiledshaders/grass.frag.spv";
    gfx.color_attachment_count = 1;
    gfx.color_formats = &r->hdr_color[0].format;
    gfx.depth_format = r->depth[0].format;
    gfx.depth_compare_op = VK_COMPARE_OP_LESS;
    gfx.depth_test_enable = true;
    gfx.depth_write_enable = true;
    g->pipeline_gfx = pipeline_create_graphics(&r->vk, &gfx);
    g->pipeline_cull = pipeline_create_compute(&r->vk, "compiledshaders/grass.comp.spv");

    free(blade_verts); free(blade_indices);
    free(ground_verts); free(ground_indices);
    free(daisy_verts); free(daisy_indices);
    free(insts);

    log_info("[grass] %u blades, %u daisies", g->blade_instance_count, GRASS_DAISY_COUNT);
}

static void grass_camera_update(Renderer *r) {
    GrassSystem *g = r->grass;
    if (!g)
        return;
    Input *in = &r->input;
    float dt = r->dt > 0 ? r->dt : 0.016f;
    float speed = (key_down(in, KEY_LEFT_SHIFT) ? 18.0f : 6.0f) * dt;

    if (mouse_down(in, MOUSE_RIGHT) && !mouse_pressed(in, MOUSE_RIGHT)) {
        g->camera_yaw += (float)mouse_dx(in) * 0.003f;
        g->camera_pitch -= (float)mouse_dy(in) * 0.003f;
        g->camera_pitch = CLAMP(g->camera_pitch, -1.5f, 1.5f);
    }

    float cy = cosf(g->camera_yaw), sy = sinf(g->camera_yaw);
    float cp = cosf(g->camera_pitch), sp = sinf(g->camera_pitch);
    vec3 forward = {sy * cp, sp, -cy * cp};
    vec3 right = {cy, 0, sy};

    float ax = input_axis(in, KEY_A, KEY_D);
    float az = input_axis(in, KEY_S, KEY_W);
    float ay = input_axis(in, KEY_LEFT_CTRL, KEY_SPACE);
    g->camera_pos[0] += (right[0] * ax + forward[0] * az) * speed;
    g->camera_pos[1] += (forward[1] * az) * speed + ay * speed;
    g->camera_pos[2] += (right[2] * ax + forward[2] * az) * speed;
}

static void grass_system_update_global(Renderer *r, GlobalData *data) {
    GrassSystem *g = r->grass;
    if (!g)
        return;
    vec3 up = {0, 1, 0};
    float cy = cosf(g->camera_yaw), sy = sinf(g->camera_yaw);
    float cp = cosf(g->camera_pitch), sp = sinf(g->camera_pitch);
    vec3 fwd = {sy * cp, sp, -cy * cp};
    vec3 center = {g->camera_pos[0] + fwd[0], g->camera_pos[1] + fwd[1], g->camera_pos[2] + fwd[2]};
    glm_lookat(g->camera_pos, center, up, data->view);

    float aspect = (float)r->vk.swapchain.extent.width / (float)r->vk.swapchain.extent.height;
    float f = 1.0f / tanf(0.9f * 0.5f);
    float n = 0.1f, fa = 200.0f;
    memset(data->projection, 0, sizeof(data->projection));
    data->projection[0][0] = f / aspect;
    data->projection[1][1] = -f;
    data->projection[2][2] = fa / (n - fa);
    data->projection[2][3] = -1.0f;
    data->projection[3][2] = (n * fa) / (n - fa);

    glm_mat4_mul(data->projection, data->view, data->viewproj);
    glm_mat4_inv(data->view, data->inv_view);
    glm_mat4_inv(data->projection, data->inv_projection);
    glm_mat4_inv(data->viewproj, data->inv_viewproj);

    data->camera_pos[0] = g->camera_pos[0];
    data->camera_pos[1] = g->camera_pos[1];
    data->camera_pos[2] = g->camera_pos[2];
    data->camera_pos[3] = 0.9f;
    data->camera_dir[0] = fwd[0];
    data->camera_dir[1] = fwd[1];
    data->camera_dir[2] = fwd[2];
    data->camera_dir[3] = aspect;
}

static void grass_pass(Renderer *r, VkCommandBuffer cmd) {
    GrassSystem *g = r->grass;
    if (!g || g->blade_instance_count == 0)
        return;

    GpuProfiler *prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(prof, cmd, "Grass",
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) {

        uint32_t frame = r->vk.current_frame;
        Buffer *args = &g->args[frame];
        VkDrawIndexedIndirectCommand *cmd_args = (VkDrawIndexedIndirectCommand *)args->mapping;
        cmd_args[0] = (VkDrawIndexedIndirectCommand){
            .indexCount = GRASS_BLADE_TRIS * 3,
            .instanceCount = 0,
            .firstIndex = 0,
            .vertexOffset = 0,
            .firstInstance = 0
        };
        vmaFlushAllocation(r->vk.devc.vmaallocator, args->allocation, 0, sizeof(*cmd_args));

        float rows[4][4];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                rows[i][j] = r->grass_viewproj[j][i];

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          r->vk.render_pipelines.pipelines[g->pipeline_cull - 1]);

        GrassCullPush cull = {
            .instances = r->vk.gpu_base_addr + g->instances.offset,
            .visible = r->vk.gpu_base_addr + g->visible.offset,
            .args = args->address,
            .instance_count = g->blade_instance_count,
            .visible_base = 0,
            .frame_region = 0,
            .fade_start = GRASS_FADE_START,
            .fade_end = GRASS_FADE_END,
            .field_radius = GRASS_FIELD_SIZE * 0.5f
        };
        memcpy(cull.clip_rows, rows, sizeof(rows));
        dispatch_push(&r->vk, cmd, BYTE_SPAN(cull), (g->blade_instance_count + 63) / 64, 1, 1);

        cmd_buffer_barrier(cmd, args->buffer, 0, sizeof(VkDrawIndexedIndirectCommand),
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);

        PassAttachment color = {
            .target = &r->hdr_color[r->vk.swapchain.current_image],
            .load = LOAD_CLEAR,
            .clear = {0.45f, 0.70f, 0.95f, 1.0f},
        };
        PassAttachment depth = {
            .target = &r->depth[r->vk.swapchain.current_image],
            .load = LOAD_CLEAR,
            .clear = {1.0f, 0, 0, 0},
        };
        begin_pass(&r->vk, cmd, &(PassDesc){
            .colors = &color, .color_count = 1, .depth = &depth, .pipeline = g->pipeline_gfx
        });

        // Ground (world-space)
        vkCmdBindVertexBuffers(cmd, 0, 1, &g->ground_verts.buffer, &(VkDeviceSize){g->ground_verts.offset});
        vkCmdBindIndexBuffer(cmd, g->ground_indices.buffer, g->ground_indices.offset, VK_INDEX_TYPE_UINT32);
        GrassDrawPush ground_push = {.visible_base = 0, .mode = 1};
        push_constants(&r->vk, cmd, BYTE_SPAN(ground_push));
        vkCmdDrawIndexed(cmd, 32 * 32 * 6, 1, 0, 0, 0);

        // Daisies (world-space)
        vkCmdBindVertexBuffers(cmd, 0, 1, &g->daisy_verts.buffer, &(VkDeviceSize){g->daisy_verts.offset});
        vkCmdBindIndexBuffer(cmd, g->daisy_indices.buffer, g->daisy_indices.offset, VK_INDEX_TYPE_UINT32);
        GrassDrawPush daisy_push = {.visible_base = 0, .mode = 1};
        push_constants(&r->vk, cmd, BYTE_SPAN(daisy_push));
        vkCmdDrawIndexed(cmd, GRASS_DAISY_COUNT * 24, 1, 0, 0, 0);

        // Blades (GPU-culled indirect)
        vkCmdBindVertexBuffers(cmd, 0, 1, &g->blade_verts.buffer, &(VkDeviceSize){g->blade_verts.offset});
        vkCmdBindIndexBuffer(cmd, g->blade_indices.buffer, g->blade_indices.offset, VK_INDEX_TYPE_UINT32);
        GrassDrawPush blade_push = {.visible_base = 0, .mode = 0};
        push_constants(&r->vk, cmd, BYTE_SPAN(blade_push));
        vkCmdDrawIndexedIndirect(cmd, args->buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));

        end_pass(cmd);
    }
}

struct GrassSystem *renderer_get_grass_system(Renderer *renderer) {
    return renderer->grass;
}
