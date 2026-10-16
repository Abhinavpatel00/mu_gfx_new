#include "renderer.h"
#include "external/dmon/dmon.h"
#include "external/mu/mu/mu_perf.h"
#include "external/stb/stb_image.h"
#include "external/stb/stb_image_write.h"
#include "src/input.h"
#include "src/input_glfw.h"
#include "src/nuklear_ui.h"
#include "src/platform.h"
#include "src/slangtypes.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define JSMN_STATIC
#define JSMN_IMPLEMENTATION
#include "external/jsmn/jsmn.h"

// Forward declarations for functions defined later in this file or in included files
static void install_callbacks(Renderer *r);

#include "src/two_d/two_d.h"
#include "src/vk.h"

/* How many HUD lines the game can queue per frame, and how long each may be. */
#define GAME_HUD_LINES 12
#define GAME_HUD_CHARS 96
/* Bloom pyramid: mip[0] is half-res, each next level halves again. */
#define BLOOM_MIPS 5
// TODO : improve it may be using libav like something idk
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

/* Grade state. The bitmask mirrors shaders/postprocess.slang: a set bit means
   "apply this stage", so a stage can be skipped without a magic neutral value.
   Falls back to a tuned cartoon preset and is overridden live from the UI. */
#define POST_EXPOSURE      (1u << 0)
#define POST_TONEMAP       (1u << 1)
#define POST_GAMMA         (1u << 2)
#define POST_CONTRAST      (1u << 3)
#define POST_SATURATION    (1u << 4)
#define POST_WHITE_BALANCE (1u << 5)
#define POST_VIGNETTE      (1u << 6)
#define POST_GRAIN         (1u << 7)
#define POST_SHARPEN       (1u << 8)
#define POST_CA            (1u << 9)
#define POST_DITHER        (1u << 10)
#define POST_SEPIA         (1u << 11)
#define POST_LIFT_GAIN     (1u << 12)
#define POST_SSAO          (1u << 13)

#define LUT_PATH_CHARS 512
#define LUT_MAX_SIZE   64u
typedef struct PostSettings {
    uint32_t flags;
    uint32_t tonemap_mode; /* 0 neutral 1 aces 2 reinhard 3 reinhard2 4 filmic 5 uncharted2 6 unreal */
    float    exposure;     /* log2 stops */
    float    gamma;
    float    contrast;
    float    saturation;
    float    temperature;
    float    tint;
    float    vignette;
    float    vignette_smoothness;
    float    sharpen;
    float    ca;
    float    grain;
    float    sepia;
    float    dither;
    float    lift[3];
    float    gain[3];
    float    lut_strength;
    float    bloom_strength;
    float    bloom_threshold;
    float    bloom_knee;
    float    bloom_radius; /* upsample spread in destination texels */
    float    ssao_strength;
    bool     toon; /* cel-shade the scene instead of the PBR pipeline */
    char     lut_path[LUT_PATH_CHARS]; /* .cube or strip .png; empty = identity */
} PostSettings;

/* The Slime-Rancher-ish starting point: bright, saturated, soft highlights.
   Numbers, not a LUT, so the look survives without an external asset. */
static PostSettings post_settings_cartoon(void) {
    return (PostSettings){
        /* Every stage on, with neutral defaults for the ones the preset does not
           use, so each UI slider takes effect without a separate enable. */
        .flags = POST_EXPOSURE | POST_TONEMAP | POST_GAMMA | POST_CONTRAST | POST_SATURATION | POST_WHITE_BALANCE |
                 POST_VIGNETTE | POST_GRAIN | POST_SHARPEN | POST_CA | POST_DITHER | POST_SEPIA | POST_LIFT_GAIN |
                 POST_SSAO,
        .tonemap_mode        = 1u, /* ACES */
        .exposure            = 0.35f,
        .gamma               = 2.2f,
        .contrast            = 1.08f,
        .saturation          = 1.35f,
        .temperature         = 0.02f,
        .tint                = 0.0f,
        .vignette            = 0.25f,
        .vignette_smoothness = 0.55f,
        .sharpen             = 0.0f,
        .ca                  = 0.0f,
        .grain               = 0.0f,
        .sepia               = 0.0f,
        .dither              = 1.0f,
        .lift                = {0.0f, 0.0f, 0.0f},
        .gain                = {1.0f, 1.0f, 1.0f},
        .lut_strength        = 0.0f,
        .bloom_strength      = 0.65f,
        .bloom_threshold     = 0.85f,
        .bloom_knee          = 0.5f,
        .bloom_radius        = 1.0f,
        .ssao_strength       = 1.0f,
        .toon                = false,
    };
}

#define POST_CONFIG_PATH "post_settings.json"

static void post_settings_save(const PostSettings *s) {
    FILE *f = fopen(POST_CONFIG_PATH, "wb");
    if (!f) {
        fprintf(stderr, "[config] cannot open %s for writing\n", POST_CONFIG_PATH);
        return;
    }
    fprintf(f,
            "{\n"
            "  \"flags\": %u,\n"
            "  \"tonemap_mode\": %u,\n"
            "  \"exposure\": %.6g,\n"
            "  \"gamma\": %.6g,\n"
            "  \"contrast\": %.6g,\n"
            "  \"saturation\": %.6g,\n"
            "  \"temperature\": %.6g,\n"
            "  \"tint\": %.6g,\n"
            "  \"vignette\": %.6g,\n"
            "  \"vignette_smoothness\": %.6g,\n"
            "  \"sharpen\": %.6g,\n"
            "  \"ca\": %.6g,\n"
            "  \"grain\": %.6g,\n"
            "  \"sepia\": %.6g,\n"
            "  \"dither\": %.6g,\n"
            "  \"lift\": [%.6g, %.6g, %.6g],\n"
            "  \"gain\": [%.6g, %.6g, %.6g],\n"
            "  \"lut_strength\": %.6g,\n"
            "  \"lut_path\": \"%s\",\n"
            "  \"bloom_strength\": %.6g,\n"
            "  \"bloom_threshold\": %.6g,\n"
            "  \"bloom_knee\": %.6g,\n"
            "  \"bloom_radius\": %.6g,\n"
            "  \"ssao_strength\": %.6g,\n"
            "  \"toon\": %s\n"
            "}\n",
            s->flags, s->tonemap_mode, s->exposure, s->gamma, s->contrast, s->saturation, s->temperature, s->tint,
            s->vignette, s->vignette_smoothness, s->sharpen, s->ca, s->grain, s->sepia, s->dither, s->lift[0],
            s->lift[1], s->lift[2], s->gain[0], s->gain[1], s->gain[2], s->lut_strength, s->lut_path,
            s->bloom_strength,
            s->bloom_threshold, s->bloom_knee, s->bloom_radius, s->ssao_strength, s->toon ? "true" : "false");
    fclose(f);
}

static float json_prim_float(const char *js, const jsmntok_t *t) {
    char buf[32];
    int  len = t->end - t->start;
    if (len <= 0 || len >= (int)sizeof(buf))
        return 0.0f;
    memcpy(buf, js + t->start, (size_t)len);
    buf[len] = '\0';
    return (float)atof(buf);
}

static bool json_tok_eq(const char *js, const jsmntok_t *t, const char *key) {
    size_t klen = strlen(key);
    return (size_t)(t->end - t->start) == klen && memcmp(js + t->start, key, klen) == 0;
}

/* Copy a JSON string token into a fixed buffer, always NUL-terminated. */
static void json_prim_string(const char *js, const jsmntok_t *t, char *out, size_t out_cap) {
    if (!out_cap)
        return;
    int len = t->end - t->start;
    if (len < 0)
        len = 0;
    if ((size_t)len >= out_cap)
        len = (int)out_cap - 1;
    memcpy(out, js + t->start, (size_t)len);
    out[len] = '\0';
}

static void post_settings_load(PostSettings *s) {
    FILE *f = fopen(POST_CONFIG_PATH, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) {
        fclose(f);
        return;
    }
    char  *js = (char *)malloc((size_t)sz + 1);
    size_t rd = fread(js, 1, (size_t)sz, f);
    fclose(f);
    js[rd] = '\0';

    jsmn_parser p;
    jsmntok_t   tok[256];
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, rd, tok, 256);
    if (n < 1 || tok[0].type != JSMN_OBJECT) {
        free(js);
        return;
    }

    /* Walk key/value pairs of the top-level object. Arrays (lift, gain) are
       three consecutive primitive tokens consumed by index. */
    for (int i = 1; i + 1 < n;) {
        jsmntok_t *k = &tok[i];
        jsmntok_t *v = &tok[i + 1];
        if (k->type != JSMN_STRING) {
            i++;
            continue;
        }
        if (json_tok_eq(js, k, "flags"))
            s->flags = (uint32_t)json_prim_float(js, v);
        else if (json_tok_eq(js, k, "tonemap_mode"))
            s->tonemap_mode = (uint32_t)json_prim_float(js, v);
        else if (json_tok_eq(js, k, "exposure"))
            s->exposure = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "gamma"))
            s->gamma = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "contrast"))
            s->contrast = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "saturation"))
            s->saturation = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "temperature"))
            s->temperature = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "tint"))
            s->tint = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "vignette"))
            s->vignette = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "vignette_smoothness"))
            s->vignette_smoothness = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "sharpen"))
            s->sharpen = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "ca"))
            s->ca = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "grain"))
            s->grain = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "sepia"))
            s->sepia = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "dither"))
            s->dither = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "lut_strength"))
            s->lut_strength = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "lut_path")) {
            if (v->type == JSMN_STRING)
                json_prim_string(js, v, s->lut_path, sizeof(s->lut_path));
            else
                s->lut_path[0] = '\0';
        } else if (json_tok_eq(js, k, "bloom_strength"))
            s->bloom_strength = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "bloom_threshold"))
            s->bloom_threshold = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "bloom_knee"))
            s->bloom_knee = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "bloom_radius"))
            s->bloom_radius = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "ssao_strength"))
            s->ssao_strength = json_prim_float(js, v);
        else if (json_tok_eq(js, k, "ssao_mode"))
            (void)0; /* removed; ignore legacy files */
        else if (json_tok_eq(js, k, "toon"))
            s->toon = (v->end - v->start == 4) && memcmp(js + v->start, "true", 4) == 0;
        else if (json_tok_eq(js, k, "lift")) {
            for (int e = 0; e < 3 && i + 2 + e < n; e++)
                s->lift[e] = json_prim_float(js, &tok[i + 2 + e]);
            i += 2 + v->size;
            continue;
        } else if (json_tok_eq(js, k, "gain")) {
            for (int e = 0; e < 3 && i + 2 + e < n; e++)
                s->gain[e] = json_prim_float(js, &tok[i + 2 + e]);
            i += 2 + v->size;
            continue;
        }
        i += 2;
    }
    free(js);
}

struct Renderer {
    VkBackend vk;

    //  Renderer3D scene;
    double             cpu_frame_ns;
    uint64_t           start_time;
    double             cpu_active_ns;
    double             cpu_wait_ns;
    double             cpu_wait_accum_ns;
    uint64_t           cpu_prev_frame;
    uint32_t           frame_count;
    float              dt;
    struct GLFWwindow *window;
    Input              input;
    NuklearUi          ui;
    RenderTarget       depth[MAX_SWAPCHAIN_IMAGES];
    RenderTarget       hdr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget       ldr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget       smaa_final[MAX_SWAPCHAIN_IMAGES];
    RenderTarget       smaa_edges[MAX_SWAPCHAIN_IMAGES];
    RenderTarget       smaa_weights[MAX_SWAPCHAIN_IMAGES];
    RenderTarget       bloom_mip[MAX_SWAPCHAIN_IMAGES][BLOOM_MIPS];
    /* SSAO target at half res, one per swapchain image. Written fresh every
       frame; the pass is stateless, so there is no history to ping-pong. */
    RenderTarget ssao[MAX_SWAPCHAIN_IMAGES];
    TextureID    lut_texture;
    uint32_t     lut_size;
    PostSettings post;
    /* Published to every shading path through GlobalData. See renderer.h. */
    RenderEnvironment env;
    /* Camera, fed by the app each frame via renderer_set_camera. cglm
       column-major. prev_viewproj is last frame's, for temporal reprojection. */
    mat4      cam_view;
    mat4      cam_proj;
    mat4      cam_viewproj;
    mat4      cam_inv_view;
    mat4      cam_prev_viewproj;
    bool      debug_ui_open;
    bool      show_gpu_text; /* GPU profiler text overlay (text.slang), F1 */
    TextureID dummy_texture;
    TextureID smaa_area_tex;
    TextureID smaa_search_tex;
    struct {
        uint32_t smaa_edge;
        uint32_t smaa_weight;
        uint32_t smaa_blend;
    } smaa_pipelines;
    Buffer       global_ubo[MAX_FRAMES_IN_FLIGHT];
    Buffer       readback_buffer;
    CaptureState capture;
    TwoD        *two_d; /* NULL unless GameHooks.two_d opted the stage in */
    GameHooks    game;
    uint32_t     hud_count;
    char         hud_text[GAME_HUD_LINES][GAME_HUD_CHARS];
    struct {
        uint32_t fullscreen;
        uint32_t postprocess;
        uint32_t gltf_minimal;
        uint32_t sprite;
        uint32_t slug_text;
        uint32_t fire;
        uint32_t beam;
        uint32_t sky;
        uint32_t skinning;
        uint32_t grass;
        uint32_t bloom;
        uint32_t ssao;
        uint32_t text;
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
        .usage       = BUF_XFER_DST,
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

    // Output tree is gitignored; create it once here so every later write can
    // assume it exists. EEXIST means it already does, which is the common case.
    mkdir(CAPTURE_DIR, 0755);
    mkdir(CAPTURE_SCREENSHOT_DIR, 0755);
    mkdir(CAPTURE_VIDEO_DIR, 0755);

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

static bool capture_take_screenshot(Renderer *r, const char *path);

bool renderer_screenshot(Renderer *r, const char *path) { return capture_take_screenshot(r, path); }

static bool capture_take_screenshot(Renderer *r, const char *path) {
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
             c->src_is_bgra ? "bgra" : "rgba", c->width, c->height, fps, path);

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

    // The slot whose turn it is this frame is the one written MAX_FRAMES_IN_FLIGHT
    // frames ago, and the fence wait in frame_start has already retired that
    // submission. The submit_value check keeps that coupling explicit rather
    // than assumed: a slot is readable only once its timeline value has passed.
    uint32_t slot = r->vk.current_frame % CAPTURE_SLOTS;
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
        .srcStageMask        = prev_stage ? prev_stage : STAGE_COLOR,
        .srcAccessMask       = prev_access ? prev_access : ACCESS_COLOR_W,
        .dstStageMask        = STAGE_XFER,
        .dstAccessMask       = ACCESS_XFER_R,
        .oldLayout           = prev_layout,
        .newLayout           = LAYOUT_XFER_SRC,
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
    vkCmdCopyImageToBuffer(cmd, src, LAYOUT_XFER_SRC, c->readback[slot].buffer, 1, &region);

    VkImageMemoryBarrier2 to_att = to_src;
    to_att.srcStageMask          = STAGE_XFER;
    to_att.srcAccessMask         = ACCESS_XFER_R;
    to_att.dstStageMask          = STAGE_COLOR;
    to_att.dstAccessMask         = ACCESS_COLOR_R | ACCESS_COLOR_W;
    to_att.oldLayout             = LAYOUT_XFER_SRC;
    to_att.newLayout             = LAYOUT_COLOR;
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

/* LUT loading: .cube files and pre-flattened strip PNGs, both landing in the
   strip layout sample_lut() expects (W=N*N, H=N, blue-major, R8G8B8A8).
   Pure C on purpose: external/lut/lut.hpp is C++ with exceptions and streams,
   which the project rule keeps out of engine code. */

/* Deferred texture destroy through the submission timeline, same policy as the
   pipeline hot-reload path: in-flight frames may still sample the old LUT. */
static void deferred_destroy_texture(VkBackend *r, void *user) {
    destroy_texture(r, (TextureID)(uintptr_t)user);
}

/* Swap the live LUT for new strip pixels. Same N re-uploads in place (no
   bindless churn); a new N recreates and retires the old texture on the
   timeline. Upload failure leaves the previous LUT live. */
static bool lut_swap_pixels(Renderer *r, uint32_t n, const uint8_t *px, const char *path) {
    uint32_t w = n * n;
    if (n == r->lut_size) {
        if (!texture_upload(&r->vk, r->lut_texture, 0, 0, (VkOffset3D){0, 0, 0}, (VkExtent3D){w, n, 1},
                            (ByteSpan){(void *)px, (uint32_t)((size_t)w * n * 4u)})) {
            fprintf(stderr, "[lut] %s: re-upload failed, keeping previous LUT\n", path);
            return false;
        }
        return true;
    }
    TextureCreateDesc desc = {.width     = w,
                              .height    = n,
                              .mip_count = 1,
                              .format    = VK_FORMAT_R8G8B8A8_UNORM,
                              .usage     = IMG_SAMPLED | IMG_XFER_DST};
    TextureID         fresh = create_texture(&r->vk, &desc);
    if (fresh == UINT32_MAX) {
        fprintf(stderr, "[lut] %s: texture create failed, keeping previous LUT\n", path);
        return false;
    }
    if (!texture_upload(&r->vk, fresh, 0, 0, (VkOffset3D){0, 0, 0}, (VkExtent3D){w, n, 1},
                        (ByteSpan){(void *)px, (uint32_t)((size_t)w * n * 4u)})) {
        fprintf(stderr, "[lut] %s: upload failed, keeping previous LUT\n", path);
        destroy_texture(&r->vk, fresh);
        return false;
    }
    TextureID old    = r->lut_texture;
    r->lut_texture   = fresh;
    r->lut_size      = n;
    delete_queue_defer(&r->vk, r->vk.timeline_last_submitted + 1, deferred_destroy_texture,
                       (void *)(uintptr_t)old);
    return true;
}

static float lut_clamp01(double v) { return (float)(v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v); }


/* A neutral (identity) strip LUT: n x n cells of n x n texels, blue major.
/* Minimal Adobe .cube reader: LUT_3D_SIZE N followed by N^3 RGB triples in
   r-fastest order. DOMAIN_MIN/MAX other than 0..1 and LUT_1D_SIZE are rejected
   rather than silently rescaled. */
static bool lut_load_cube(Renderer *r, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[lut] %s: cannot open\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (16 << 20)) {
        fprintf(stderr, "[lut] %s: bad size %ld\n", path, sz);
        fclose(f);
        return false;
    }
    char *text = (char *)malloc((size_t)sz + 1);
    if (!text) {
        fclose(f);
        return false;
    }
    size_t rd = fread(text, 1, (size_t)sz, f);
    fclose(f);
    text[rd] = '\0';

    uint32_t n = 0;
    bool     has_1d = false, domain_ok = true;
    for (char *line = text;;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        while (*line == ' ' || *line == '\t')
            line++;
        if (strncmp(line, "LUT_3D_SIZE", 11) == 0)
            n = (uint32_t)strtoul(line + 11, NULL, 10);
        else if (strncmp(line, "LUT_1D_SIZE", 11) == 0)
            has_1d = true;
        else if (strncmp(line, "DOMAIN_MIN", 10) == 0 || strncmp(line, "DOMAIN_MAX", 10) == 0) {
            const char *p   = strchr(line, ' ');
            bool        min = line[7] == 'I'; /* DOMAIN_MIN vs DOMAIN_MAX */
            for (int c = 0; c < 3 && p && domain_ok; ++c) {
                double v    = strtod(p, (char **)&p);
                double want = min ? 0.0 : 1.0;
                if (v < want - 1e-6 || v > want + 1e-6)
                    domain_ok = false;
            }
        }
        if (!nl)
            break;
        line = nl + 1;
        if (*line == '\0')
            break;
    }
    if (has_1d || n < 2u || n > LUT_MAX_SIZE || !domain_ok) {
        fprintf(stderr, "[lut] %s: unsupported header (1d=%d N=%u domain=%d)\n", path, (int)has_1d, n,
                (int)domain_ok);
        free(text);
        return false;
    }

    size_t   count = (size_t)n * n * n;
    uint8_t *px    = (uint8_t *)malloc(count * 4u);
    if (!px) {
        free(text);
        return false;
    }
    size_t idx = 0;
    bool   ok  = true;
    for (char *line = text; ok && idx < count;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '#' || *line == '\0' || (*line >= 'A' && *line <= 'Z')) {
            /* comment, blank, or keyword line */
        } else {
            char *p = line, *start = line;
            double c[3];
            for (int k = 0; k < 3; ++k) {
                c[k] = strtod(p, &p);
                if (p == start && k == 0) {
                    ok = false;
                    break;
                }
                start = p;
            }
            if (ok) {
                uint32_t rr  = (uint32_t)(idx % n);
                uint32_t gg  = (uint32_t)((idx / n) % n);
                uint32_t bb  = (uint32_t)(idx / ((size_t)n * n));
                uint8_t *dst = px + ((size_t)gg * n * n + bb * n + rr) * 4u;
                dst[0]       = (uint8_t)(lut_clamp01(c[0]) * 255.0f + 0.5f);
                dst[1]       = (uint8_t)(lut_clamp01(c[1]) * 255.0f + 0.5f);
                dst[2]       = (uint8_t)(lut_clamp01(c[2]) * 255.0f + 0.5f);
                dst[3]       = 255u;
                idx++;
            }
        }
        if (!nl)
            break;
        line = nl + 1;
        if (*line == '\0' && idx < count)
            break;
    }
    free(text);
    if (!ok || idx != count) {
        fprintf(stderr, "[lut] %s: want %zu triples, got %zu\n", path, count, idx);
        free(px);
        return false;
    }
    bool swapped = lut_swap_pixels(r, n, px, path);
    free(px);
    if (swapped)
        fprintf(stderr, "[lut] %s: cube N=%u\n", path, n);
    return swapped;
}

/* Strip PNG: W must equal N*N with H=N. Vendored 289x17 files are N=17. */
static bool lut_load_png(Renderer *r, const char *path) {
    int            w, h, c;
    unsigned char *src = stbi_load(path, &w, &h, &c, 4);
    if (!src) {
        fprintf(stderr, "[lut] %s: cannot decode: %s\n", path, stbi_failure_reason());
        return false;
    }
    bool ok = false;
    if (w > 0 && h > 0 && w == h * h && h <= (int)LUT_MAX_SIZE) {
        ok = lut_swap_pixels(r, (uint32_t)h, src, path);
        if (ok)
            fprintf(stderr, "[lut] %s: strip N=%d\n", path, h);
    } else {
        fprintf(stderr, "[lut] %s: strip must be N*N x N, got %dx%d\n", path, w, h);
    }
    stbi_image_free(src);
    return ok;
}

/* Dispatcher shared by boot (post_settings.json lut_path) and the grade UI. */
static bool renderer_lut_load(Renderer *r, const char *path) {
    if (!path || !*path)
        return false;
    const char *dot = strrchr(path, '.');
    if (dot && (strcmp(dot, ".cube") == 0 || strcmp(dot, ".CUBE") == 0))
        return lut_load_cube(r, path);
    return lut_load_png(r, path);
}


/* A neutral (identity) strip LUT: n x n cells of n x n texels, blue major.
   Generated so the LUT path is always valid; dropping a real LUT in and setting
   lut_strength is all it takes to use one. */
static void create_identity_lut(Renderer *r) {
    const uint32_t N  = 32u;
    const uint32_t W  = N * N;
    uint8_t       *px = (uint8_t *)malloc((size_t)W * N * 4u);
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t *p = px + ((size_t)y * W + x) * 4u;
            p[0]       = (uint8_t)((x % N) * 255u / (N - 1u));
            p[1]       = (uint8_t)(y * 255u / (N - 1u));
            p[2]       = (uint8_t)((x / N) * 255u / (N - 1u));
            p[3]       = 255u;
        }
    TextureCreateDesc desc = {.width     = W,
                              .height    = N,
                              .mip_count = 1,
                              .format    = VK_FORMAT_R8G8B8A8_UNORM,
                              .usage     = IMG_SAMPLED | IMG_XFER_DST};
    r->lut_texture         = create_texture(&r->vk, &desc);
    r->lut_size            = N;
    texture_upload(&r->vk, r->lut_texture, 0, 0, (VkOffset3D){0, 0, 0}, (VkExtent3D){W, N, 1},
                   (ByteSpan){px, W * N * 4u});
    free(px);
}

static void renderer_resources_create(Renderer *r, VkBackendDesc *desc) {
    MemTag   prev         = vk_mem_set_tag(&r->vk, MEM_TAG_POST);
    VkFormat depth_format = pick_depth_format(r->vk.devc.physical_device);
    VkFormat hdr_format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    capture_init(r, r->vk.swapchain.extent.width, r->vk.swapchain.extent.height);

    RenderTargetSpec depth_spec = {.width  = r->vk.swapchain.extent.width,
                                   .height = r->vk.swapchain.extent.height,
                                   .layers = 1,

                                   .format = depth_format,

                                   .usage = IMG_DEPTH | IMG_SAMPLED,

                                   .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "depth_buffer"};
    RenderTargetSpec hdr_spec   = {.width      = r->vk.swapchain.extent.width,
                                   .height     = r->vk.swapchain.extent.height,
                                   .layers     = 1,
                                   .format     = hdr_format,
                                   .usage      = IMG_COLOR | IMG_STORAGE |
                                                 IMG_SAMPLED,
                                   .mip_count  = 1,
                                   .debug_name = "hdr_color"};
    RenderTargetSpec ldr_spec   = {.width  = r->vk.swapchain.extent.width,
                                   .height = r->vk.swapchain.extent.height,
                                   .layers = 1,

                                   .format = VK_FORMAT_R8G8B8A8_UNORM,

                                   .usage = IMG_COLOR |

                                            IMG_STORAGE |      // compute writes tonemap result
                                            IMG_XFER_SRC | // copy → swapchain
                                            IMG_XFER_DST | // optional clears
                                            IMG_SAMPLED,       // future post effects

                                   .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "ldr_color"};
    RenderTargetSpec smaa_final_spec = ldr_spec;
    smaa_final_spec.debug_name       = "smaa_final";

    RenderTargetSpec smaa_edge_spec = {.width  = r->vk.swapchain.extent.width,
                                       .height = r->vk.swapchain.extent.height,
                                       .layers = 1,

                                       .format = VK_FORMAT_R8G8_UNORM,

                                       .usage = IMG_COLOR | IMG_SAMPLED,

                                       .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                       .mip_count  = 1,
                                       .debug_name = "smaa_edges"};

    RenderTargetSpec smaa_weight_spec = {.width  = r->vk.swapchain.extent.width,
                                         .height = r->vk.swapchain.extent.height,
                                         .layers = 1,

                                         .format = VK_FORMAT_R8G8B8A8_UNORM,

                                         .usage = IMG_COLOR | IMG_SAMPLED,

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
                                      .usage     = IMG_SAMPLED | IMG_XFER_DST};

            smaa_area    = create_texture(&r->vk, &desc);
            Texture *tex = &r->vk.texture_system.textures[smaa_area];

            VkDeviceSize size = AREATEX_SIZE;

            Buffer staging;
            create_buffer(&r->vk, size, BUF_XFER_SRC, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, areaTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = LAYOUT_UNDEFINED,
                                            .newLayout                   = LAYOUT_XFER_DST,
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

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, LAYOUT_XFER_DST, 1, &region);

            barrier.oldLayout     = LAYOUT_XFER_DST;
            barrier.newLayout     = LAYOUT_SHADER_RO;
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
                                      .usage     = IMG_SAMPLED | IMG_XFER_DST};

            smaa_search  = create_texture(&r->vk, &desc);
            Texture *tex = &r->vk.texture_system.textures[smaa_search];

            VkDeviceSize size = SEARCHTEX_SIZE;

            Buffer staging;
            create_buffer(&r->vk, size, BUF_XFER_SRC, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, searchTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = LAYOUT_UNDEFINED,
                                            .newLayout                   = LAYOUT_XFER_DST,
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

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, LAYOUT_XFER_DST, 1, &region);

            barrier.oldLayout     = LAYOUT_XFER_DST;
            barrier.newLayout     = LAYOUT_SHADER_RO;
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
    /* Bloom pyramid: mip[k] is half-res shifted by k, each a single-mip target
       so its storage view covers exactly one level. */
    forEach(i, r->vk.swapchain.image_count) {
        rt_create(&r->vk, &r->depth[i], &depth_spec);
        rt_create(&r->vk, &r->hdr_color[i], &hdr_spec);
        rt_create(&r->vk, &r->ldr_color[i], &ldr_spec);
        rt_create(&r->vk, &r->smaa_final[i], &smaa_final_spec);

        rt_create(&r->vk, &r->smaa_edges[i], &smaa_edge_spec);
        rt_create(&r->vk, &r->smaa_weights[i], &smaa_weight_spec);
        forEach(k, BLOOM_MIPS) {
            uint32_t         mw       = MAX(r->vk.swapchain.extent.width >> (k + 1u), 1u);
            uint32_t         mh       = MAX(r->vk.swapchain.extent.height >> (k + 1u), 1u);
            RenderTargetSpec mip_spec = {.width      = mw,
                                         .height     = mh,
                                         .layers     = 1,
                                         .format     = hdr_format,
                                         .usage      = IMG_STORAGE | IMG_SAMPLED,
                                         .mip_count  = 1,
                                         .debug_name = "bloom_mip"};
            rt_create(&r->vk, &r->bloom_mip[i][k], &mip_spec);
        }
        /* SSAO at half resolution: the occlusion signal is low-frequency, and
           the hemisphere march is a depth-sample gather, so half res halves the
           tap count at no visible cost once the post pass averages it back. */
        uint32_t         aw        = MAX(r->vk.swapchain.extent.width / 2u, 1u);
        uint32_t         ah        = MAX(r->vk.swapchain.extent.height / 2u, 1u);
        RenderTargetSpec ssao_spec = {.width      = aw,
                                      .height     = ah,
                                      .layers     = 1,
                                      .format     = VK_FORMAT_R16G16B16A16_SFLOAT,
                                      .usage      = IMG_STORAGE | IMG_SAMPLED,
                                      .mip_count  = 1,
                                      .debug_name = "ssao"};
        rt_create(&r->vk, &r->ssao[i], &ssao_spec);
    }

    vk_mem_set_tag(&r->vk, MEM_TAG_CORE);
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
                                  .usage     = IMG_SAMPLED | IMG_XFER_DST |
                                               IMG_STORAGE};
        TextureID         id   = create_texture(&r->vk, &desc);
        Texture          *tex  = &r->vk.texture_system.textures[id];

        TextureInfo *texinfo    = &r->vk.texture_system.info[id];
        VkDeviceSize image_size = w * h * 4;
        Buffer       staging;
        create_buffer(&r->vk, image_size, BUF_XFER_SRC, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      &staging);

        memcpy(staging.mapping, pixels, image_size);
        stbi_image_free(pixels);
        VkCommandBuffer      cmd      = vk_begin_one_time_cmd(r->vk.devc.device, r->vk.one_time_gfx_pool);
        VkImageMemoryBarrier barrier1 = {
            .sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout                   = LAYOUT_UNDEFINED,
            .newLayout                   = LAYOUT_XFER_DST,
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
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, LAYOUT_XFER_DST, 1, &region);
        VkImageMemoryBarrier barrier2 = barrier1;
        barrier2.oldLayout            = LAYOUT_XFER_DST;
        barrier2.newLayout            = LAYOUT_SHADER_RO;
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

        create_buffer(&r->vk, size, BUF_XFER_DST, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      &r->readback_buffer);
    }
    {
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            create_buffer(&r->vk, sizeof(GlobalData), BUF_UNIFORM, VMA_MEMORY_USAGE_CPU_TO_GPU,
                          &r->global_ubo[i]);
        }
    }

    {

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

        {

            GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
            cfg.vert_path              = "compiledshaders/fire.vert.spv";
            cfg.frag_path              = "compiledshaders/fire.frag.spv";
            cfg.color_formats          = &r->hdr_color[0].format;

            r->EnginePipelines.fire = pipeline_create_graphics(&r->vk, &cfg);
        }

        r->EnginePipelines.bloom = pipeline_create_compute(&r->vk, "compiledshaders/bloom.comp.spv");
        r->EnginePipelines.ssao  = pipeline_create_compute(&r->vk, "compiledshaders/ssao.comp.spv");
        r->EnginePipelines.text  = pipeline_create_compute(&r->vk, "compiledshaders/text.comp.spv");

        {
            GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
            cfg.vert_path              = "compiledshaders/sky.vert.spv";
            cfg.frag_path              = "compiledshaders/sky.frag.spv";
            cfg.color_formats          = &r->hdr_color[0].format;
            r->EnginePipelines.sky     = pipeline_create_graphics(&r->vk, &cfg);
        }
    }

    r->post = post_settings_cartoon();
    r->env  = renderer_environment_default();
    post_settings_load(&r->post);
    create_identity_lut(r); if (r->post.lut_path[0])
        renderer_lut_load(r, r->post.lut_path);

    if (r->game.two_d)
        r->two_d = two_d_create(&r->vk, &r->hdr_color[0].format);
    if (r->game.start)
        r->game.start(r->game.user, r);
    vk_mem_report(&r->vk, "init");
}

Renderer *renderer_create(bool use_wayland, GameHooks game) {

    VK_CHECK(volkInitialize());

    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
    glfwInit();
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};

    uint32_t     platform_ext_count = 0;
    const char **platform_exts      = glfwGetRequiredInstanceExtensions(&platform_ext_count);
    if (!platform_exts || !platform_ext_count) {
        log_error("[renderer] GLFW Vulkan instance extensions unavailable");
        glfwTerminate();
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
        .enable_gpu_based_validation = GPU_VALIDATION,
        .enable_validation           = VALIDATION,

        .validation_severity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .validation_types = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .width            = 1362,
        .height           = 749,

        /* UNORM on purpose. The post pass applies the display gamma itself, and
           an sRGB swapchain would encode a second time on every write into it —
           a mid grey arrives near white and the whole image reads washed out.
           post_pass also clears FLAG_GAMMA when it does get an sRGB surface, so
           the encode happens exactly once either way. */
        .swapchain_preferred_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
        .swapchain_preferred_format      = VK_FORMAT_B8G8R8A8_UNORM,
        .swapchain_extra_usage_flags =
            IMG_XFER_DST | IMG_SAMPLED | IMG_XFER_SRC | IMG_STORAGE,
        .vsync               = false,
        .enable_debug_printf = true,

        .bindless_sampled_image_count     = MAX_BINDLESS_TEXTURES,
        .bindless_sampler_count           = MAX_BINDLESS_SAMPLERS,
        .bindless_storage_image_count     = 16384,
        .enable_pipeline_stats            = true,
        .enable_graphics_profiler         = true,
        .swapchain_preferred_present_mode = VK_PRESENT_MODE_MAILBOX_KHR,

        .size_of_cpu_pool     = MU_MB(32),
        .size_of_gpu_pool     = MU_MB(100),
        .size_of_staging_pool = MU_MB(100),

    };

    Renderer *r;
    (void)posix_memalign((void **)&r, _Alignof(Renderer), sizeof(*r));
    memset(r, 0, sizeof(*r)); // zero-value = safe defaults everywhere
    r->game = game;
    r->show_gpu_text = true; /* F1 toggles the GPU profiler text overlay */
    /* MU_RES=WxH overrides the window size, so headless runs can measure the
       per-pixel passes at a target resolution instead of the 1362x749 default. */
    {
        const char *res = getenv("MU_RES");
        unsigned    w = 0, h = 0;
        if (res && sscanf(res, "%ux%u", &w, &h) == 2 && w >= 64u && h >= 64u) {
            desc.width  = w;
            desc.height = h;
        }
    }
    vk_instance_create(&r->vk, &desc);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); /* we drive Vulkan ourselves */
    r->window = glfwCreateWindow((int)desc.width, (int)desc.height, "Vulkan", NULL, NULL);
    if (!r->window) {
        log_error("[renderer] glfwCreateWindow failed");
        exit(EXIT_FAILURE);
    }

    /* MU_FULLSCREEN=1 puts the window on the primary monitor at the video
       mode's size, so a tiling WM cannot halve it and measurements are stable. */
    if (getenv("MU_FULLSCREEN") && atoi(getenv("MU_FULLSCREEN"))) {
        GLFWmonitor       *mon = glfwGetPrimaryMonitor();
        const GLFWvidmode *vm  = mon ? glfwGetVideoMode(mon) : NULL;
        if (mon && vm)
            glfwSetWindowMonitor(r->window, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
    }

    VK_CHECK(glfwCreateWindowSurface(r->vk.instance.instance, r->window, NULL, &r->vk.surface));

    int width, height;
    glfwGetFramebufferSize(r->window, &width, &height);

    desc.width  = (uint32_t)width;
    desc.height = (uint32_t)height;
    vk_backend_create(&r->vk, &desc);
    renderer_resources_create(r, &desc);

    r->start_time = r->cpu_prev_frame = mu_time_now();
    nuklear_init(r);

    // gfx_pipelines();
    input_init(&r->input, r->window);
    install_callbacks(r);
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
    /* Live by default: the overlay, the profiler window and the headless perf
       dump all read this snapshot, and a frozen default made all three blank
       until someone ticked Pause off. The window's Pause box still freezes it
       on demand. */
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

#define PERF_REPORT_PATH "perf_report.txt"

/* Snapshot the live frame + per-pass stats to a text report. Called on demand
   from the profiler UI; one line per recorded pass. */
static void perf_dump(const Renderer *r) {
    FILE *f = fopen(PERF_REPORT_PATH, "wb");
    if (!f) {
        fprintf(stderr, "[config] cannot open %s for writing\n", PERF_REPORT_PATH);
        return;
    }
    double frame_ms  = ns_to_ms(r->cpu_frame_ns);
    double active_ms = ns_to_ms(r->cpu_active_ns);
    double wait_ms   = ns_to_ms(r->cpu_wait_accum_ns);
    double gpu_ms    = g_gpu_profiler_ui.total_gpu_time_ms;

    fprintf(f, "== mu_gfx perf report ==\n");
    fprintf(f, "frames            : %u\n", r->frame_count);
    fprintf(f, "frame time        : %.3f ms (%.1f fps)\n", frame_ms, frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0);
    fprintf(f, "cpu active (ema)  : %.3f ms\n", active_ms);
    fprintf(f, "cpu wait (ema)    : %.3f ms\n", wait_ms);
    fprintf(f, "gpu frame         : %.3f ms (avg %.3f ms)\n", gpu_ms, g_gpu_profiler_ui.avg_total_gpu_time_ms);
    fprintf(f, "resolution        : %ux%u\n", r->vk.swapchain.extent.width, r->vk.swapchain.extent.height);
    fprintf(f, "\n%-28s %10s %10s %10s %10s %12s %12s\n", "pass", "cur(ms)", "avg(ms)", "min(ms)", "max(ms)", "verts", "frags");
    for (uint32_t i = 0; i < g_gpu_profiler_ui.pass_count; i++) {
        const GpuPassStats *ps = &g_gpu_profiler_ui.pass_stats[i];
        fprintf(f, "%-28s %10.3f %10.3f %10.3f %10.3f %12llu %12llu\n", ps->name, ps->time_ms, ps->avg_ms,
                ps->min_ms, ps->max_ms, (unsigned long long)ps->vs_invocations,
                (unsigned long long)ps->fs_invocations);
    }
    fclose(f);
    fprintf(stderr, "[config] wrote %s\n", PERF_REPORT_PATH);
}

/* Headless entry point for the perf dump, so a scripted run can measure
   without opening the profiler window. */
void renderer_write_perf(Renderer *r) {
    if (r)
        perf_dump(r);
}

static MU_INLINE bool frame_start(Renderer *r) {
    TracyCZoneNC(ctx, "frame_start", 0x00FF00, 1);
    uint64_t frame_now  = mu_time_now();
    r->cpu_frame_ns     = (double)(frame_now - r->cpu_prev_frame);
    r->cpu_prev_frame   = frame_now;
    r->vk.current_frame = (r->vk.current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    int fb_w, fb_h;
    glfwGetFramebufferSize(r->window, &fb_w, &fb_h);

    if (fb_w == 0 || fb_h == 0 || glfwGetWindowAttrib(r->window, GLFW_ICONIFIED)) {
        uint64_t wait_start = mu_time_now();
        glfwWaitEventsTimeout(0.1); /* was RGFW_waitForEvent(100) */
        r->cpu_wait_accum_ns += (double)(mu_time_now() - wait_start);
        TracyCZoneEnd(ctx);
        return false;
    }
    r->vk.swapchain.needs_recreate |=
        fb_w != (int)r->vk.swapchain.extent.width || fb_h != (int)r->vk.swapchain.extent.height;

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
            forEach(k, BLOOM_MIPS) rt_resize(&r->vk, &r->bloom_mip[i][k], MAX((uint32_t)fb_w >> (k + 1u), 1u),
                                             MAX((uint32_t)fb_h >> (k + 1u), 1u));
            rt_resize(&r->vk, &r->ssao[i], MAX((uint32_t)fb_w / 2u, 1u), MAX((uint32_t)fb_h / 2u, 1u));
        }

        capture_resize(r, (uint32_t)fb_w, (uint32_t)fb_h);
        r->vk.swapchain.needs_recreate = false;
        TracyCZoneEnd(ctx);
        return false;
    }

    uint64_t wait_start  = mu_time_now();
    bool     acquired    = vk_frame_acquire(&r->vk);
    r->cpu_wait_ns       = (double)(mu_time_now() - wait_start);
    r->cpu_wait_accum_ns = r->cpu_wait_accum_ns * 0.95 + r->cpu_wait_ns * 0.05;
    r->cpu_active_ns     = MAX(r->cpu_frame_ns - r->cpu_wait_ns, 0.0);
    capture_consume(r);
    gpu_profiler_ui_update(&r->vk.gpuprofiler[r->vk.current_frame]);
    TracyCZoneEnd(ctx);
    return acquired;
}
static void update_global_data(Renderer *r) {
    GlobalData data = {0};
    glm_mat4_copy(r->cam_view, data.view);
    glm_mat4_copy(r->cam_proj, data.projection);
    glm_mat4_copy(r->cam_viewproj, data.viewproj);
    glm_mat4_copy(r->cam_inv_view, data.inv_view);
    glm_mat4_inv(r->cam_proj, data.inv_projection);
    glm_mat4_inv(r->cam_viewproj, data.inv_viewproj);
    glm_mat4_copy(r->cam_prev_viewproj, data.prev_viewproj);

    data.time             = (float)((double)(mu_time_now() - r->start_time) / mu_time_freq());
    data.delta_time       = r->dt;
    data.frame_count      = r->frame_count++;
    data.screen_params[0] = (float)r->vk.swapchain.extent.width;
    data.screen_params[1] = (float)r->vk.swapchain.extent.height;
    data.screen_params[2] = 1.0f / data.screen_params[0];
    data.screen_params[3] = 1.0f / data.screen_params[1];

    /* Environment: one write, read by the sky pass and both scene pipelines. */
    glm_vec3_copy(r->env.sky, data.env_sky);
    glm_vec3_copy(r->env.horizon, data.env_horizon);
    glm_vec3_copy(r->env.ground, data.env_ground);
    data.env_sky[3] = r->env.ambient;
    {
        const float *d = r->env.sun_dir;
        float        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float        inv = len > 1e-6f ? 1.0f / len : 0.0f;
        data.sun_dir[0]  = d[0] * inv;
        data.sun_dir[1]  = d[1] * inv;
        data.sun_dir[2]  = d[2] * inv;
    }
    data.sun_dir[3] = r->env.sun_intensity;
    glm_vec3_copy(r->env.sun_color, data.sun_color);

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

/* Field order is the shader's: both sides are flat scalars, so the layouts
   line up word for word. */
PUSH_CONSTANT(PostPush, uint32_t src_texture_id; uint32_t output_image_id; uint32_t sampler_id; uint32_t width;
              uint32_t height; uint32_t frame; float exposure; uint32_t flags; uint32_t tonemap_mode; float gamma_value;
              float contrast; float saturation; float temperature; float tint; float vignette_intensity;
              float vignette_smoothness; float sharpen_strength; float ca_strength; float grain_intensity;
              float sepia_intensity; float dither_strength; float lift_r; float lift_g; float lift_b; float gain_r;
              float gain_g; float gain_b; uint32_t lut_texture_id; uint32_t lut_size; float lut_strength;
              uint32_t bloom_texture_id; float bloom_strength; float time; uint32_t ssao_texture_id;
              uint32_t depth_texture_id; float ssao_strength;);

PUSH_CONSTANT(BloomPush, uint32_t src_texture_id; uint32_t dst_image_id; uint32_t sampler_id; uint32_t width;
              uint32_t height; uint32_t pass; float radius; float threshold; float soft_knee; uint32_t prefilter;);

PUSH_CONSTANT(SsaoPush, uint32_t depth_texture_id; uint32_t output_image_id; uint32_t sampler_id; uint32_t width;
              uint32_t height; uint32_t sample_count; uint32_t frame; float proj_00; float proj_11; float radius;
              float bias; float intensity; float near_z;);

PUSH_CONSTANT(SkyPush, float exponent; float glow_strength; float glow_power; float ground_fade;);
PUSH_CONSTANT(EdgePush, uint32_t texture_id; uint32_t sampler_id;);

PUSH_CONSTANT(BlendPush, uint32_t color_tex; uint32_t weight_tex; uint32_t sampler_id; uint32_t pad;);

PUSH_CONSTANT(WeightPush, uint32_t edge_tex; uint32_t area_tex; uint32_t search_tex; uint32_t sampler_id;);

#include "src/nuklear_profiler.inl"

/* Extract emissive into the half-res head, blur down the chain, then
   accumulate back up. The post pass reads mip[0]. */
static void bloom_dispatch(Renderer *r, VkCommandBuffer cmd, RenderTarget *src, RenderTarget *dst, uint32_t pass,
                           float radius) {

    RenderTarget *reads[]  = {src};
    RenderTarget *writes[] = {dst};
    PassDesc      pd       = {.shader_reads       = reads,
                              .shader_read_count  = 1,
                              .shader_writes      = writes,
                              .shader_write_count = 1,
                              .pipeline           = r->EnginePipelines.bloom};
    begin_pass(&r->vk, cmd, &pd);
    BloomPush p = {.src_texture_id = src->bindless_index,
                   .dst_image_id   = dst->bindless_index,
                   .sampler_id     = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
                   .width          = dst->width,
                   .height         = dst->height,
                   .pass           = pass,
                   .radius         = radius,
                   .threshold      = r->post.bloom_threshold,
                   .soft_knee      = r->post.bloom_knee,
                   .prefilter      = pass == 0u ? 1u : 0u};
    dispatch_push(&r->vk, cmd, BYTE_SPAN(p), (p.width + 7) / 8, (p.height + 7) / 8, 1);
    end_pass(&r->vk, cmd, &pd);
}

static void pass_bloom(Renderer *r, VkCommandBuffer cmd, uint32_t image) {
    if (r->post.bloom_strength <= 0.0f)
        return;

    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(frame_prof, cmd, "Bloom", STAGE_CS) {
        /* pass 0 extracts HDR into the half-res head; later passes convolve
           already-extracted light, so upsample reads the small level straight. */
        bloom_dispatch(r, cmd, &r->hdr_color[image], &r->bloom_mip[image][0], 0u, 1.0f);
        forEach(k, BLOOM_MIPS - 1u)
            bloom_dispatch(r, cmd, &r->bloom_mip[image][k], &r->bloom_mip[image][k + 1u], 1u, 1.0f);
        /* Upsample: dst[pix] += blur(src), read back through the storage view so
           the same target never sits in both the sampled and storage lists. */
        for (uint32_t k = BLOOM_MIPS - 1u; k > 0u; k--)
            bloom_dispatch(r, cmd, &r->bloom_mip[image][k], &r->bloom_mip[image][k - 1u], 2u, r->post.bloom_radius);
    }
}
/* Screen-space AO compute pass. Reads depth, reconstructs normals from its
   derivatives, and marches a rotated Poisson hemisphere. Stateless: it writes
   the half-res AO target fresh every frame, and post reads it via bindless
   index. There is nothing to invalidate between frames. */
static void pass_ssao(Renderer *r, VkCommandBuffer cmd) {
    uint32_t image = r->vk.swapchain.current_image;

    RenderTarget *reads[]  = {&r->depth[image]};
    RenderTarget *writes[] = {&r->ssao[image]};
    PassDesc      pd       = {.shader_reads       = reads,
                              .shader_read_count  = 1,
                              .shader_writes      = writes,
                              .shader_write_count = 1,
                              .pipeline           = r->EnginePipelines.ssao};

    const GlobalData *gd = (const GlobalData *)r->global_ubo[r->vk.current_frame].mapping;
    SsaoPush          p  = {.depth_texture_id = r->depth[image].bindless_index,
                            .output_image_id  = r->ssao[image].bindless_index,
                            /* D32_SFLOAT is not filterable, and filtering a depth texel
                               would invent geometry across the silhouette the AO is
                               meant to detect, so the depth taps are point samples. */
                            .sampler_id       = r->vk.default_samplers.samplers[SAMPLER_NEAREST_CLAMP],
                            .width            = r->ssao[image].width,
                            .height           = r->ssao[image].height,
                            .sample_count     = 16u,
                            .frame            = r->frame_count,
                            .proj_00          = gd->projection[0][0],
                            .proj_11          = gd->projection[1][1],
                            /* Half a metre reads as contact shading on props this size;
                               larger radii turn the scene into a grey wash. */
                            .radius           = 0.5f,
                            .bias             = 0.02f,
                            /* >1 deepens the crevices the samples do find without
                               letting a single occluded sample crush the pixel. */
                            .intensity        = 1.4f,
                            .near_z           = 0.1f};

    begin_pass(&r->vk, cmd, &pd);
    dispatch_push(&r->vk, cmd, BYTE_SPAN(p), (p.width + 7) / 8, (p.height + 7) / 8, 1);
    end_pass(&r->vk, cmd, &pd);
}

/* An sRGB swapchain encodes on every write into it, including the blit that
   lands the final image. The post pass must not have encoded first. */
static bool swapchain_encodes_srgb(const Renderer *r) {
    return r->vk.swapchain.format == VK_FORMAT_B8G8R8A8_SRGB || r->vk.swapchain.format == VK_FORMAT_R8G8B8A8_SRGB;
}

static void post_pass(Renderer *r, VkCommandBuffer cmd) {
    uint32_t image = r->vk.swapchain.current_image;

    pass_bloom(r, cmd, image);

    const PostSettings *s        = &r->post;
    bool                bloom_on = s->bloom_strength > 0.0f;
    bool                ssao_on  = (s->flags & POST_SSAO) && s->ssao_strength > 0.0f;

    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(frame_prof, cmd, "Tonemap", STAGE_CS) {
        /* bloom mip[0] was written as a storage image by the bloom passes, and
           the ssao target by the ssao pass, so naming them here is what
           transitions them to sampled reads. */
        /* Build the read set as a prefix so read_count stays correct when
           bloom and ssao are independently toggled. */
        RenderTarget *reads[4];
        uint32_t      read_count = 0;
        reads[read_count++]      = &r->hdr_color[image];
        if (bloom_on)
            reads[read_count++] = &r->bloom_mip[image][0];
        if (ssao_on) {
            reads[read_count++] = &r->ssao[image];
            reads[read_count++] = &r->depth[image];
        }
        RenderTarget *writes[] = {&r->ldr_color[image]};

        begin_pass(&r->vk, cmd,
                   &(PassDesc){
                       .shader_reads       = reads,
                       .shader_read_count  = read_count,
                       .shader_writes      = writes,
                       .shader_write_count = 1,
                       .pipeline           = r->EnginePipelines.postprocess,
                   });

        uint32_t bloom_id = bloom_on ? r->bloom_mip[image][0].bindless_index : 0xFFFFFFFFu;
        uint32_t ssao_id  = ssao_on ? r->ssao[image].bindless_index : 0xFFFFFFFFu;
        PostPush push     = {
            .src_texture_id      = r->hdr_color[image].bindless_index,
            .output_image_id     = r->ldr_color[image].bindless_index,
            .sampler_id          = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            .width               = r->vk.swapchain.extent.width,
            .height              = r->vk.swapchain.extent.height,
            .frame               = r->frame_count,
            .exposure            = s->exposure,
            /* Own the gamma or leave it to the swapchain, never both. */
            .flags               = swapchain_encodes_srgb(r) ? (s->flags & ~POST_GAMMA) : s->flags,
            .tonemap_mode        = s->tonemap_mode,
            .gamma_value         = s->gamma,
            .contrast            = s->contrast,
            .saturation          = s->saturation,
            .temperature         = s->temperature,
            .tint                = s->tint,
            .vignette_intensity  = s->vignette,
            .vignette_smoothness = s->vignette_smoothness,
            .sharpen_strength    = s->sharpen,
            .ca_strength         = s->ca,
            .grain_intensity     = s->grain,
            .sepia_intensity     = s->sepia,
            .dither_strength     = s->dither,
            .lift_r              = s->lift[0],
            .lift_g              = s->lift[1],
            .lift_b              = s->lift[2],
            .gain_r              = s->gain[0],
            .gain_g              = s->gain[1],
            .gain_b              = s->gain[2],
            .lut_texture_id      = r->lut_texture,
            .lut_size            = r->lut_size,
            .lut_strength        = s->lut_strength,
            .bloom_texture_id    = bloom_id,
            .bloom_strength      = s->bloom_strength,
            .time                = (float)((double)(mu_time_now() - r->start_time) / mu_time_freq()),
            .ssao_texture_id     = ssao_id,
            /* The AO bilateral upsample needs the full-res depth under each
               half-res tap, so post names the depth target too. */
            .depth_texture_id    = r->depth[image].bindless_index,
            .ssao_strength       = s->ssao_strength,
        };

        dispatch_push(&r->vk, cmd, BYTE_SPAN(push), (push.width + 15) / 16, (push.height + 15) / 16, 1);
    }
}

static void pass_smaa(Renderer *r, VkCommandBuffer cmd) {
    uint32_t     image      = r->vk.swapchain.current_image;
    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];

    {
        /* 1. Edge detection */
        GPU_SCOPE(frame_prof, cmd, "SMAA Edge", STAGE_COLOR) {
            PassAttachment color   = {.target = &r->smaa_edges[image], .load = LOAD_CLEAR};
            RenderTarget  *reads[] = {&r->ldr_color[image]};

            PassDesc pd = {
                .colors            = &color,
                .color_count       = 1,
                .shader_reads      = reads,
                .shader_read_count = 1,
                .pipeline          = r->smaa_pipelines.smaa_edge,
            };
            begin_pass(&r->vk, cmd, &pd);

            EdgePush edge_push = {
                .texture_id = r->ldr_color[image].bindless_index,
                .sampler_id = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            cmd_draw(&r->vk, cmd, BYTE_SPAN(edge_push), 3, 1);
            end_pass(&r->vk, cmd, &pd);
        }
    }
    {
        /* 2. Weight calculation */
        GPU_SCOPE(frame_prof, cmd, "SMAA Weight", STAGE_COLOR) {
            PassAttachment color   = {.target = &r->smaa_weights[image], .load = LOAD_CLEAR};
            RenderTarget  *reads[] = {&r->smaa_edges[image]};

            PassDesc pd = {
                .colors            = &color,
                .color_count       = 1,
                .shader_reads      = reads,
                .shader_read_count = 1,
                .pipeline          = r->smaa_pipelines.smaa_weight,
            };
            begin_pass(&r->vk, cmd, &pd);

            WeightPush weight_push = {
                .edge_tex   = r->smaa_edges[image].bindless_index,
                .area_tex   = r->smaa_area_tex,
                .search_tex = r->smaa_search_tex,
                .sampler_id = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            cmd_draw(&r->vk, cmd, BYTE_SPAN(weight_push), 3, 1);
            end_pass(&r->vk, cmd, &pd);
        }
    }
    {
        /* 3. Blend LDR + SMAA into FINAL */
        GPU_SCOPE(frame_prof, cmd, "SMAA Blend", STAGE_COLOR) {
            PassAttachment color   = {.target = &r->smaa_final[image], .load = LOAD_CLEAR};
            RenderTarget  *reads[] = {&r->ldr_color[image], &r->smaa_weights[image]};

            PassDesc pd = {
                .colors            = &color,
                .color_count       = 1,
                .shader_reads      = reads,
                .shader_read_count = 2,
                .pipeline          = r->smaa_pipelines.smaa_blend,
            };
            begin_pass(&r->vk, cmd, &pd);

            BlendPush blend_push = {
                .color_tex  = r->ldr_color[image].bindless_index,
                .weight_tex = r->smaa_weights[image].bindless_index,
                .sampler_id = r->vk.default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            cmd_draw(&r->vk, cmd, BYTE_SPAN(blend_push), 3, 1);
            end_pass(&r->vk, cmd, &pd);
        }
    }
}

/* GPU profiler text overlay (text.slang): one compute dispatch per line,
   5x8 glyphs blitted straight into the swapchain image. Reads the CPU-side
   profiler copy (a frame old — fine for an overlay). Deliberately outside
   any GPU_SCOPE so the overlay never profiles itself. */
typedef struct TextPush {
    int32_t  ox, oy;     /* origin in characters */
    int32_t  scale;      /* pixel replication per glyph texel */
    uint32_t color;      /* ABGR */
    uint32_t image;      /* bindless storage slot of the target */
    uint32_t data[28];   /* 112 chars, one byte each */
} TextPush;

#define GPU_TEXT_MAX_LINES 10
#define GPU_TEXT_MAX_CHARS 112
#define GPU_TEXT_SCALE     2

static void pass_text_overlay(Renderer *r, VkCommandBuffer cmd) {
    if (!r->show_gpu_text || !r->vk.enable_graphics_profiler)
        return;
    FlowSwapchain *sc = &r->vk.swapchain;
    if (!(sc->image_usage & VK_IMAGE_USAGE_STORAGE_BIT))
        return; /* surface refused STORAGE: overlay impossible, stay silent */

    /* Read the live per-frame profiler rather than the Nuklear profiler
       snapshot: the snapshot stays frozen while its Pause box is ticked, which
       would leave the overlay permanently blank. Timings here are one collect
       old (a pass's value lands with the frame's own submission), which is
       exactly what an overlay wants. */
    GpuProfiler *fp = &r->vk.gpuprofiler[r->vk.current_frame];
    if (fp->pass_count == 0)
        return;
    uint32_t image = sc->current_image;

    /* Headline total, lightly smoothed so it does not flicker frame to frame. */
    static double s_total_ema = 0.0;
    double        total       = 0.0;
    for (uint32_t i = 0; i < fp->pass_count; ++i)
        total += fp->passes[i].time_ms;
    s_total_ema += (total - s_total_ema) * 0.1;

    char lines[GPU_TEXT_MAX_LINES][GPU_TEXT_MAX_CHARS + 1];
    int  nlines = 0;
    snprintf(lines[nlines++], sizeof(lines[0]), "GPU %.2fms avg %.2fms", total, s_total_ema);
    for (uint32_t i = 0; i < fp->pass_count && nlines < GPU_TEXT_MAX_LINES; ++i) {
        const char *name = fp->passes[i].name ? fp->passes[i].name : "?";
        snprintf(lines[nlines++], sizeof(lines[0]), "%-16.16s %6.2f", name, fp->passes[i].time_ms);
    }

    image_transition_swapchain(&r->vk, cmd, sc, LAYOUT_GENERAL, STAGE_CS, ACCESS_SSBO_W);
    flush_barriers(&r->vk, cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      r->vk.render_pipelines.pipelines[r->EnginePipelines.text - 1]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->vk.bindless_system.pipeline_layout, 0, 1,
                            &r->vk.bindless_system.set, 0, NULL);

    /* Right-aligned block: one origin per line since lengths differ. */
    int cols = (int)sc->extent.width / (6 * GPU_TEXT_SCALE);
    for (int i = 0; i < nlines; ++i) {
        size_t n = strlen(lines[i]);
        if (n == 0 || n > GPU_TEXT_MAX_CHARS)
            n = n > GPU_TEXT_MAX_CHARS ? GPU_TEXT_MAX_CHARS : n;
        if (n == 0)
            continue;
        TextPush p;
        memset(&p, 0, sizeof(p));
        p.ox    = cols - (int)n - 1;
        p.oy    = 1 + i;
        p.scale = GPU_TEXT_SCALE;
        p.color = (i == 0) ? 0xFFFFFFFFu : 0xFFB0B0B0u;
        p.image = sc->bindless_index[image];
        memcpy(p.data, lines[i], n);
        dispatch_push(&r->vk, cmd, (ByteSpan){&p, sizeof(p)}, (uint32_t)n, 1, 1);
    }
    flush_barriers(&r->vk, cmd);
    /* Layout stays GENERAL here; the PRESENT transition below picks it up
       through the swapchain state tracker. */
}

static void pass_ldr_to_swapchain(Renderer *r, VkCommandBuffer cmd) {
    GpuProfiler *frame_prof = &r->vk.gpuprofiler[r->vk.current_frame];
    GPU_SCOPE(frame_prof, cmd, "Blit Swapchain", STAGE_XFER) {
        uint32_t image = r->vk.swapchain.current_image;

        rt_transition_all(&r->vk, cmd, &r->smaa_final[image], LAYOUT_XFER_SRC,
                          STAGE_XFER, ACCESS_XFER_R);

        image_transition_swapchain(&r->vk, cmd, &r->vk.swapchain, LAYOUT_XFER_DST,
                                   STAGE_XFER, ACCESS_XFER_W);

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

        vkCmdBlitImage(cmd, r->smaa_final[image].image, LAYOUT_XFER_SRC,
                       r->vk.swapchain.images[image], LAYOUT_XFER_DST, 1, &blit,
                       VK_FILTER_NEAREST);
    }
}

#include "src/nuklear_pass.inl"

static Renderer *g_renderer;

static void on_key(GLFWwindow *w, int key, int scancode, int action, int mods) {
    Renderer          *r   = g_renderer;
    struct nk_context *ctx = &r->ui.context;

    input_feed_key(&r->input, key, action);

    bool down = action != GLFW_RELEASE;
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    nk_input_key(ctx, NK_KEY_SHIFT, (mods & GLFW_MOD_SHIFT) != 0);
    nk_input_key(ctx, NK_KEY_CTRL, ctrl);

    switch (key) {
    case GLFW_KEY_DELETE:
        nk_input_key(ctx, NK_KEY_DEL, down);
        break;
    case GLFW_KEY_ENTER:
        nk_input_key(ctx, NK_KEY_ENTER, down);
        break;
    case GLFW_KEY_TAB:
        nk_input_key(ctx, NK_KEY_TAB, down);
        break;
    case GLFW_KEY_BACKSPACE:
        nk_input_key(ctx, NK_KEY_BACKSPACE, down);
        break;
    case GLFW_KEY_UP:
        nk_input_key(ctx, NK_KEY_UP, down);
        break;
    case GLFW_KEY_DOWN:
        nk_input_key(ctx, NK_KEY_DOWN, down);
        break;
    case GLFW_KEY_LEFT:
        nk_input_key(ctx, NK_KEY_LEFT, down && !ctrl);
        nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down && ctrl);
        break;
    case GLFW_KEY_RIGHT:
        nk_input_key(ctx, NK_KEY_RIGHT, down && !ctrl);
        nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down && ctrl);
        break;
    case GLFW_KEY_HOME:
        nk_input_key(ctx, NK_KEY_TEXT_START, down);
        nk_input_key(ctx, NK_KEY_SCROLL_START, down);
        break;
    case GLFW_KEY_END:
        nk_input_key(ctx, NK_KEY_TEXT_END, down);
        nk_input_key(ctx, NK_KEY_SCROLL_END, down);
        break;
    case GLFW_KEY_PAGE_UP:
        nk_input_key(ctx, NK_KEY_SCROLL_UP, down);
        break;
    case GLFW_KEY_PAGE_DOWN:
        nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down);
        break;
    case GLFW_KEY_C:
        nk_input_key(ctx, NK_KEY_COPY, down && ctrl);
        break;
    case GLFW_KEY_V:
        nk_input_key(ctx, NK_KEY_PASTE, down && ctrl);
        break;
    case GLFW_KEY_X:
        nk_input_key(ctx, NK_KEY_CUT, down && ctrl);
        break;
    case GLFW_KEY_Z:
        nk_input_key(ctx, NK_KEY_TEXT_UNDO, down && ctrl);
        break;
    case GLFW_KEY_Y:
        nk_input_key(ctx, NK_KEY_TEXT_REDO, down && ctrl);
        break;
    case GLFW_KEY_A:
        nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, down && ctrl);
        break;
    default:
        break;
    }

    /* Hotkeys: only on the initial press, no Ctrl, no active text edit.
     * GLFW's repeat is a separate action value, so it's excluded here. */
    if (action != GLFW_PRESS || ctrl || ctx->text_edit.active)
        return;

    if (key == GLFW_KEY_R) {
        char path[256];
        snprintf(path, sizeof(path), CAPTURE_SCREENSHOT_DIR "/screenshot_%llu.png",
                 (unsigned long long)(mu_time_now() * 1000.0 / mu_time_freq()));
        capture_take_screenshot(r, path);
    } else if (key == GLFW_KEY_F9) {
        if (r->capture.recording) {
            capture_stop_video(r);
        } else {
            char path[256];
            snprintf(path, sizeof(path), CAPTURE_VIDEO_DIR "/recording_%llu.mp4",
                     (unsigned long long)(mu_time_now() / mu_time_freq()));
            capture_start_video(r, path, 60);
        }
    }
}

static void on_char(GLFWwindow *w, unsigned int codepoint) { nk_input_unicode(&g_renderer->ui.context, codepoint); }

static void on_mouse_button(GLFWwindow *w, int button, int action, int mods) {
    Renderer          *r   = g_renderer;
    struct nk_context *ctx = &r->ui.context;

    input_feed_button(&r->input, button, action);

    enum nk_buttons b;
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:
        b = NK_BUTTON_LEFT;
        break;
    case GLFW_MOUSE_BUTTON_MIDDLE:
        b = NK_BUTTON_MIDDLE;
        break;
    case GLFW_MOUSE_BUTTON_RIGHT:
        b = NK_BUTTON_RIGHT;
        break;
    default:
        return;
    }
    nk_input_button(ctx, b, (int)ctx->input.mouse.pos.x, (int)ctx->input.mouse.pos.y, action == GLFW_PRESS);
}

static void on_cursor(GLFWwindow *w, double x, double y) {
    Renderer *r = g_renderer;
    input_feed_cursor(&r->input, x, y);
    nk_input_motion(&r->ui.context, (int)x, (int)y);
}

static void on_scroll(GLFWwindow *w, double x, double y) {
    Renderer *r = g_renderer;
    input_feed_scroll(&r->input, x, y);
    nk_input_scroll(&r->ui.context, nk_vec2((float)x, (float)y));
}

static void on_focus(GLFWwindow *w, int focused) {
    Renderer          *r   = g_renderer;
    struct nk_context *ctx = &r->ui.context;

    input_feed_focus(&r->input, focused != 0);
    if (!focused) {
        for (int k = 0; k < NK_KEY_MAX; ++k)
            nk_input_key(ctx, (enum nk_keys)k, nk_false);
        for (int b = 0; b < NK_BUTTON_MAX; ++b)
            nk_input_button(ctx, (enum nk_buttons)b, (int)ctx->input.mouse.pos.x, (int)ctx->input.mouse.pos.y,
                            nk_false);
    }
}

static void install_callbacks(Renderer *r) {
    g_renderer    = r;
    GLFWwindow *w = r->window;
    glfwSetWindowUserPointer(w, r);
    glfwSetKeyCallback(w, on_key);
    glfwSetCharCallback(w, on_char);
    glfwSetMouseButtonCallback(w, on_mouse_button);
    glfwSetCursorPosCallback(w, on_cursor);
    glfwSetScrollCallback(w, on_scroll);
    glfwSetWindowFocusCallback(w, on_focus);
}

static void platform_poll_events(Renderer *r) {
    struct nk_context *ctx = &r->ui.context;
    input_begin(&r->input);
    nk_input_begin(ctx);
    glfwPollEvents(); /* fires the callbacks above */
    nk_input_end(ctx);

    int width, height;
    glfwGetWindowSize(r->window, &width, &height);
    r->ui.width  = (float)width;
    r->ui.height = (float)height;
}

typedef struct Transform2D {
    vec2  position;
    vec2  scale;
    float rotation;
} Transform2D;

typedef struct Sprite2D {
    TextureID   texture_id;
    Transform2D transform;

    vec2 velocity;

    vec4  tint_color;
    float depth;
    vec4  uv_rect;
} Sprite2D;

void renderer_hud(Renderer *r, const char *fmt, ...) {
    if (r->hud_count >= GAME_HUD_LINES)
        return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->hud_text[r->hud_count], GAME_HUD_CHARS, fmt, ap);
    va_end(ap);
    r->hud_count++;
}

VkBackend *renderer_vk(Renderer *r) { return &r->vk; }

bool renderer_toon(Renderer *r) { return r ? r->post.toon : false; }

RenderEnvironment renderer_environment_default(void) {
    return (RenderEnvironment){
        /* Both stops carry red on purpose. A physically blue sky has almost no
           red, and a contrast curve crushes what little there is to zero, so
           any honest blue lands on pure cyan. The red here is the grade's
           inverse, not a claim about the sky. */
        .sky       = {0.18f, 0.34f, 0.76f},
        .horizon   = {0.40f, 0.62f, 0.96f},
        .ground    = {0.46f, 0.32f, 0.17f},
        .sun_dir   = {0.40f, 0.78f, 0.32f},
        .sun_color = {1.00f, 0.93f, 0.78f},
        /* The key carries the shape. Ambient this low is what keeps a shadowed
           face reading as the object's own colour rather than as blue-grey
           fill, and it leaves the tonemap headroom for the sun to clip into. */
        .sun_intensity  = 1.20f,
        .ambient        = 0.38f,
        /* Measured, not guessed: this camera's sky covers dir.y 0.55..0.76, and
           a low exponent maps that whole band to t 0.92..0.96 — a gradient with
           4% of its range left, which is a flat band. Near 1 the exponent is the
           plain zenith-to-horizon blend, and the visible slice lands across it. */
        .sky_exponent   = 1.0f,
        .sun_glow       = 0.22f,
        .sun_glow_power = 24.0f,
    };
}

void renderer_set_environment(Renderer *r, const RenderEnvironment *env) {
    if (r && env)
        r->env = *env;
}

RenderEnvironment renderer_environment(const Renderer *r) {
    return r ? r->env : renderer_environment_default();
}

void renderer_set_camera(Renderer *r, const float *view, const float *projection) {
    if (!r || !view || !projection)
        return;
    /* Rotate history: last frame's viewproj becomes prev, then rebuild. */
    mat4 v, p;
    memcpy(v, view, sizeof(v));
    memcpy(p, projection, sizeof(p));
    glm_mat4_copy(r->cam_viewproj, r->cam_prev_viewproj);
    glm_mat4_copy(v, r->cam_view);
    glm_mat4_copy(p, r->cam_proj);
    glm_mat4_mul(r->cam_proj, r->cam_view, r->cam_viewproj);
    glm_mat4_inv(r->cam_view, r->cam_inv_view);
}

struct SpriteSystem *renderer_sprites(Renderer *r) { return r->two_d ? two_d_sprites(r->two_d) : NULL; }

/* The scene background. Whoever runs first on the HDR target owns this clear:
   the 2D stage through sprite_flush, or the core when 2D is opted out. */
static const float kSceneClear[4] = {0.02f, 0.025f, 0.03f, 1.0f};

/* Draws a bright vertical gradient into the HDR target before the scene. The
   scene then loads instead of clearing, so the sky stays behind geometry. */
static void pass_sky(Renderer *r, VkCommandBuffer cmd, RenderTarget *target) {
    PassAttachment color = {.target = target, .load = LOAD_CLEAR, .store = STORE_KEEP};
    PassDesc       pd    = {.colors = &color, .color_count = 1, .pipeline = r->EnginePipelines.sky};
    begin_pass(&r->vk, cmd, &pd);
    SkyPush sp = {.exponent        = r->env.sky_exponent,
                  .glow_strength   = r->env.sun_glow,
                  .glow_power      = r->env.sun_glow_power,
                  .ground_fade     = 0.85f};
    push_constants(&r->vk, cmd, (ByteSpan){&sp, sizeof(sp)});
    vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(&r->vk, cmd, &pd);
}

bool renderer_frame(Renderer *r) {
    TracyCFrameMark;
    platform_poll_events(r);
    if (glfwWindowShouldClose(r->window)) /* was RGFW_window_shouldClose */
        return false;

    pipeline_rebuild(&r->vk);

    delete_queue_tick(&r->vk);

    if (!frame_start(r))
        return true; // swapchain out-of-date / minimized: nothing to record

    /* dt before the frame hook, because the hook reads it off GameFrame and
       update_global_data no longer runs ahead of the hook. */
    r->dt = (float)((double)r->cpu_frame_ns / 1000000000.0);

    if (r->game.frame) {
        r->hud_count    = 0;
        GameFrame frame = {
            .renderer   = r,
            .sprites    = r->two_d ? two_d_sprites(r->two_d) : NULL,
            .input      = &r->input,
            .dt         = r->dt,
            .viewport_w = r->vk.swapchain.extent.width,
            .viewport_h = r->vk.swapchain.extent.height,
        };
        r->game.frame(r->game.user, &frame);
    }

    /* Published after the frame hook, and that ordering is load-bearing: the
       hook is what sets the camera, and the sky pass — which records first —
       builds its view ray from these matrices. Publishing ahead of the hook
       left the sky one frame behind the geometry, so the gradient slid against
       the terrain on every camera turn. */
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
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->vk.bindless_system.pipeline_layout, 0, 1,
                                    &r->vk.bindless_system.set, 0, NULL);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->vk.bindless_system.pipeline_layout, 0, 1,
                                    &r->vk.bindless_system.set, 0, NULL);

            rt_transition_all(
                &r->vk, cmd, &r->depth[r->vk.swapchain.current_image], LAYOUT_DEPTH,
                STAGE_EARLY_Z | STAGE_LATE_Z,
                ACCESS_DEPTH_W | ACCESS_DEPTH_R);

            flush_barriers(&r->vk, cmd);
        }
    }
    RenderTarget *hdr = &r->hdr_color[r->vk.swapchain.current_image];
    GPU_SCOPE(frame_prof, cmd, "Scene", STAGE_COLOR) {
        if (r->two_d)
            two_d_render(r->two_d, cmd, hdr, kSceneClear);
        else
            pass_sky(r, cmd, hdr);
        if (r->game.render) {
            uint32_t image = r->vk.swapchain.current_image;
            r->game.render(r->game.user, cmd, &r->hdr_color[image], &r->depth[image]);
        }
    }

    /* Edge-triggered so the windows persist while you drag a slider; a held
       key toggling every frame is what the old local did. */
    if (key_pressed(&r->input, KEY_TAB))
        r->debug_ui_open = !r->debug_ui_open;
    if (key_pressed(&r->input, KEY_F1))
        r->show_gpu_text = !r->show_gpu_text;

    GPU_SCOPE(frame_prof, cmd, "SSAO", STAGE_CS) {
        /* The checkbox gates the pass itself: when SSAO is off post does not
           name the target either, so there is nothing to reset — the pass is
           stateless and next enabled frame recomputes it from scratch. */
        if ((r->post.flags & POST_SSAO) && r->post.ssao_strength > 0.0f)
            pass_ssao(r, cmd);
    }
    post_pass(r, cmd);
    pass_smaa(r, cmd);
    pass_ldr_to_swapchain(r, cmd);
    pass_text_overlay(r, cmd);
    render_gpu_profiler_ui(r);

    /* The game HUD belongs to the game, not to the debug windows, so it draws
       whether or not those are open — a health bar the player cannot see
       without opening a settings panel is not a HUD. */
    render_game_ui(r);

    if (r->debug_ui_open) {
        render_grade_ui(r);
        render_capture_ui(r);
    }
    pass_nuklear(r, cmd);
    capture_record(r, cmd);
    image_transition_swapchain(&r->vk, cmd, &r->vk.swapchain, LAYOUT_PRESENT,
                               STAGE_COLOR, 0);

    flush_barriers(&r->vk, cmd);

    vk_cmd_end(cmd);

    vk_frame_submit(&r->vk);

    return true;
}

void renderer_destroy(Renderer *r) {
    dmon_deinit();
    wait_idle(&r->vk);
    delete_queue_drain(&r->vk);
    post_settings_save(&r->post);
    if (r->game.shutdown)
        r->game.shutdown(r->game.user);
    forEach(i, CAPTURE_SLOTS) {
        r->vk.current_frame = (i + 1) % CAPTURE_SLOTS;
        capture_consume(r);
    }
    nuklear_shutdown(r);
    capture_shutdown(r);
    forEach(i, MAX_SWAPCHAIN_IMAGES) {
        rt_destroy(&r->vk, &r->depth[i]);
        rt_destroy(&r->vk, &r->hdr_color[i]);
        rt_destroy(&r->vk, &r->ldr_color[i]);
        rt_destroy(&r->vk, &r->smaa_final[i]);
        rt_destroy(&r->vk, &r->smaa_edges[i]);
        rt_destroy(&r->vk, &r->smaa_weights[i]);
        forEach(k, BLOOM_MIPS) rt_destroy(&r->vk, &r->bloom_mip[i][k]);
        rt_destroy(&r->vk, &r->ssao[i]);
    }
    if (r->two_d) {
        two_d_destroy(r->two_d);
        r->two_d = NULL;
    }
    forEach(i, MAX_FRAMES_IN_FLIGHT) destroy_buffer(&r->vk, &r->global_ubo[i]);
    destroy_buffer(&r->vk, &r->readback_buffer);
    pipeline_cache_save(r->vk.devc.device, r->vk.devc.physical_device, r->vk.devc.pipeline_cache, "pipeline_cache.bin");
    vk_mem_report(&r->vk, "shutdown");
    vk_backend_destroy(&r->vk);
    vkDestroySurfaceKHR(r->vk.instance.instance, r->vk.surface, NULL);
    vk_instance_destroy(&r->vk);
    glfwDestroyWindow(r->window);
    glfwTerminate();
    free(r);
}
