#include "sprite.h"

#include "../../external/stb/stb_image.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SPRITE_ATLAS_INITIAL 2048u
#define SPRITE_ATLAS_MAX     8192u

#define SPRITE_WIDE_MASK                                                                                     \
    (VK_IMAGE_ASPECT_COLOR_BIT) /* keep the range expression readable below */

static uint32_t quantize_uv(float v) {
    float q = v * 65535.0f;
    if (q < 0.0f)
        q = 0.0f;
    if (q > 65535.0f)
        q = 65535.0f;
    return (uint32_t)(q + 0.5f);
}

static bool atlas_create(SpriteSystem *s, uint32_t width, uint32_t height) {
    if (s->atlas_count >= SPRITE_MAX_ATLASES)
        return false;

    TextureCreateDesc desc = {
        .width      = width,
        .height     = height,
        .mip_count  = 1,
        .format     = VK_FORMAT_R8G8B8A8_SRGB,
        .usage      = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debug_name = "sprite_atlas",
    };

    TextureID id = create_texture(s->vk, &desc);
    if (id == UINT32_MAX)
        return false;

    SpriteAtlas *a     = &s->atlases[s->atlas_count++];
    *a                 = (SpriteAtlas){.texture = id, .width = width, .height = height};
    return true;
}

/* Classic shelf packer: one open shelf, recycled whenever a taller sprite shows
   up. Rects abut exactly; the half-texel UV inset keeps bilinear filtering
   inside the rect, so no gutter texels are needed. */
static bool atlas_alloc(SpriteAtlas *a, uint32_t w, uint32_t h, uint32_t *out_x, uint32_t *out_y) {
    if (w > a->width || h > a->height)
        return false;

    if (a->cursor_x + w > a->width || h > a->shelf_height) {
        uint32_t next = a->cursor_y + a->shelf_height;
        if (next + h > a->height)
            return false;
        a->cursor_y     = next;
        a->cursor_x     = 0;
        a->shelf_height = h;
    }

    *out_x = a->cursor_x;
    *out_y = a->cursor_y;
    a->cursor_x += w;
    return true;
}

/* Double the atlas height so existing shelves keep their coordinates. Loading
   only: the caller waits for idle first, because retiring the old image has to
   outlive every frame still sampling it. */
static bool atlas_grow(SpriteSystem *s, uint32_t index) {
    SpriteAtlas *a = &s->atlases[index];
    uint32_t     new_height = a->height * 2u;
    if (a->height >= SPRITE_ATLAS_MAX || new_height > SPRITE_ATLAS_MAX)
        return false;

    wait_idle(s->vk);

    TextureCreateDesc desc = {
        .width      = a->width,
        .height     = new_height,
        .mip_count  = 1,
        .format     = VK_FORMAT_R8G8B8A8_SRGB,
        .usage      = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debug_name = "sprite_atlas",
    };

    TextureID fresh = create_texture(s->vk, &desc);
    if (fresh == UINT32_MAX)
        return false;

    Texture *old_image = &s->vk->texture_system.textures[a->texture];
    Texture *new_image = &s->vk->texture_system.textures[fresh];

    /* Nothing has been packed-and-uploaded yet, so the old image holds nothing
       worth carrying over. */
    if (!a->uploaded) {
        destroy_texture(s->vk, a->texture);
        a->texture = fresh;
        a->height  = new_height;
        return true;
    }

    VkImageMemoryBarrier to_src = {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask         = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask         = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout             = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED,
        .image                 = old_image->image,
        .subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageMemoryBarrier to_dst = {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask         = 0,
        .dstAccessMask         = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout             = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout             = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED,
        .image                 = new_image->image,
        .subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };

    VkCommandBuffer cmd = vk_begin_one_time_cmd(s->vk->devc.device, s->vk->one_time_gfx_pool);

    VkImageMemoryBarrier first[2] = {to_src, to_dst};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         2, first);

    VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset      = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset      = {0, 0, 0},
        .extent         = {a->width, a->height, 1},
    };
    vkCmdCopyImage(cmd, old_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_image->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_read);

    vk_end_one_time_cmd(s->vk->devc.device, s->vk->devc.graphics_queue, s->vk->one_time_gfx_pool, cmd);

    destroy_texture(s->vk, a->texture);

    a->texture       = fresh;
    a->height        = new_height;
    a->shelf_height  = a->shelf_height; /* unchanged: shelves keep their y */
    return true;
}

static bool atlas_reserve(SpriteSystem *s, uint32_t w, uint32_t h, uint32_t *out_atlas, uint32_t *out_x,
                          uint32_t *out_y) {
    if (h > SPRITE_ATLAS_MAX || w > SPRITE_ATLAS_MAX)
        return false;

    for (uint32_t i = 0; i < s->atlas_count; i++)
        if (atlas_alloc(&s->atlases[i], w, h, out_x, out_y)) {
            *out_atlas = i;
            return true;
        }

    if (s->atlas_count > 0) {
        uint32_t last = s->atlas_count - 1;
        while (atlas_grow(s, last)) {
            if (atlas_alloc(&s->atlases[last], w, h, out_x, out_y)) {
                *out_atlas = last;
                return true;
            }
        }
    }

    uint32_t width  = w > SPRITE_ATLAS_INITIAL ? w : SPRITE_ATLAS_INITIAL;
    uint32_t height = h > SPRITE_ATLAS_INITIAL ? h : SPRITE_ATLAS_INITIAL;
    if (!atlas_create(s, width, height))
        return false;

    uint32_t last = s->atlas_count - 1;
    if (!atlas_alloc(&s->atlases[last], w, h, out_x, out_y))
        return false;
    *out_atlas = last;
    return true;
}

/* One submit per region. The first region of an atlas leaves UNDEFINED; later
   ones round-trip through SHADER_READ_ONLY, which is where the sampler leaves
   it. Loading is expected to happen before frames are in flight. */
static bool atlas_upload(SpriteSystem *s, uint32_t atlas_index, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const void *pixels) {
    SpriteAtlas *a  = &s->atlases[atlas_index];
    VkBackend   *vk = s->vk;
    Texture     *tex = &vk->texture_system.textures[a->texture];
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4u;

    Buffer staging;
    if (!create_buffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging))
        return false;
    memcpy(staging.mapping, pixels, (size_t)bytes);

    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier to_dst = {
        .sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask        = a->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0,
        .dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout            = a->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED,
        .image                = tex->image,
        .subresourceRange     = range,
    };

    VkCommandBuffer cmd = vk_begin_one_time_cmd(vk->devc.device, vk->one_time_gfx_pool);

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_dst);

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset       = {(int32_t)x, (int32_t)y, 0},
        .imageExtent       = {w, h, 1},
    };
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_read);

    vk_end_one_time_cmd(vk->devc.device, vk->devc.graphics_queue, vk->one_time_gfx_pool, cmd);
    destroy_buffer(vk, &staging);

    a->uploaded = true;
    return true;
}

void picture_system_init(SpriteSystem *s) {
    s->picture_capacity = SPRITE_MAX_PICTURES;
    s->pictures         = (Picture *)calloc(s->picture_capacity, sizeof(Picture));
    s->picture_count    = 0;
    s->atlas_count      = 0;
    assert(s->pictures);
    atlas_create(s, SPRITE_ATLAS_INITIAL, SPRITE_ATLAS_INITIAL);
}

void picture_system_destroy(SpriteSystem *s) {
    for (uint32_t i = 0; i < s->atlas_count; i++)
        destroy_texture(s->vk, s->atlases[i].texture);
    s->atlas_count = 0;
    free(s->pictures);
    s->pictures         = NULL;
    s->picture_count    = 0;
    s->picture_capacity = 0;
}

PictureId picture_load_rgba(SpriteSystem *s, const void *pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0)
        return UINT32_MAX;
    if (s->picture_count >= s->picture_capacity)
        return UINT32_MAX;

    uint32_t atlas = 0, x = 0, y = 0;
    if (!atlas_reserve(s, width, height, &atlas, &x, &y))
        return UINT32_MAX;
    if (!atlas_upload(s, atlas, x, y, width, height, pixels))
        return UINT32_MAX;

    SpriteAtlas *a = &s->atlases[atlas];
    Picture     *p = &s->pictures[s->picture_count];

    p->atlas   = atlas;
    p->sampler = SPRITE_SAMPLER_LINEAR_CLAMP;
    p->uv0     = quantize_uv((x + 0.5f) / (float)a->width) |
             (quantize_uv((y + 0.5f) / (float)a->height) << 16);
    p->uv1     = quantize_uv((x + width - 0.5f) / (float)a->width) |
             (quantize_uv((y + height - 0.5f) / (float)a->height) << 16);
    p->w       = width;
    p->h       = height;

    return s->picture_count++;
}

PictureId picture_load(SpriteSystem *s, const char *path) {
    int            width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels) {
        log_error("[sprite] failed to load '%s': %s", path, stbi_failure_reason());
        return UINT32_MAX;
    }

    PictureId id = picture_load_rgba(s, pixels, (uint32_t)width, (uint32_t)height);
    stbi_image_free(pixels);
    return id;
}
