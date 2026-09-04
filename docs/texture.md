The system has three layers that integrate with your existing bindless architecture (mu_id_pool, global textures[] array, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE at binding 0).
1. Core Types
// --- Texture creation descriptor (your existing pattern) ---
typedef struct {
    uint32_t         width;
    uint32_t         height;
    uint32_t         mip_count;   // 0 = auto, 1 = no mips, >1 = explicit
    VkFormat         format;
    VkImageUsageFlags usage;
    const char*      debug_name;
} TextureCreateDesc;

// --- GPU texture resource ---
typedef struct {
    VkImage          image;
    VmaAllocation    allocation;
    VkImageView      view;
    VkImageView      mip_views[MAX_MIPS];  // per-mip views for RT/storage

    VkFormat         format;
    uint32_t         width;
    uint32_t         height;
    uint32_t         mip_count;
    uint32_t         layer_count;

    VkImageUsageFlags usage;

    ImageState       state;               // current layout/access/stage
    ImageState       mip_states[MAX_MIPS];

    uint32_t         bindless_index;      // index into global descriptor array

    char             debug_name[64];
} Texture;
2. File Structure (all in main.c)
// ── Section A: Types ──────────────────────────────────
TextureCreateDesc, Texture, TextureID          // above existing Renderer struct

// ── Section B: Global state ───────────────────────────
Texture textures[MAX_BINDLESS_TEXTURES];       // lives next to your mu_id_pool declarations

// ── Section C: Synchronous API ────────────────────────
texture_create()       // alloc image + view + assign bindless index
texture_destroy()      // free image + view + release bindless index
texture_upload()       // staging buffer copy (one-time cmd), UNDEFINED→TRANSFER_DST→SHADER_READ
texture_generate_mips_cpu()   // CPU-side box filter, writes all mip levels to staging, uploads
texture_generate_mips_gpu()   // vkCmdBlitImage chain from mip 0 down
texture_transition()   // manual layout transition via barrier

// ── Section D: Async system ──────────────────────────
TextureUploadRequest   // {data, size, width, height, format, mip_gen_mode, out_id}
TransferRingBuffer     // triple-buffered staging ring
texture_load_async()   // pushes request into lock-free SPSC queue
texture_pump_uploads() // called once per frame in renderer_begin/flush:
                       //   1. dequeue requests
                       //   2. write to staging ring
                       //   3. if previous frame's transfer is done → submit new batch
                       //   4. retire completed staging slots
3. Synchronous API (blocking, used at init time)
texture_create — called during renderer_create() for static textures:
texture_create(r, &desc) → TextureID
  1. id = mu_id_pool_create_id(&r->texture_pool)
  2. VkImageCreateInfo (imageType=2D, samples=1, tiling=OPTIMAL)
  3. vmaCreateImage()
  4. vkCreateImageView() → tex->view
  5. for each mip: create per-mip view if mip_count > 1
  6. write tex->bindless_index = id
  7. update bindless descriptor: vkUpdateDescriptorSets
     (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, tex->view, SHADER_READ_ONLY_OPTIMAL)
  8. return id
texture_upload — staging buffer copy, reuses the exact pattern already in your SMAA/dummy texture code:
texture_upload(r, id, data, data_size)
  1. create_buffer(TRANSFER_SRC, CPU_ONLY, staging)
  2. memcpy staging.mapping = data
  3. begin one-time cmd
  4. barrier: UNDEFINED → TRANSFER_DST
  5. vkCmdCopyBufferToImage(staging → tex->image)
  6. barrier: TRANSFER_DST → SHADER_READ_ONLY
  7. end one-time cmd
  8. destroy_buffer(staging)
  9. tex->state = {STAGE=FRAGMENT_SHADER, ACCESS=SHADER_READ, LAYOUT=SHADER_READ_ONLY}
texture_generate_mips_cpu — CPU box filter, uploads full mip chain in one copy:
texture_generate_mips_cpu(r, id, base_pixels)
  1. compute total mip size: sum of (w*h*bytesPerPixel) for each level
  2. allocate temp buffer, write mip 0 from base_pixels
  3. for mip = 1..mip_count:
       downsample 2×2 → 1×1 box filter from mip-1
  4. staging upload of entire mip chain
     (VkBufferImageCopy with imageSubresource.baseMipLevel per level)
  5. barrier: UNDEFINED → SHADER_READ_ONLY (full mip chain)
texture_generate_mips_gpu — vkCmdBlitImage chain:
texture_generate_mips_gpu(r, id)
  1. begin one-time cmd
  2. barrier: mip 0 → TRANSFER_SRC
  3. for mip = 1..mip_count:
       barrier: mip N → TRANSFER_DST
       vkCmdBlitImage(mip N-1 → mip N, LINEAR filter)
       barrier: mip N → TRANSFER_SRC
  4. barrier: all mips → SHADER_READ_ONLY
  5. end one-time cmd
4. Async Upload System
Designed around triple-buffered staging ring + SPSC queue (single producer = main thread, single consumer = transfer thread or frame pump).
typedef enum {
    MIP_GEN_NONE,       // data already contains full mip chain
    MIP_GEN_CPU,        // CPU box filter before upload
    MIP_GEN_GPU,        // GPU vkCmdBlitImage chain
} MipGenMode;

typedef struct {
    void*              data;           // pixel data (caller-owned, copied)
    uint32_t           data_size;
    uint32_t           width;
    uint32_t           height;
    VkFormat           format;
    MipGenMode         mip_gen;
    bool               generate_mips; // convenience: mip_gen != NONE
    TextureID          out_id;        // written by texture_load_async
} TextureUploadRequest;

// Triple-buffered staging slot
typedef struct {
    VkBuffer     buffer;
    VmaAllocation allocation;
    void*        mapping;
    uint32_t     size;
    VkFence      fence;           // signaled when GPU is done reading this slot
} StagingSlot;

#define STAGING_SLOTS 3

typedef struct {
    StagingSlot  slots[STAGING_SLOTS];
    uint32_t     head;            // next slot to write to (CPU)
    uint32_t     tail;            // next slot to retire (GPU done)
    uint32_t     in_flight;       // 0, 1, or 2 submissions in flight
} TransferRingBuffer;
Per-frame pump (texture_pump_uploads, called once per frame before rendering):
texture_pump_uploads(r):
  1. RETIRE: if in_flight > 0 and slots[tail].fence is signaled:
       unmap, advance tail, in_flight--

  2. DEQUEUE + WRITE: while queue is not empty AND slots[head] is free:
       peek request
       if request.data_size > slots[head].size: grow the slot
       memcpy(slots[head].mapping, request.data)
       submit = {slot=head, request}

  3. SUBMIT: if in_flight < STAGING_SLOTS and there's a pending submit:
       begin one-time cmd
       barrier: UNDEFINED → TRANSFER_DST
       vkCmdCopyBufferToImage
       if request.mip_gen == GPU: run blit chain
       barrier: → SHADER_READ_ONLY
       end cmd
       in_flight++
       advance head

  4. PROMOTE: if request.out_id texture is now in SHADER_READ_ONLY:
       mark texture as "ready" (optional flag)
The key property: texture_load_async returns a TextureID immediately (the slot is reserved via mu_id_pool), but the texture isn't usable in shaders until texture_pump_uploads completes its transfer. You can check tex->state.validity == IMAGE_STATE_VALID or add a dedicated ready flag.
5. Layout/Barrier Tracking
Integrate with your existing ImageState:
void texture_transition(Texture* tex, VkImageLayout new_layout,
                        VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access,
                        VkCommandBuffer cmd)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = tex->state.layout,
        .newLayout = new_layout,
        .srcAccessMask = tex->state.access,
        .dstAccessMask = dst_access,
        .image = tex->image,
        .subresourceRange = {COLOR_BIT, 0, tex->mip_count, 0, tex->layer_count}
    };
    vkCmdPipelineBarrier(cmd, tex->state.stage, dst_stage, ...);
    tex->state = (ImageState){dst_stage, dst_access, new_layout, /*queue_family=*/0, IMAGE_STATE_VALID};
}
6. Texture Destruction
void texture_destroy(Renderer* r, TextureID id)
{
    Texture* tex = &textures[id];
    // wait if async upload is in flight for this texture
    vkDeviceWaitIdle(r->devc.device);  // or use per-texture fence
    vkDestroyImageView(r->devc.device, tex->view, NULL);
    for (uint32_t m = 0; m < tex->mip_count; m++)
        if (tex->mip_views[m]) vkDestroyImageView(..., tex->mip_views[m], NULL);
    vmaDestroyImage(r->devc.vmaallocator, tex->image, tex->allocation);
    *tex = (Texture){0};
    mu_id_pool_release_id(&r->texture_pool, id);
}
7. Summary of Functions to Implement
Function	Section	Notes
texture_create	sync	image + view + bindless write
texture_destroy	sync	 
texture_upload	sync	staging buffer pattern (already proven in your code)
texture_generate_mips_cpu	sync	CPU box filter, full mip chain upload
texture_generate_mips_gpu	sync	vkCmdBlitImage chain
texture_transition	sync	manual barrier wrapper
texture_load_async	async	enqueue request, reserve TextureID
texture_pump_uploads	async	per-frame: dequeue → write staging → submit transfer → retire
transfer_ring_init	async	allocate triple-buffered staging
transfer_ring_destroy	async	 
8. Integration Points
- renderer_create() calls transfer_ring_init() after VMA setup, before SMAA texture loads
- SMAA/dummy textures switch from inline staging code to texture_create + texture_upload + texture_generate_mips_gpu (simplifies the 200 lines of copy-paste barrier code)
- Game code calls texture_load_async() for runtime loads, texture_pump_uploads() once per frame
- Shaders access textures via texture[tex.bindless_index] in GLSL (set 0, binding 0)
