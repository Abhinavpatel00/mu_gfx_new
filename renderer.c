#include "renderer.h"
#include "src/platform.h"
#include "renderer3d.h"
#include "src/input_rgfw.h"
#include "src/nuklear_ui.h"
#include "src/slangtypes.h"
#include "external/dmon/dmon.h"
#include "external/stb/stb_image.h"
#include "external/stb/stb_image_write.h"
#include "external/mu/mu/mu_perf.h"

typedef struct CaptureState {
    Buffer   readback[CAPTURE_SLOTS];
    uint64_t submit_value[CAPTURE_SLOTS]; // timeline value of the submission carrying the copy; 0 = idle
    bool     shot_slot[CAPTURE_SLOTS];    // save a PNG when consumed
    bool     video_slot[CAPTURE_SLOTS];   // feed ffmpeg when consumed
    char     shot_path[CAPTURE_SLOTS][512];

    uint32_t     width;
    uint32_t     height;
    VkDeviceSize bytes_per_frame;
    bool         src_is_bgra;

    // video
    bool     recording;
    FILE    *pipe;
    uint32_t fps;
    uint64_t frames_written;

    // pending request (set from main thread / UI)
    bool screenshot_pending;
    char screenshot_path[512];

    uint8_t *png_scratch;

    bool inited;
} CaptureState;
typedef struct NuklearUi {
    struct nk_context           context;
    struct nk_font_atlas        atlas;
    struct nk_draw_null_texture null_texture;
    struct nk_buffer            commands;
    struct nk_buffer            vertices;
    struct nk_buffer            indices;
    RenderTarget                font;
    Buffer                      uploads[MAX_FRAMES_IN_FLIGHT];
    VkPipeline                  pipeline;
    uint32_t                    vertex_count;
    uint32_t                    index_count;
    uint32_t                    draw_count;
    float                       width;
    float                       height;
} NuklearUi;


struct Renderer {
    VkBackend vk;
    Renderer3D scene;
    double   cpu_frame_ns;
    uint64_t start_time;
    double   cpu_active_ns;
    double   cpu_wait_ns;
    double   cpu_wait_accum_ns;
    uint64_t cpu_prev_frame;
    uint32_t frame_count;
    float    dt;
    RGFW_window           *window;
    Input                  input;
    NuklearUi        ui;
    RenderTarget depth[MAX_SWAPCHAIN_IMAGES];
    RenderTarget hdr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget ldr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_final[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_edges[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_weights[MAX_SWAPCHAIN_IMAGES];
    TextureID dummy_texture;
    TextureID smaa_area_tex;
    TextureID smaa_search_tex;
    struct {
        uint32_t smaa_edge;
        uint32_t smaa_weight;
        uint32_t smaa_blend;
    } smaa_pipelines;
    Buffer global_ubo[MAX_FRAMES_IN_FLIGHT];
    Buffer            readback_buffer;
    CaptureState      capture;
    struct {
        uint32_t fullscreen;
        uint32_t postprocess;
        uint32_t gltf_minimal;
        uint32_t fire;
        uint32_t sprite;
        uint32_t slug_text;

        uint32_t beam;
        uint32_t sky;
        uint32_t skinning;
    } EnginePipelines;
};

static bool trigger_shader_compilation(void) {
    // Use system() to run the bash script
    // Note: system() blocks until the process finishes. 
    // For a quick compilation script, this is acceptable, 
    // but for better UX, consider using popen() or a thread.
    // Since dmon callback is already on a thread, blocking there is okay.
    // But if the script is slow, it might queue up. We'll use system() for simplicity.
    printf("[HotReload] Triggering shader compilation...\n");
    int result = system("bash compileslang.sh"); // Adjust path if needed
    if (result != 0) {
        fprintf(stderr, "[HotReload] compileslang.sh failed with code %d\n", result);
        return false;
    }
    return true;
}


static dmon_watch_id g_source_watch_id;

static void watch_callback(dmon_watch_id watch_id, dmon_action action, const char *rootdir, const char *filepath,
                           const char *oldfilepath, void *user) {
    Renderer *r = (Renderer *)user;

    if (action != DMON_ACTION_CREATE && action != DMON_ACTION_MODIFY && action != DMON_ACTION_MOVE) {
        return;
    }

    // --- Handle Source Shader Changes (e.g., in "shaders/") ---
    if (watch_id.id == g_source_watch_id.id) {
        // Check if it's a source file (e.g., .slang, .vert, .frag)
        const char *ext = strrchr(filepath, '.');
        if (ext && (strcmp(ext, ".slang") == 0 || strcmp(ext, ".vert") == 0 || strcmp(ext, ".frag") == 0)) {
            // Trigger the bash script
            if (trigger_shader_compilation()) {
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", rootdir, filepath);
                pipeline_mark_dirty(&r->vk, full_path);
            }
        }
    }
    // --- Handle Compiled Shader Changes (e.g., in "compiledshaders/") ---
    else {
        // Check if it's a .spv file
        const char *ext = strrchr(filepath, '.');
        if (!ext || strcmp(ext, ".spv") != 0)
            return;

        // Construct path and mark dirty
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", rootdir, filepath);
        pipeline_mark_dirty(&r->vk, full_path);
    }
}

static bool capture_alloc_slot(Renderer *r, Buffer *b, VkDeviceSize size) {
    VkBufferCreateInfo bi = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo ai = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VmaAllocationInfo info;
    if (vmaCreateBuffer(r->vk.devc.vmaallocator, &bi, &ai, &b->buffer, &b->allocation, &info) != VK_SUCCESS)
        return false;
    b->buffer_size = size;
    b->mapping     = info.pMappedData;
    b->address     = 0;
    return true;
}

static void capture_free_slot(Renderer *r, Buffer *b) {
    if (b->buffer)
        vmaDestroyBuffer(r->vk.devc.vmaallocator, b->buffer, b->allocation);
    memset(b, 0, sizeof(*b));
}

void capture_init(Renderer *r, uint32_t w, uint32_t h) {
    CaptureState *c = &r->capture;
    if (c->inited || w == 0 || h == 0)
        return;

    memset(c, 0, sizeof(*c));
    c->width           = w;
    c->height          = h;
    c->fps             = 60;
    c->bytes_per_frame = (VkDeviceSize)w * h * 4;
    c->src_is_bgra =
        (r->vk.swapchain.format == VK_FORMAT_B8G8R8A8_SRGB || r->vk.swapchain.format == VK_FORMAT_B8G8R8A8_UNORM);

    for (uint32_t i = 0; i < CAPTURE_SLOTS; i++) {
        if (!capture_alloc_slot(r, &c->readback[i], c->bytes_per_frame))
            log_error("[capture] slot %u alloc failed", i);
    }

    c->png_scratch = malloc((size_t)w * h * 4);
    c->inited      = true;

    log_info("[capture] ready %ux%u (%d slots, bgra=%d)", w, h, CAPTURE_SLOTS, (int)c->src_is_bgra);
}

void capture_shutdown(Renderer *r) {
    CaptureState *c = &r->capture;
    if (!c->inited)
        return;

    if (c->recording) {
        pclose(c->pipe);
        c->pipe      = NULL;
        c->recording = false;
    }

    // Intentional stall: shutdown has no frames left to overlap with.
    vkDeviceWaitIdle(r->vk.devc.device);
    for (uint32_t i = 0; i < CAPTURE_SLOTS; i++)
        capture_free_slot(r, &c->readback[i]);

    free(c->png_scratch);
    memset(c, 0, sizeof(*c));
}

void capture_resize(Renderer *r, uint32_t w, uint32_t h) {
    CaptureState *c = &r->capture;
    if (!c->inited || (w == c->width && h == c->height))
        return;

    if (c->recording) {
        log_warn("[capture] resize during recording; stopping");
        pclose(c->pipe);
        c->pipe      = NULL;
        c->recording = false;
    }

    // Intentional stall: readback slots are recreated in place and are rare
    // (window resize only); deferral would outlive the CaptureState bookkeeping.
    vkDeviceWaitIdle(r->vk.devc.device);
    for (uint32_t i = 0; i < CAPTURE_SLOTS; i++) {
        capture_free_slot(r, &c->readback[i]);
        capture_alloc_slot(r, &c->readback[i], (VkDeviceSize)w * h * 4);
    }
    free(c->png_scratch);
    c->png_scratch = malloc((size_t)w * h * 4);

    c->width           = w;
    c->height          = h;
    c->bytes_per_frame = (VkDeviceSize)w * h * 4;
}

// ---- public API ----------------------------------------------

bool capture_take_screenshot(Renderer *r, const char *path) {
    CaptureState *c = &r->capture;
    if (!c->inited || !path)
        return false;
    strncpy(c->screenshot_path, path, sizeof(c->screenshot_path) - 1);
    c->screenshot_path[sizeof(c->screenshot_path) - 1] = '\0';
    c->screenshot_pending                              = true;
    return true;
}

bool capture_start_video(Renderer *r, const char *path, uint32_t fps) {
    CaptureState *c = &r->capture;
    if (!c->inited || c->recording)
        return false;

    // libx264 requires even width/height. Pad to the next even value if needed.
    // The readback buffer is still c->width x c->height; ffmpeg pads the frame
    // on its side after receiving it.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -loglevel error "
             "-f rawvideo -pix_fmt %s -s %ux%u -r %u -i - "
             "-vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" "
             "-c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p \"%s\"",
             c->src_is_bgra ? "bgra" : "rgba",
             c->width, c->height, fps, path);

    FILE *p = popen(cmd, "w");
    if (!p) {
        log_error("[capture] ffmpeg not found on PATH");
        return false;
    }

    c->pipe           = p;
    c->recording      = true;
    c->fps            = fps;
    c->frames_written = 0;
    log_info("[capture] recording -> %s (%ux%u @ %u fps)", path, c->width, c->height, fps);
    return true;
}
void capture_stop_video(Renderer *r) {
    CaptureState *c = &r->capture;
    if (!c->recording)
        return;
    c->recording = false;
    if (c->pipe) {
        pclose(c->pipe);
        c->pipe = NULL;
    }
    log_info("[capture] stopped (%llu frames)", (unsigned long long)c->frames_written);
}

// ---- per-frame hooks -----------------------------------------

// Call in frame_start() after vkWaitForFences() succeeded.
// The copy recorded MAX_FRAMES_IN_FLIGHT frames ago on this slot has now completed.
static void capture_consume(Renderer *r) {
    CaptureState *c = &r->capture;
    if (!c->inited)
        return;

    // Consume the slot recorded MAX_FRAMES_IN_FLIGHT - 1 submissions ago; the
    // frame-slot timeline wait in frame_start already covered its submission.
    // The submit_value check makes that coupling explicit and enforced: a slot
    // is readable only when its covering timeline value has completed.
    uint32_t slot = (r->vk.current_frame + CAPTURE_SLOTS - 1) % CAPTURE_SLOTS;
    if (c->submit_value[slot] == 0)
        return;

    uint64_t completed = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(r->vk.devc.device, r->vk.timeline, &completed));
    if (completed < c->submit_value[slot]) {
        if (c->shot_slot[slot])
            log_warn("[capture] screenshot deferred: submission %llu still in flight",
                     (unsigned long long)c->submit_value[slot]);
        return;
    }

    Buffer *b = &c->readback[slot];
    if (!b->mapping)
        return;

    vmaInvalidateAllocation(r->vk.devc.vmaallocator, b->allocation, 0, VK_WHOLE_SIZE);
    const uint8_t *src = (const uint8_t *)b->mapping;

    if (c->shot_slot[slot]) {
        if (c->src_is_bgra) {
            size_t npix = (size_t)c->width * (size_t)c->height;
            for (size_t i = 0; i < npix; i++) {
                c->png_scratch[i * 4 + 0] = src[i * 4 + 2];
                c->png_scratch[i * 4 + 1] = src[i * 4 + 1];
                c->png_scratch[i * 4 + 2] = src[i * 4 + 0];
                c->png_scratch[i * 4 + 3] = 255; // swapchain alpha is undefined; screenshots must stay opaque
            }
            stbi_write_png(c->shot_path[slot], (int)c->width, (int)c->height, 4, c->png_scratch, (int)(c->width * 4));
        } else {
            size_t npix = (size_t)c->width * (size_t)c->height;
            memcpy(c->png_scratch, src, npix * 4);
            for (size_t i = 0; i < npix; i++)
                c->png_scratch[i * 4 + 3] = 255; // swapchain alpha is undefined; screenshots must stay opaque
            stbi_write_png(c->shot_path[slot], (int)c->width, (int)c->height, 4, c->png_scratch, (int)(c->width * 4));
        }
        log_info("[capture] screenshot: %s", c->shot_path[slot]);
        c->shot_slot[slot] = false;
    }

    if (c->video_slot[slot] && c->pipe) {
        fwrite(src, 1, (size_t)c->bytes_per_frame, c->pipe);
        c->frames_written++;
        c->video_slot[slot] = false;
    }

    c->submit_value[slot] = 0;
}

// Call right after pass_nuklear(), before the swapchain is transitioned
// to PRESENT_SRC. Handles the temporary TRANSFER_SRC layout ourselves.
static void capture_record(Renderer *r, VkCommandBuffer cmd) {
    CaptureState *c = &r->capture;
    if (!c->inited)
        return;

    uint32_t slot = r->vk.current_frame % CAPTURE_SLOTS;

    bool want_shot  = c->screenshot_pending;
    bool want_video = c->recording;
    if (!want_shot && !want_video)
        return;

    if (c->submit_value[slot] != 0) {
        // The slot's previous copy is not consumed yet; recording into it would
        // overwrite data the GPU may still be writing. Requested work is not
        // silently lost: a pending screenshot is logged when it is skipped.
        if (want_shot)
            log_warn("[capture] screenshot dropped: readback slot %u busy", slot);
        return;
    }

    if (c->width != r->vk.swapchain.extent.width || c->height != r->vk.swapchain.extent.height)
        return;

    uint32_t image = r->vk.swapchain.current_image;
    VkImage  src   = r->vk.swapchain.images[image];

    ImageState           *st          = &r->vk.swapchain.states[image];
    VkImageLayout         prev_layout = st->layout;
    VkPipelineStageFlags2 prev_stage  = st->stage;
    VkAccessFlags2        prev_access = st->access;

    VkImageMemoryBarrier2 to_src = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = prev_stage ? prev_stage : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = prev_access ? prev_access : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout           = prev_layout,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = src,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &to_src,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region = {
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel   = 0,
                .layerCount = 1,
            },
        .imageExtent = {c->width, c->height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->readback[slot].buffer, 1, &region);

    VkImageMemoryBarrier2 to_att = to_src;
    to_att.srcStageMask          = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_att.srcAccessMask         = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_att.dstStageMask          = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_att.dstAccessMask         = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_att.oldLayout             = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_att.newLayout             = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dep.pImageMemoryBarriers     = &to_att;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Restore the tracker so the later transition to PRESENT sees the state
    // it would have seen without us.
    st->layout = prev_layout;
    st->stage  = prev_stage;
    st->access = prev_access;

    // capture_record runs before this frame's single submit, so the next
    // timeline value is exactly the submission that will carry this copy.
    c->submit_value[slot] = r->vk.timeline_last_submitted + 1;
    c->shot_slot[slot]    = want_shot;
    c->video_slot[slot]   = want_video;

    if (want_shot) {
        strncpy(c->shot_path[slot], c->screenshot_path, sizeof(c->shot_path[slot]) - 1);
        c->shot_path[slot][sizeof(c->shot_path[slot]) - 1] = '\0';
        c->screenshot_pending                              = false;
    }
}
#include "src/nuklear_renderer.inl"


static void renderer_resources_create(Renderer *r, VkBackendDesc *desc) {
    VkFormat depth_format = pick_depth_format(r->vk.devc.physical_device);
    VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    capture_init(r, r->vk.swapchain.extent.width, r->vk.swapchain.extent.height);

    RenderTargetSpec depth_spec = {.width  = r->vk.swapchain.extent.width,
                                   .height = r->vk.swapchain.extent.height,
                                   .layers = 1,

                                   .format = depth_format,

                                   .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                   .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "depth_buffer"};
    RenderTargetSpec hdr_spec   = {.width      = r->vk.swapchain.extent.width,
                                   .height     = r->vk.swapchain.extent.height,
                                   .layers     = 1,
                                   .format     = hdr_format,
                                   .usage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                   .mip_count  = 1,
                                   .debug_name = "hdr_color"};
    RenderTargetSpec ldr_spec   = {.width  = r->vk.swapchain.extent.width,
                                   .height = r->vk.swapchain.extent.height,
                                   .layers = 1,

                                   .format = VK_FORMAT_R8G8B8A8_UNORM,

                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |

                                            VK_IMAGE_USAGE_STORAGE_BIT |      // compute writes tonemap result
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | // copy → swapchain
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | // optional clears
                                            VK_IMAGE_USAGE_SAMPLED_BIT,       // future post effects

                                   .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "ldr_color"};
    RenderTargetSpec smaa_final_spec = ldr_spec;
    smaa_final_spec.debug_name       = "smaa_final";

    RenderTargetSpec smaa_edge_spec = {.width  = r->vk.swapchain.extent.width,
                                       .height = r->vk.swapchain.extent.height,
                                       .layers = 1,

                                       .format = VK_FORMAT_R8G8_UNORM,

                                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                       .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                       .mip_count  = 1,
                                       .debug_name = "smaa_edges"};

    RenderTargetSpec smaa_weight_spec = {.width  = r->vk.swapchain.extent.width,
                                         .height = r->vk.swapchain.extent.height,
                                         .layers = 1,

                                         .format = VK_FORMAT_R8G8B8A8_UNORM,

                                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                         .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                         .mip_count  = 1,
                                         .debug_name = "smaa_weights"};


#include "external/smaa/Textures/AreaTex.h"

#include "external/smaa/Textures/SearchTex.h"

    {

        TextureID smaa_area;
        TextureID smaa_search;

        {
            TextureCreateDesc desc = {.width     = AREATEX_WIDTH,
                                      .height    = AREATEX_HEIGHT,
                                      .mip_count = 1,
                                      .format    = VK_FORMAT_R8G8_UNORM,
                                      .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

            smaa_area    = create_texture(&r->vk, &desc);
            Texture *tex = &r->vk.texture_system.textures[smaa_area];

            VkDeviceSize size = AREATEX_SIZE;

            Buffer staging;
            create_buffer(&r->vk, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, areaTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcAccessMask               = 0,
                                            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .image                       = tex->image,
                                            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .subresourceRange.levelCount = 1,
                                            .subresourceRange.layerCount = 1};

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &barrier);

            VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            vk_end_one_time_cmd(r->vk.devc.device, r->vk.devc.graphics_queue, r->vk.one_time_gfx_pool, cmd);

            destroy_buffer(&r->vk, &staging);
        }

        {
            TextureCreateDesc desc = {.width     = SEARCHTEX_WIDTH,
                                      .height    = SEARCHTEX_HEIGHT,
                                      .mip_count = 1,
                                      .format    = VK_FORMAT_R8_UNORM,
                                      .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

            smaa_search  = create_texture(&r->vk, &desc);
            Texture *tex = &r->vk.texture_system.textures[smaa_search];

            VkDeviceSize size = SEARCHTEX_SIZE;

            Buffer staging;
            create_buffer(&r->vk, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, searchTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcAccessMask               = 0,
                                            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .image                       = tex->image,
                                            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .subresourceRange.levelCount = 1,
                                            .subresourceRange.layerCount = 1};

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &barrier);

            VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            vk_end_one_time_cmd(r->vk.devc.device, r->vk.devc.graphics_queue, r->vk.one_time_gfx_pool, cmd);

            destroy_buffer(&r->vk, &staging);
        }

        r->smaa_area_tex   = smaa_area;
        r->smaa_search_tex = smaa_search;
    }
    forEach(i, r->vk.swapchain.image_count) {
        rt_create(&r->vk, &r->depth[i], &depth_spec);
        rt_create(&r->vk, &r->hdr_color[i], &hdr_spec);
        rt_create(&r->vk, &r->ldr_color[i], &ldr_spec);
        rt_create(&r->vk, &r->smaa_final[i], &smaa_final_spec);

        rt_create(&r->vk, &r->smaa_edges[i], &smaa_edge_spec);
        rt_create(&r->vk, &r->smaa_weights[i], &smaa_weight_spec);
    }
    {
        const char *path = "data/dummy_texture.png";

        int            w, h, c;
        unsigned char *pixels = stbi_load(path, &w, &h, &c, 4);
        if (!pixels) {
            fprintf(stderr, "Failed to load %s\n", path);
            exit(EXIT_FAILURE);
        }
        TextureCreateDesc desc = {.width     = w,
                                  .height    = h,
                                  .mip_count = 1,
                                  .format    = VK_FORMAT_R8G8B8A8_UNORM,
                                  .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                               VK_IMAGE_USAGE_STORAGE_BIT};
        TextureID         id   = create_texture(&r->vk, &desc);
        Texture          *tex  = &r->vk.texture_system.textures[id];

        TextureInfo *texinfo    = &r->vk.texture_system.info[id];
        VkDeviceSize image_size = w * h * 4;
        Buffer       staging;
        create_buffer(&r->vk, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &staging);

        memcpy(staging.mapping, pixels, image_size);
        stbi_image_free(pixels);
        VkCommandBuffer      cmd      = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);
        VkImageMemoryBarrier barrier1 = {
            .sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcAccessMask               = 0,
            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
            .image                       = tex->image,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = texinfo->mip_count,
            .subresourceRange.layerCount = 1,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier1);
        VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .imageSubresource.layerCount = 1,
                                    .imageExtent                 = {w, h, 1}};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier barrier2 = barrier1;
        barrier2.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier2.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier2.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier2.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier2);
        vk_end_one_time_cmd(r->vk.devc.device, r->vk.devc.graphics_queue, r->vk.one_time_gfx_pool, cmd);
        destroy_buffer(&r->vk, &staging);

        r->dummy_texture = id;
    };
    {
        VkDeviceSize size = r->vk.swapchain.extent.width * r->vk.swapchain.extent.height * 4; // RGBA8

        create_buffer(&r->vk, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      &r->readback_buffer);
    }
    {
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            create_buffer(&r->vk, sizeof(GlobalData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                          &r->global_ubo[i]);
        }
    }

    {

        {
            GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
            cfg.vert_path              = "compiledshaders/fire.vert.spv";
            cfg.frag_path              = "compiledshaders/fire.frag.spv";
            cfg.color_formats          = &r->hdr_color[0].format;

            r->EnginePipelines.fire = pipeline_create_graphics(&r->vk, &cfg);
        }

        {
            r->EnginePipelines.postprocess = pipeline_create_compute(&r->vk, "compiledshaders/postprocess.comp.spv");
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
            cfg.vert_path              = "compiledshaders/smaa_edge.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_edge.frag.spv";
            cfg.color_formats          = &r->smaa_edges[0].format;

            r->smaa_pipelines.smaa_edge = pipeline_create_graphics(&r->vk, &cfg);
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
            cfg.vert_path              = "compiledshaders/smaa_weight.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_weight.frag.spv";
            cfg.color_formats          = &r->smaa_weights[0].format;

            r->smaa_pipelines.smaa_weight = pipeline_create_graphics(&r->vk, &cfg);
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
            cfg.vert_path              = "compiledshaders/smaa_blend.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_blend.frag.spv";
            cfg.color_formats          = &r->smaa_final[0].format;

            r->smaa_pipelines.smaa_blend = pipeline_create_graphics(&r->vk, &cfg);
        }
    }
}
Renderer *renderer_create(bool use_wayland) {
#ifndef RGFW_WAYLAND
    if (use_wayland) {
        log_error("[renderer] Wayland support is not enabled in this build");
        return NULL;
    }
#endif
    VK_CHECK(volkInitialize());
    if (RGFW_init("mu_gfx", RGFW_initVulkan | (use_wayland ? 0 : RGFW_initX11)) != 0) {
        log_error("[renderer] RGFW initialization failed");
        return NULL;
    }
    if (use_wayland && !RGFW_usingWayland()) {
        log_error("[renderer] Could not connect to the requested Wayland display");
        RGFW_deinit();
        return NULL;
    }
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};

    size_t       platform_ext_count = 0;
    const char **platform_exts = RGFW_getRequiredInstanceExtensions_Vulkan(&platform_ext_count);
    if (!platform_exts || !platform_ext_count) {
        log_error("[renderer] RGFW Vulkan instance extensions unavailable");
        RGFW_deinit();
        return NULL;
    }

    VkBackendDesc desc = {
        .app_name            = "My Renderer",
        .instance_layers     = NULL,
        .instance_extensions = platform_exts,
        .device_extensions   = dev_exts,

        .instance_layer_count        = 0,
        .instance_extension_count    = (uint32_t)platform_ext_count,
        .device_extension_count      = 2,
        .enable_gpu_based_validation = false,
        .enable_validation           = VALIDATION,

        .validation_severity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .validation_types = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .width            = 1362,
        .height           = 749,

        .swapchain_preferred_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
        .swapchain_preferred_format      = VK_FORMAT_B8G8R8A8_SRGB,
        .swapchain_extra_usage_flags =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .vsync               = false,
        .enable_debug_printf = false,

        .bindless_sampled_image_count     = MAX_BINDLESS_TEXTURES,
        .bindless_sampler_count           = MAX_BINDLESS_SAMPLERS,
        .bindless_storage_image_count     = 16384,
        .enable_pipeline_stats            = true,
        .enable_graphics_profiler         = true,
        .swapchain_preferred_present_mode = VK_PRESENT_MODE_MAILBOX_KHR,

        .size_of_cpu_pool     = MB(32),
        .size_of_gpu_pool     = MB(512),
        .size_of_staging_pool = MB(128),

    };

    Renderer *r;
    (void)posix_memalign((void **)&r, _Alignof(Renderer), sizeof(*r));
    memset(r, 0, sizeof(*r)); // zero-value = safe defaults everywhere
    vk_instance_create(&r->vk, &desc);
    r->window = RGFW_createWindow("Vulkan", 0, 0, desc.width, desc.height, RGFW_windowCenter);
    if (!r->window) {
        log_error("[renderer] RGFW window creation failed");
        exit(EXIT_FAILURE);
    }
    RGFW_window_setExitKey(r->window, RGFW_keyNULL);
    VK_CHECK(RGFW_window_createSurface_Vulkan(r->window, r->vk.instance.instance, &r->vk.surface));
    int width, height;
    RGFW_window_getSizeInPixels(r->window, &width, &height);
    desc.width = (uint32_t)width;
    desc.height = (uint32_t)height;
    vk_backend_create(&r->vk, &desc);
    renderer_resources_create(r, &desc);
    if (!renderer3d_create(&r->vk, &r->scene, r->hdr_color[0].format, r->depth[0].format))
        exit(EXIT_FAILURE);
    r->start_time = r->cpu_prev_frame = mu_time_now();
    nuklear_init(r);

    // gfx_pipelines();
    input_init(&r->input, r->window);
    dmon_init();
    g_source_watch_id = dmon_watch("shaders", watch_callback, DMON_WATCHFLAGS_RECURSIVE, r);
    dmon_watch("compiledshaders", watch_callback, DMON_WATCHFLAGS_RECURSIVE, r);
    return r;
}
#define GPU_PROF_HISTORY_SIZE 128
#define MAX_RECORDED_PASSES   32

typedef struct GpuPassStats {
    char     name[64];
    double   time_ms;
    double   avg_ms;
    double   min_ms;
    double   max_ms;
    uint64_t vs_invocations;
    uint64_t fs_invocations;
    uint64_t primitives;
    float    history[GPU_PROF_HISTORY_SIZE];
    uint32_t history_idx;
} GpuPassStats;

typedef struct GpuProfilerUIState {
    bool         open;
    bool         paused;
    bool         show_pipeline_stats;
    uint32_t     pass_count;
    double       total_gpu_time_ms;
    double       avg_total_gpu_time_ms;
    float        total_history[GPU_PROF_HISTORY_SIZE];
    uint32_t     total_history_idx;
    GpuPassStats pass_stats[MAX_RECORDED_PASSES];
    uint32_t     frame_counter;
} GpuProfilerUIState;

static double ns_to_ms(double ns) { return ns / 1000000.0; }

static GpuProfilerUIState g_gpu_profiler_ui = {
    .open                = false,
    .paused              = false,
    .show_pipeline_stats = true,
};

static void gpu_profiler_ui_update(GpuProfiler *p) {
    if (g_gpu_profiler_ui.paused || !p || !p->enabled || p->pass_count == 0)
        return;

    g_gpu_profiler_ui.pass_count = MIN(p->pass_count, MAX_RECORDED_PASSES);
    double total_ms              = 0.0;

    for (uint32_t i = 0; i < p->pass_count && i < MAX_RECORDED_PASSES; i++) {
        GpuPass      *pass = &p->passes[i];
        GpuPassStats *ps   = &g_gpu_profiler_ui.pass_stats[i];

        if (pass->name) {
            strncpy(ps->name, pass->name, sizeof(ps->name) - 1);
            ps->name[sizeof(ps->name) - 1] = '\0';
        } else {
            snprintf(ps->name, sizeof(ps->name), "Pass %u", i);
        }

        double ms = pass->time_ms;
        if (ms < 0.0)
            ms = 0.0;
        ps->time_ms = ms;
        total_ms += ms;

        if (ps->avg_ms == 0.0) {
            ps->avg_ms = ms;
            ps->min_ms = ms;
            ps->max_ms = ms;
        } else {
            ps->avg_ms = ps->avg_ms * 0.95 + ms * 0.05;
            if (ms < ps->min_ms)
                ps->min_ms = ms;
            if (ms > ps->max_ms)
                ps->max_ms = ms;
        }

        ps->vs_invocations = pass->vs_invocations;
        ps->fs_invocations = pass->fs_invocations;
        ps->primitives     = pass->primitives;

        ps->history[ps->history_idx] = (float)ms;
        ps->history_idx              = (ps->history_idx + 1) % GPU_PROF_HISTORY_SIZE;
    }

    g_gpu_profiler_ui.total_gpu_time_ms = total_ms;
    if (g_gpu_profiler_ui.avg_total_gpu_time_ms == 0.0) {
        g_gpu_profiler_ui.avg_total_gpu_time_ms = total_ms;
    } else {
        g_gpu_profiler_ui.avg_total_gpu_time_ms = g_gpu_profiler_ui.avg_total_gpu_time_ms * 0.95 + total_ms * 0.05;
    }

    g_gpu_profiler_ui.total_history[g_gpu_profiler_ui.total_history_idx] = (float)total_ms;
    g_gpu_profiler_ui.total_history_idx = (g_gpu_profiler_ui.total_history_idx + 1) % GPU_PROF_HISTORY_SIZE;
    g_gpu_profiler_ui.frame_counter++;
}

static void profiler_format_count(char *buffer, size_t buffer_size, uint64_t value) {
    if (value >= 1000000000ull) {
        snprintf(buffer, buffer_size, "%.2f B", (double)value / 1000000000.0);
    } else if (value >= 1000000ull) {
        snprintf(buffer, buffer_size, "%.2f M", (double)value / 1000000.0);
    } else if (value >= 1000ull) {
        snprintf(buffer, buffer_size, "%.1f k", (double)value / 1000.0);
    } else {
        snprintf(buffer, buffer_size, "%llu", (unsigned long long)value);
    }
}

static MU_INLINE bool frame_start(Renderer *r) {
    TracyCZoneNC(ctx, "frame_start", 0x00FF00, 1);
    uint64_t frame_now = mu_time_now();
    r->cpu_frame_ns    = (double)(frame_now - r->cpu_prev_frame);
    r->cpu_prev_frame  = frame_now;
    r->vk.current_frame   = (r->vk.current_frame + 1) % MAX_FRAMES_IN_FLIGHT;

    int fb_w, fb_h;
    RGFW_window_getSizeInPixels(r->window, &fb_w, &fb_h);

    if (fb_w == 0 || fb_h == 0 || RGFW_window_isMinimized(r->window)) {
        uint64_t wait_start = mu_time_now();
        RGFW_waitForEvent(100);
        r->cpu_wait_accum_ns += (double)(mu_time_now() - wait_start);
        TracyCZoneEnd(ctx);
        return false;
    }
    r->vk.swapchain.needs_recreate |= fb_w != (int)r->vk.swapchain.extent.width || fb_h != (int)r->vk.swapchain.extent.height;

    // Recreate first, acquire after: if the swapchain is (re)created, the old
    // acquired image would be invalid. Skip the frame and acquire fresh next time.
    if (r->vk.swapchain.needs_recreate) {
        // Recreation waits for both graphics and presentation before replacing resources.
        vk_swapchain_recreate(r->vk.devc.device, r->vk.devc.physical_device, &r->vk.swapchain, fb_w, fb_h,
                              r->vk.devc.graphics_queue, r->vk.one_time_gfx_pool, &r->vk);

        forEach(i, r->vk.swapchain.image_count) {
            rt_resize(&r->vk, &r->depth[i], fb_w, fb_h);
            rt_resize(&r->vk, &r->hdr_color[i], fb_w, fb_h);

            rt_resize(&r->vk, &r->ldr_color[i], fb_w, fb_h);

            rt_resize(&r->vk, &r->smaa_final[i], fb_w, fb_h);
            rt_resize(&r->vk, &r->smaa_edges[i], fb_w, fb_h);
            rt_resize(&r->vk, &r->smaa_weights[i], fb_w, fb_h);
        }

        capture_resize(r, (uint32_t)fb_w, (uint32_t)fb_h);
        r->vk.swapchain.needs_recreate = false;
        TracyCZoneEnd(ctx);
        return false;
    }

    uint64_t wait_start = mu_time_now();
    bool acquired = vk_frame_acquire(&r->vk);
    r->cpu_wait_ns = (double)(mu_time_now() - wait_start);
    r->cpu_wait_accum_ns = r->cpu_wait_accum_ns * 0.95 + r->cpu_wait_ns * 0.05;
    r->cpu_active_ns = MAX(r->cpu_frame_ns - r->cpu_wait_ns, 0.0);
    capture_consume(r);
    gpu_profiler_ui_update(&r->vk.gpuprofiler[r->vk.current_frame]);
    TracyCZoneEnd(ctx);
    return acquired;
}
static void update_global_data(Renderer *r) {
    GlobalData data = {0};
    glm_mat4_identity(data.view);
    glm_mat4_identity(data.projection);
    glm_mat4_identity(data.viewproj);
    glm_mat4_identity(data.inv_view);
    glm_mat4_identity(data.inv_projection);
    glm_mat4_identity(data.inv_viewproj);

    data.time             = (float)((double)(mu_time_now() - r->start_time) / mu_time_freq());
    data.delta_time       = (float)((double)r->cpu_frame_ns / 1000000000.0);
    data.frame_count      = r->frame_count++;
    data.screen_params[0] = (float)r->vk.swapchain.extent.width;
    data.screen_params[1] = (float)r->vk.swapchain.extent.height;
    data.screen_params[2] = 1.0f / data.screen_params[0];
    data.screen_params[3] = 1.0f / data.screen_params[1];

    Buffer *global_buffer = &r->global_ubo[r->vk.current_frame];
    memcpy(global_buffer->mapping, &data, sizeof(data));
    vmaFlushAllocation(r->vk.devc.vmaallocator, global_buffer->allocation, 0, sizeof(data));

    VkDescriptorBufferInfo info = {
        .buffer = global_buffer->buffer,
        .offset = 0,
        .range  = sizeof(data),
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = r->vk.bindless_system.set,
        .dstBinding      = GLOBAL_DATA_BINDING,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo     = &info,
    };
    vkUpdateDescriptorSets(r->vk.devc.device, 1, &write, 0, NULL);
}

PUSH_CONSTANT(PostPush, uint32_t src_texture_id; uint32_t output_image_id; uint32_t sampler_id; uint32_t width;
              uint32_t height; uint frame; float exposure;

);
PUSH_CONSTANT(EdgePush, uint32_t texture_id; uint32_t sampler_id;);

PUSH_CONSTANT(BlendPush, uint32_t color_tex; uint32_t weight_tex; uint32_t sampler_id; uint32_t pad;);

PUSH_CONSTANT(WeightPush, uint32_t edge_tex; uint32_t area_tex; uint32_t search_tex; uint32_t sampler_id;);

#include "src/nuklear_profiler.inl"

static void post_pass(Renderer *r, VkCommandBuffer cmd) {
    uint32_t image = r->vk.swapchain.current_image;

    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(frame_prof, cmd, "Post Processing", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
        RenderTarget *reads[]  = {&r->hdr_color[image]};
        RenderTarget *writes[] = {&r->ldr_color[image]};

        begin_pass(&r->vk, cmd, &(PassDesc){
                         .shader_reads      = reads,
                         .shader_read_count = 1,
                         .shader_writes     = writes,
                         .shader_write_count = 1,
                         .pipeline          = r->EnginePipelines.postprocess,
                     });

        PostPush push = {
            .src_texture_id  = r->hdr_color[image].bindless_index,
            .output_image_id = r->ldr_color[image].bindless_index,
            .sampler_id      = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            .width           = r->vk.swapchain.extent.width,
            .height          = r->vk.swapchain.extent.height,
            .frame           = 0,
            .exposure        = 1.2f,
        };

        dispatch_push(&r->vk, cmd, BYTE_SPAN(push), (push.width + 15) / 16, (push.height + 15) / 16, 1);
    }
}

typedef struct FirePush {
    uint32_t width;
    uint32_t height;
    float    time;
    float    pad;
} FirePush;

static void pass_fire(Renderer *r, VkCommandBuffer cmd) {
    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(frame_prof, cmd, "Fire Pass", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        uint32_t image = r->vk.swapchain.current_image;

        PassAttachment color = {
            .target = &r->hdr_color[image],
            .load   = LOAD_CLEAR,
            .clear  = {0.02f, 0.025f, 0.03f, 1.0f},
        };

        begin_pass(&r->vk, cmd, &(PassDesc){.colors = &color, .color_count = 1, .pipeline = r->EnginePipelines.fire});

        FirePush push = {
            .width  = r->vk.swapchain.extent.width,
            .height = r->vk.swapchain.extent.height,
            .time   = (float)((double)(mu_time_now() - r->start_time) / mu_time_freq()),
            .pad    = 0.0f,
        };

        cmd_draw(&r->vk, cmd, BYTE_SPAN(push), 3, 1);
        end_pass(cmd);
    }
}

static void pass_smaa(Renderer *r, VkCommandBuffer cmd) {
    uint32_t     image      = r->vk.swapchain.current_image;
    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];

    {
        /* 1. Edge detection */
        GPU_SCOPE(frame_prof, cmd, "SMAA Edge", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            PassAttachment color = {.target = &r->smaa_edges[image], .load = LOAD_CLEAR};
            RenderTarget  *reads[] = {&r->ldr_color[image]};

            begin_pass(&r->vk, cmd, &(PassDesc){
                             .colors            = &color,
                             .color_count       = 1,
                             .shader_reads      = reads,
                             .shader_read_count = 1,
                             .pipeline          = r->smaa_pipelines.smaa_edge,
                         });

            EdgePush edge_push = {
                .texture_id = r->ldr_color[image].bindless_index,
                .sampler_id = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            cmd_draw(&r->vk, cmd, BYTE_SPAN(edge_push), 3, 1);
            end_pass(cmd);
        }
    }
    {
        /* 2. Weight calculation */
        GPU_SCOPE(frame_prof, cmd, "SMAA Weight", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            PassAttachment color      = {.target = &r->smaa_weights[image], .load = LOAD_CLEAR};
            RenderTarget  *reads[]    = {&r->smaa_edges[image]};

            begin_pass(&r->vk, cmd, &(PassDesc){
                             .colors            = &color,
                             .color_count       = 1,
                             .shader_reads      = reads,
                             .shader_read_count = 1,
                             .pipeline          = r->smaa_pipelines.smaa_weight,
                         });

            WeightPush weight_push = {
                .edge_tex   = r->smaa_edges[image].bindless_index,
                .area_tex   = r->smaa_area_tex,
                .search_tex = r->smaa_search_tex,
                .sampler_id = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            cmd_draw(&r->vk, cmd, BYTE_SPAN(weight_push), 3, 1);
            end_pass(cmd);
        }
    }
    {
        /* 3. Blend LDR + SMAA into FINAL */
        GPU_SCOPE(frame_prof, cmd, "SMAA Blend", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            PassAttachment color   = {.target = &r->smaa_final[image], .load = LOAD_CLEAR};
            RenderTarget  *reads[] = {&r->ldr_color[image], &r->smaa_weights[image]};

            begin_pass(&r->vk, cmd, &(PassDesc){
                             .colors            = &color,
                             .color_count       = 1,
                             .shader_reads      = reads,
                             .shader_read_count = 2,
                             .pipeline          = r->smaa_pipelines.smaa_blend,
                         });

            BlendPush blend_push = {
                .color_tex  = r->ldr_color[image].bindless_index,
                .weight_tex = r->smaa_weights[image].bindless_index,
                .sampler_id = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            cmd_draw(&r->vk, cmd, BYTE_SPAN(blend_push), 3, 1);
            end_pass(cmd);
        }
    }
}

static void pass_ldr_to_swapchain(Renderer *r, VkCommandBuffer cmd) {
    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(frame_prof, cmd, "Blit Swapchain", VK_PIPELINE_STAGE_2_TRANSFER_BIT) {
        uint32_t image = r->vk.swapchain.current_image;

        rt_transition_all(&r->vk, cmd, &r->smaa_final[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        image_transition_swapchain(&r->vk, cmd, &r->vk.swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        flush_barriers(&r->vk, cmd);

        VkImageBlit blit = {
            .srcSubresource =
                {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = 0,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },

            .srcOffsets =
                {
                    {0, 0, 0},
                    {(int32_t)r->vk.swapchain.extent.width, (int32_t)r->vk.swapchain.extent.height, 1},
                },

            .dstSubresource =
                {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = 0,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },

            .dstOffsets =
                {
                    {0, 0, 0},
                    {(int32_t)r->vk.swapchain.extent.width, (int32_t)r->vk.swapchain.extent.height, 1},
                },
        };

        vkCmdBlitImage(cmd, r->smaa_final[image].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       r->vk.swapchain.images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
}

#include "src/nuklear_pass.inl"

static void platform_poll_events(Renderer *r) {
    struct nk_context *ctx = &r->ui.context;
    RGFW_event event;
    input_begin(&r->input);
    nk_input_begin(ctx);
    while (RGFW_checkEvent(&event)) {
        input_feed_rgfw(&r->input, &event);
        if (event.common.win != r->window)
            continue;
        switch (event.type) {
        case RGFW_windowClose:
            break;
        case RGFW_mouseMotion:
            nk_input_motion(ctx, event.mouse.x, event.mouse.y);
            break;
        case RGFW_mouseButtonPressed:
        case RGFW_mouseButtonReleased: {
            enum nk_buttons button;
            switch (event.button.value) {
            case RGFW_mouseLeft: button = NK_BUTTON_LEFT; break;
            case RGFW_mouseMiddle: button = NK_BUTTON_MIDDLE; break;
            case RGFW_mouseRight: button = NK_BUTTON_RIGHT; break;
            default: continue;
            }
            nk_input_button(ctx, button, (int)ctx->input.mouse.pos.x, (int)ctx->input.mouse.pos.y,
                            event.type == RGFW_mouseButtonPressed);
            break;
        }
        case RGFW_mouseScroll:
            nk_input_scroll(ctx, nk_vec2(event.delta.x, event.delta.y));
            break;
        case RGFW_keyChar:
            nk_input_unicode(ctx, event.keyChar.value);
            break;
        case RGFW_windowFocusOut:
            for (int key = 0; key < NK_KEY_MAX; ++key)
                nk_input_key(ctx, (enum nk_keys)key, nk_false);
            for (int button = 0; button < NK_BUTTON_MAX; ++button)
                nk_input_button(ctx, (enum nk_buttons)button, (int)ctx->input.mouse.pos.x,
                                (int)ctx->input.mouse.pos.y, nk_false);
            break;
        case RGFW_keyPressed:
        case RGFW_keyReleased: {
            bool down = event.type == RGFW_keyPressed;
            bool ctrl = (event.key.mod & RGFW_modControl) != 0;
            nk_input_key(ctx, NK_KEY_SHIFT, (event.key.mod & RGFW_modShift) != 0);
            nk_input_key(ctx, NK_KEY_CTRL, ctrl);
            switch (event.key.value) {
            case RGFW_keyDelete: nk_input_key(ctx, NK_KEY_DEL, down); break;
            case RGFW_keyReturn: nk_input_key(ctx, NK_KEY_ENTER, down); break;
            case RGFW_keyTab: nk_input_key(ctx, NK_KEY_TAB, down); break;
            case RGFW_keyBackSpace: nk_input_key(ctx, NK_KEY_BACKSPACE, down); break;
            case RGFW_keyUp: nk_input_key(ctx, NK_KEY_UP, down); break;
            case RGFW_keyDown: nk_input_key(ctx, NK_KEY_DOWN, down); break;
            case RGFW_keyLeft:
                nk_input_key(ctx, NK_KEY_LEFT, down && !ctrl);
                nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down && ctrl);
                break;
            case RGFW_keyRight:
                nk_input_key(ctx, NK_KEY_RIGHT, down && !ctrl);
                nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down && ctrl);
                break;
            case RGFW_keyHome:
                nk_input_key(ctx, NK_KEY_TEXT_START, down);
                nk_input_key(ctx, NK_KEY_SCROLL_START, down);
                break;
            case RGFW_keyEnd:
                nk_input_key(ctx, NK_KEY_TEXT_END, down);
                nk_input_key(ctx, NK_KEY_SCROLL_END, down);
                break;
            case RGFW_keyPageUp: nk_input_key(ctx, NK_KEY_SCROLL_UP, down); break;
            case RGFW_keyPageDown: nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down); break;
            case RGFW_keyC: nk_input_key(ctx, NK_KEY_COPY, down && ctrl); break;
            case RGFW_keyV: nk_input_key(ctx, NK_KEY_PASTE, down && ctrl); break;
            case RGFW_keyX: nk_input_key(ctx, NK_KEY_CUT, down && ctrl); break;
            case RGFW_keyZ: nk_input_key(ctx, NK_KEY_TEXT_UNDO, down && ctrl); break;
            case RGFW_keyY: nk_input_key(ctx, NK_KEY_TEXT_REDO, down && ctrl); break;
            case RGFW_keyA: nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, down && ctrl); break;
            default: break;
            }
            if (!down || event.key.repeat || ctrl || ctx->text_edit.active)
                break;
            if (event.key.value == RGFW_keyR) {
                char path[256];
                snprintf(path, sizeof(path), "screenshot_%llu.png",
                         (unsigned long long)(mu_time_now() * 1000.0 / mu_time_freq()));
                capture_take_screenshot(r, path);
            } else if (event.key.value == RGFW_keyF9) {
                if (r->capture.recording) {
                    capture_stop_video(r);
                } else {
                    char path[256];
                    snprintf(path, sizeof(path), "recording_%llu.mp4",
                             (unsigned long long)(mu_time_now() / mu_time_freq()));
                    capture_start_video(r, path, 60);
                }
            }
            break;
        }
        default: break;
        }
    }
    nk_input_end(ctx);
    int width, height;
    RGFW_window_getSize(r->window, &width, &height);
    r->ui.width  = (float)width;
    r->ui.height = (float)height;
}


bool renderer_frame(Renderer *r) {
        TracyCFrameMark;
        platform_poll_events(r);
        if (RGFW_window_shouldClose(r->window))
            return false;
        pipeline_rebuild(&r->vk);
   

        delete_queue_tick(&r->vk);
        

        if (!frame_start(r))
            return true; // swapchain out-of-date / minimized: nothing to record
        update_global_data(r);




        VkCommandBuffer cmd        = r->vk.frames[r->vk.current_frame].cmdbuf;
        GpuProfiler    *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];

        vk_cmd_begin(cmd, false);
        // This frame's queries complete with this frame's submission value; the
        // slot is consumed once the timeline covers it. Same value the capture
        // slot records.
        frame_prof->submit_value = r->vk.timeline_last_submitted + 1;
        gpu_profiler_begin_frame(frame_prof, cmd);

        {
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->vk.bindless_system.pipeline_layout,
                                        0, 1, &r->vk.bindless_system.set, 0, NULL);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->vk.bindless_system.pipeline_layout,
                                        0, 1, &r->vk.bindless_system.set, 0, NULL);

                rt_transition_all(&r->vk, cmd, &r->depth[r->vk.swapchain.current_image],
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

                flush_barriers(&r->vk, cmd);
            }
        }

        GPU_SCOPE(frame_prof, cmd, "3D cull and draw", VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
            renderer3d_record(&r->vk, &r->scene, cmd, &r->hdr_color[r->vk.swapchain.current_image],
                               &r->depth[r->vk.swapchain.current_image]);
        }
        post_pass(r, cmd);
        pass_smaa(r, cmd);
        pass_ldr_to_swapchain(r, cmd);
        render_gpu_profiler_ui(r);
        render_capture_ui(r);
        pass_nuklear(r, cmd);
        capture_record(r, cmd);
        image_transition_swapchain(&r->vk, cmd, &r->vk.swapchain, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);

        flush_barriers(&r->vk, cmd);

        vk_cmd_end(cmd);

        vk_frame_submit(&r->vk);

    return true;
}

void renderer_destroy(Renderer *r) {
    dmon_deinit();
    wait_idle(&r->vk);
    delete_queue_drain(&r->vk);
    forEach(i, CAPTURE_SLOTS) {
        r->vk.current_frame = (i + 1) % CAPTURE_SLOTS;
        capture_consume(r);
    }
    nuklear_shutdown(r);
    renderer3d_destroy(&r->vk, &r->scene);
    capture_shutdown(r);
    forEach(i, MAX_SWAPCHAIN_IMAGES) {
        rt_destroy(&r->vk, &r->depth[i]);
        rt_destroy(&r->vk, &r->hdr_color[i]);
        rt_destroy(&r->vk, &r->ldr_color[i]);
        rt_destroy(&r->vk, &r->smaa_final[i]);
        rt_destroy(&r->vk, &r->smaa_edges[i]);
        rt_destroy(&r->vk, &r->smaa_weights[i]);
    }
    forEach(i, MAX_FRAMES_IN_FLIGHT)
        destroy_buffer(&r->vk, &r->global_ubo[i]);
    destroy_buffer(&r->vk, &r->readback_buffer);
    pipeline_cache_save(r->vk.devc.device, r->vk.devc.physical_device, r->vk.devc.pipeline_cache,
                        "pipeline_cache.bin");
    vk_backend_destroy(&r->vk);
    vkDestroySurfaceKHR(r->vk.instance.instance, r->vk.surface, NULL);
    vk_instance_destroy(&r->vk);
    RGFW_window_close(r->window);
    RGFW_deinit();
    free(r);
}
