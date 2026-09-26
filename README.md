# mu_gfx

## Coordinate conventions

`begin_pass` (`vk.c`) calls `vk_cmd_set_viewport_scissor(cmd, extent)` for every
pass, and that helper installs a **negative-height** viewport:

```c
VkViewport vp = {.x = 0, .y = extent.height,
                 .width = extent.width, .height = -(float)extent.height};
```

So for every pass the framebuffer mapping is

```
fb_y = height * (1 - ndc.y) / 2      ndc.x = 2 * fb_x / width - 1
```

or, solved for NDC, `ndc.y = 1 - 2 * fb_y / height`. Framebuffer rows are y-down
with row 0 at the top.

### Which sign a pass needs

The frame chain (`renderer.c`) is:

| stage | kind | Y flip |
|---|---|---|
| `pass_sprites` → `hdr_color` | graphics | writes hdr |
| `post_pass` (`shaders/postprocess.slang`) | compute, `src.Load(pix)` → `dst[pix]` | none |
| `pass_smaa`: edge, weight, blend | 3 graphics fullscreen passes | 3 |
| `pass_ldr_to_swapchain` (`vkCmdBlitImage`) | transfer | none |
| `pass_nuklear` → swapchain | graphics | writes swapchain |

Every **graphics fullscreen pass** emits `uv = (ndc + 1) / 2` while the
negative-height viewport puts `uv.y = 0` at the bottom of its target, so each one
mirrors the image. Compute passes and blits do not.

Anything that renders into `hdr_color` therefore reaches the screen with **one**
net flip, so a world pixel `p` is emitted as

```slang
ndc = 2 * p / viewport - 1        // pixel_to_uv() in shaders/common.slang
```

A pass that draws **directly into the swapchain** (`src/nuklear_pass.inl`) sees no
flips at all and needs the opposite form:

```slang
ndc.y = 1 - 2 * y / height        // shaders/nuklear.slang
```

`shaders/sprite_cull.slang` tests in pixel space against `pc.viewport` and never
enters NDC, so it always uses the identity `fb_y = p.y`.

### Getting it wrong

Dropping the `- 1` — `ndc.y = 2 * p.y / height` — shifts every sprite down by
half the screen: the middle of the view lands on the bottom edge. Changing the
flip count (adding or removing a fullscreen graphics pass) turns a shift into a
mirror.

Do not repair orientation by editing `vk_cmd_set_viewport_scissor`. The negative
height is deliberate and the whole post chain depends on it. Fix the shader that
disagrees with the convention instead, and update this table when a pass is added
or removed.
