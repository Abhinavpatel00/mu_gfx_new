/* mu_texcomp.h - single-header GPU block texture codecs (DOD + stb style)
 *
 * v1 slice: BC1 + BC4 + BC5 decode + fast encode working.
 * All other formats (BC3, BC6H, BC7, ETC1, ETC2/EAC, ASTC) are declared
 * in the API so callers can code against the final shape, but return
 * MU_TEXCOMP_ERR_NOT_IMPLEMENTED until their slice lands.
 *
 * Roadmap: BC3 (BC1+BC4) -> BC7/BC6H -> ETC1/ETC2/EAC -> ASTC LDR -> ASTC HDR.
 * Each slice adds decode first (to prove metrics), then fast encode, then ultra.
 *
 * Design (per AGENTS.md):
 * - pure C99, no C++ headers, no malloc in hot path, no hidden globals.
 * - preparation (validate, resolve sizes) at boundaries; execution (block loops)
 *   operates on already-valid data with minimal branching.
 * - caller owns all memory; image APIs take dense row-major inputs.
 *
 * Usage:
 *   #define MU_TEXCOMP_IMPLEMENTATION
 *   #include "mu_texcomp.h"
 * Link with -lm when MU_TEXCOMP_ENABLE_DECODE is on (psnr metrics only).
 *
 * Knobs:
 *   #define MU_TEXCOMP_STATIC               -> all functions static
 *   #define MU_TEXCOMP_ENABLE_DECODE 0      -> strip CPU decode (runtime only)
 *   #define MU_TEXCOMP_ASSERT(x)            -> override assert
 *
 * Quality: FAST = single-mode bbox fit (real-time). ULTRA = multi-mode and
 * partition search with exact decode-measure selection (offline baker).
 * ULTRA needs MU_TEXCOMP_ENABLE_DECODE=1 (falls back to FAST when off).
 *
 * Decode != loading (see docs at bottom):
 *   loading moves compressed blocks disk -> RAM -> VRAM untouched.
 *   decode expands blocks to RGBA on CPU (tools/metrics/fallback only).
 */
#ifndef MU_TEXCOMP_H
#define MU_TEXCOMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- */
/* linkage                                                          */
/* ---------------------------------------------------------------- */

#ifdef MU_TEXCOMP_STATIC
#define MU_TEXCOMP_API static
#else
#define MU_TEXCOMP_API extern
#endif

#ifndef MU_TEXCOMP_ASSERT
#include <assert.h>
#define MU_TEXCOMP_ASSERT(x) assert(x)
#endif

#ifndef MU_TEXCOMP_ENABLE_DECODE
#define MU_TEXCOMP_ENABLE_DECODE 1
#endif

/* ---------------------------------------------------------------- */
/* spans (by value)                                                 */
/* ---------------------------------------------------------------- */

typedef struct Mu_Texcomp_Bytes {
    uint8_t       *data;
    uint64_t      size;
} Mu_Texcomp_Bytes;

typedef struct Mu_Texcomp_ConstBytes {
    uint8_t const *data;
    uint64_t      size;
} Mu_Texcomp_ConstBytes;

/* 4x4 RGBA8 tile, AoS: complete records processed together in block kernels. */
typedef struct Mu_Texcomp_RgbaTile {
    uint8_t   px[16][4];
} Mu_Texcomp_RgbaTile;

/* ---------------------------------------------------------------- */
/* formats + errors                                                 */
/* ---------------------------------------------------------------- */

typedef enum Mu_Texcomp_Format {
    MU_TEXCOMP_FMT_BC1 = 0,
    MU_TEXCOMP_FMT_BC2 = 1,
    MU_TEXCOMP_FMT_BC3 = 2,
    MU_TEXCOMP_FMT_BC4 = 3,
    MU_TEXCOMP_FMT_BC5 = 4,
    MU_TEXCOMP_FMT_BC6H = 5,
    MU_TEXCOMP_FMT_BC7 = 6,
    MU_TEXCOMP_FMT_ETC1 = 7,
    MU_TEXCOMP_FMT_ETC2_RGB = 8,
    MU_TEXCOMP_FMT_ETC2_RGBA = 9,
    MU_TEXCOMP_FMT_EAC_R = 10,
    MU_TEXCOMP_FMT_EAC_RG = 11,
    MU_TEXCOMP_FMT_ASTC_4X4 = 12,
    MU_TEXCOMP_FMT_ASTC_6X6 = 13,
    MU_TEXCOMP_FMT_ASTC_8X8 = 14,
    MU_TEXCOMP_FMT_COUNT = 15
} Mu_Texcomp_Format;

typedef enum Mu_Texcomp_Quality {
    MU_TEXCOMP_FAST = 0,
    MU_TEXCOMP_ULTRA = 1
} Mu_Texcomp_Quality;

typedef enum Mu_Texcomp_Error {
    MU_TEXCOMP_OK = 0,
    MU_TEXCOMP_ERR_INVALID_INPUT = 1,
    MU_TEXCOMP_ERR_DST_TOO_SMALL = 2,
    MU_TEXCOMP_ERR_NOT_IMPLEMENTED = 3
} Mu_Texcomp_Error;

/* Encode params: preparation-time knob block. Fill with designated
 * initializer, no init call required. Inner loops read the resolved copy. */
typedef struct Mu_Texcomp_EncodeParams {
    Mu_Texcomp_Format   format;
    Mu_Texcomp_Quality  quality;
    uint32_t            perceptual;   /* reserved for BC7/ASTC ultra; 0 = linear */
} Mu_Texcomp_EncodeParams;

#define MU_TEXCOMP_ENCODE_PARAMS_DEFAULT(f, q) \
    ((Mu_Texcomp_EncodeParams){ .format = (f), .quality = (q), .perceptual = 0 })

/* ---------------------------------------------------------------- */
/* block geometry (all v1 formats are 4x4; ASTC sizes map to dims)  */
/* ---------------------------------------------------------------- */

MU_TEXCOMP_API uint64_t mu_texcomp_block_bytes(Mu_Texcomp_Format fmt);
MU_TEXCOMP_API void     mu_texcomp_block_dim(Mu_Texcomp_Format fmt, uint32_t *w_out, uint32_t *h_out);
MU_TEXCOMP_API uint64_t mu_texcomp_image_block_count(uint32_t w, uint32_t h, Mu_Texcomp_Format fmt);
MU_TEXCOMP_API uint64_t mu_texcomp_image_bytes(uint32_t w, uint32_t h, Mu_Texcomp_Format fmt);

/* ---------------------------------------------------------------- */
/* block encode (preparation already done: inputs validated by      */
/* image wrappers; these assume 16 texels + 8/16 dst bytes valid)   */
/* ---------------------------------------------------------------- */

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc1(Mu_Texcomp_RgbaTile const *src, uint8_t dst[8], Mu_Texcomp_Quality quality);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc2(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Quality quality);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc4(uint8_t const src[16], uint8_t dst[8]);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc5(uint8_t const src_r[16], uint8_t const src_g[16], uint8_t dst[16]);

/* Declared for final API shape; v1 returns NOT_IMPLEMENTED. */
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc3(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Quality quality);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc6h(float const src_rgb[16][3], uint8_t dst[16], Mu_Texcomp_Quality quality, int is_signed);
MU_TEXCOMP_API uint16_t mu_texcomp_float_to_half(float f);
MU_TEXCOMP_API float    mu_texcomp_half_to_float(uint16_t h);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc6h(uint8_t const src[16], float dst_rgb[16][3], int is_signed);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_image_bc6h(Mu_Texcomp_ConstBytes src_f32rgb, uint32_t w, uint32_t h, uint32_t stride,
    Mu_Texcomp_Bytes dst_blocks, Mu_Texcomp_Quality quality, int is_signed);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_image_bc6h(Mu_Texcomp_ConstBytes src_blocks, uint32_t w, uint32_t h,
    Mu_Texcomp_Bytes dst_f32rgb, uint32_t stride, int is_signed);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc7(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Quality quality);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_etc1(Mu_Texcomp_RgbaTile const *src, uint8_t dst[8], Mu_Texcomp_Quality quality);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_etc2(Mu_Texcomp_RgbaTile const *src, uint8_t dst[8], Mu_Texcomp_Quality quality);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_eac(uint8_t const src[16], uint8_t dst[8], int is_signed);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_astc(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Format fmt, Mu_Texcomp_Quality quality);

/* ---------------------------------------------------------------- */
/* image encode (validates once, then streams blocks, no per-block  */
/* validation; pads edge blocks by clamping, matching DDS/KTX)      */
/* ---------------------------------------------------------------- */

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_image(Mu_Texcomp_ConstBytes src_rgba8, uint32_t w, uint32_t h, uint32_t stride,
    Mu_Texcomp_Bytes dst_blocks, Mu_Texcomp_EncodeParams params);

/* ---------------------------------------------------------------- */
/* decode (CPU expansion; NOT the runtime path - GPU samples the    */
/* blocks directly. Gated so shipping builds pay zero cost.)        */
/* ---------------------------------------------------------------- */

#if MU_TEXCOMP_ENABLE_DECODE
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc1(uint8_t const src[8], Mu_Texcomp_RgbaTile *dst);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc2(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc3(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc7(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc4(uint8_t const src[8], uint8_t dst[16]);
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc5(uint8_t const src[16], uint8_t dst_r[16], uint8_t dst_g[16]);

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_image(Mu_Texcomp_ConstBytes src_blocks, uint32_t w, uint32_t h,
    Mu_Texcomp_Bytes dst_rgba8, uint32_t stride, Mu_Texcomp_Format fmt);
MU_TEXCOMP_API double mu_texcomp_psnr_rgba(Mu_Texcomp_ConstBytes a_rgba8, Mu_Texcomp_ConstBytes b_rgba8);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MU_TEXCOMP_H */

/* ================================================================== */
#ifdef MU_TEXCOMP_IMPLEMENTATION
/* ================================================================== */

#ifndef MU_TEXCOMP_H_IMPL
#define MU_TEXCOMP_H_IMPL

#ifdef __cplusplus
extern "C" {
#endif

MU_TEXCOMP_API uint64_t mu_texcomp_block_bytes(Mu_Texcomp_Format fmt)
{
    switch (fmt) {
    case MU_TEXCOMP_FMT_BC1:      return 8;
    case MU_TEXCOMP_FMT_BC4:      return 8;
    case MU_TEXCOMP_FMT_ETC1:     return 8;
    case MU_TEXCOMP_FMT_EAC_R:    return 8;
    case MU_TEXCOMP_FMT_BC2:      return 16;
    case MU_TEXCOMP_FMT_BC3:      return 16;
    case MU_TEXCOMP_FMT_BC5:      return 16;
    case MU_TEXCOMP_FMT_BC6H:     return 16;
    case MU_TEXCOMP_FMT_BC7:      return 16;
    case MU_TEXCOMP_FMT_ETC2_RGB: return 8;
    case MU_TEXCOMP_FMT_ETC2_RGBA:return 16;
    case MU_TEXCOMP_FMT_EAC_RG:   return 16;
    case MU_TEXCOMP_FMT_ASTC_4X4: return 16;
    case MU_TEXCOMP_FMT_ASTC_6X6: return 16;
    case MU_TEXCOMP_FMT_ASTC_8X8: return 16;
    default: return 0;
    }
}

MU_TEXCOMP_API void mu_texcomp_block_dim(Mu_Texcomp_Format fmt, uint32_t *w_out, uint32_t *h_out)
{
    uint32_t   w = 4;
    uint32_t   h = 4;
    if (fmt == MU_TEXCOMP_FMT_ASTC_6X6) { w = 6; h = 6; }
    if (fmt == MU_TEXCOMP_FMT_ASTC_8X8) { w = 8; h = 8; }
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
}

MU_TEXCOMP_API uint64_t mu_texcomp_image_block_count(uint32_t w, uint32_t h, Mu_Texcomp_Format fmt)
{
    uint32_t   bw = 4;
    uint32_t   bh = 4;
    mu_texcomp_block_dim(fmt, &bw, &bh);
    if (w == 0 || h == 0) return 0;
    return (uint64_t)((w + bw - 1u) / bw) * (uint64_t)((h + bh - 1u) / bh);
}

MU_TEXCOMP_API uint64_t mu_texcomp_image_bytes(uint32_t w, uint32_t h, Mu_Texcomp_Format fmt)
{
    return mu_texcomp_image_block_count(w, h, fmt) * mu_texcomp_block_bytes(fmt);
}

/* ---- internal: 565 pack/unpack (hot, branchless) ---- */

#include <string.h>

/* ---- half conversion: f32->h16 flushes denormals (documented fast-encoder
 * choice) and maps NaN->0, INF->max; h16->f32 is exact incl. denormals. ---- */
MU_TEXCOMP_API uint16_t mu_texcomp_float_to_half(float f)
{
    uint32_t   x, sign, ax, exp, mant, he, hm, rs;
    memcpy(&x, &f, 4);
    sign = (x >> 16) & 0x8000u;
    ax = x & 0x7FFFFFFFu;
    if (ax >= 0x7F800000u) {
        if (ax == 0x7F800000u) return (uint16_t)(sign | 0x7BFFu);
        return 0;
    }
    if (ax >= 0x47800000u) return (uint16_t)(sign | 0x7BFFu);
    if (ax < 0x38800000u) return (uint16_t)sign;
    exp = ax >> 23;
    mant = ax & 0x7FFFFFu;
    he = exp - 112u;
    hm = mant >> 13;
    rs = mant & 0x1FFFu;
    if (rs > 0x1000u || (rs == 0x1000u && (hm & 1u))) {
        hm++;
        if (hm == 0x400u) { hm = 0; he++; }
    }
    if (he >= 31u) return (uint16_t)(sign | 0x7BFFu);
    return (uint16_t)(sign | (he << 10) | hm);
}

MU_TEXCOMP_API float mu_texcomp_half_to_float(uint16_t hh)
{
    uint32_t   o = ((uint32_t)(hh & 0x7FFFu) << 13);
    uint32_t   exp = o & (0x7C00u << 13);
    float      f;
    o += (127u - 15u) << 23;
    if (exp == (0x7C00u << 13)) {
        o += (128u - 16u) << 23;
    } else if (exp == 0) {
        uint32_t mb = 113u << 23;
        float    m;
        o += 1u << 23;
        memcpy(&f, &o, 4);
        memcpy(&m, &mb, 4);
        f -= m;
        memcpy(&o, &f, 4);
    }
    o |= ((uint32_t)(hh & 0x8000u) << 16);
    memcpy(&f, &o, 4);
    return f;
}

/* ---- sequential LSB-first bit reader/writer over the 16-byte block ---- */
typedef struct Mu_Texcomp_BitReader { uint8_t const *b; uint32_t pos; } Mu_Texcomp_BitReader;
typedef struct Mu_Texcomp_BitWriter { uint8_t *b; uint32_t pos; } Mu_Texcomp_BitWriter;

#if MU_TEXCOMP_ENABLE_DECODE
static uint32_t mu_texcomp_br_read(Mu_Texcomp_BitReader *r, uint32_t n)
{
    uint32_t   v = 0, s = 0;
    while (n) {
        uint32_t byte = r->pos >> 3, bit = r->pos & 7, take = 8u - bit;
        if (take > n) take = n;
        v |= ((uint32_t)((r->b[byte] >> bit) & ((1u << take) - 1u)) << s);
        r->pos += take;
        s += take;
        n -= take;
    }
    return v;
}

static uint32_t mu_texcomp_br_read_rev(Mu_Texcomp_BitReader *r, uint32_t n)
{
    uint32_t   v = mu_texcomp_br_read(r, n), o = 0, i;
    for (i = 0; i < n; i++) { o = (o << 1) | (v & 1u); v >>= 1; }
    return o;
}
#endif /* MU_TEXCOMP_ENABLE_DECODE bit reader */

static void mu_texcomp_bw_write(Mu_Texcomp_BitWriter *w, uint32_t v, uint32_t n)
{
    uint32_t   i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1u) w->b[w->pos >> 3] |= (uint8_t)(1u << (w->pos & 7));
        w->pos++;
    }
}

/* ---- BC6H mode table (MN = ARB Table.MF mode number) ---- */
typedef struct Mu_Texcomp_Bc6hMode { uint8_t mn, tr, nsub, epb, dr, dg, db; } Mu_Texcomp_Bc6hMode;

#if MU_TEXCOMP_ENABLE_DECODE
static const Mu_Texcomp_Bc6hMode MU_TEXCOMP_BC6H_MODES[14] = {
    { 0, 1, 2, 10,  5,  5,  5 }, { 1, 1, 2,  7,  6,  6,  6 },
    { 2, 1, 2, 11,  5,  4,  4 }, { 3, 0, 1, 10, 10, 10, 10 },
    { 6, 1, 2, 11,  4,  5,  4 }, { 7, 1, 1, 11,  9,  9,  9 },
    {10, 1, 2, 11,  4,  4,  5 }, {11, 1, 1, 12,  8,  8,  8 },
    {14, 1, 2,  9,  5,  5,  5 }, {15, 1, 1, 16,  4,  4,  4 },
    {18, 1, 2,  8,  6,  5,  5 }, {22, 1, 2,  8,  5,  6,  5 },
    {26, 1, 2,  8,  5,  5,  6 }, {30, 0, 2,  6,  6,  6,  6 }
};

static const Mu_Texcomp_Bc6hMode *mu_texcomp_bc6h_mode(uint32_t mn)
{
    uint32_t   i;
    for (i = 0; i < 14; i++) if (MU_TEXCOMP_BC6H_MODES[i].mn == mn) return &MU_TEXCOMP_BC6H_MODES[i];
    return 0;
}
#endif /* MU_TEXCOMP_ENABLE_DECODE bc6h mode table */

static int32_t mu_texcomp_bc6h_sext(uint32_t v, uint32_t n)
{
    uint32_t   m = 1u << (n - 1u);
    return (int32_t)((v ^ m) - m);
}

static int32_t mu_texcomp_bc6h_tinv(int32_t d, int32_t a0, uint32_t p, int is_signed)
{
    uint32_t   m = ((1u << p) - 1u);
    uint32_t   w = ((uint32_t)(d + a0)) & m;
    if (is_signed) return mu_texcomp_bc6h_sext(w, p);
    return (int32_t)w;
}

static int32_t mu_texcomp_bc6h_unq(int32_t c, uint32_t bits, int is_signed)
{
    if (!is_signed) {
        uint32_t u = (uint32_t)c;
        if (bits >= 15) return (int32_t)u;
        if (u == 0) return 0;
        if (u == ((1u << bits) - 1u)) return 0xFFFF;
        return (int32_t)(((u << 16) + 0x8000u) >> bits);
    } else {
        int32_t s = 0, x = c;
        if (bits >= 16) return x;
        if (x < 0) { s = 1; x = -x; }
        if (x == 0) return 0;
        if (x >= (int32_t)((1u << (bits - 1u)) - 1u)) return s ? -0x7FFF : 0x7FFF;
        x = (int32_t)(((uint32_t)((uint32_t)x << 15) + 0x4000u) >> (bits - 1u));
        return s ? -x : x;
    }
}

static uint16_t mu_texcomp_bc6h_finish(int32_t v, int is_signed)
{
    if (!is_signed) return (uint16_t)(((uint32_t)v * 31u) >> 6);
    {
        int32_t m = v < 0 ? -((int32_t)(((uint32_t)(-v) * 31u) >> 5)) : (int32_t)(((uint32_t)v * 31u) >> 5);
        if (m < 0) return (uint16_t)(0x8000u | (uint32_t)(-m));
        return (uint16_t)(uint32_t)m;
    }
}

/* BC6H two-region data: first 32 rows of the shared 2-subset table + fixups. */
static const uint8_t MU_TEXCOMP_BC6H_PART32[512] = {
    0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1, 0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1, 0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1, 0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1, 0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1, 0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1, 0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1,
    0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1, 0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1, 0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1, 0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,
    0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1, 0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0, 0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0, 0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0, 0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0, 0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0, 0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1,
    0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0, 0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0, 0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0, 0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0, 0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0, 0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0, 0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0, 0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0
};
static const uint8_t MU_TEXCOMP_BC6H_ANCHOR32[32] = { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2 };
static const uint32_t MU_TEXCOMP_BW4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
static const uint32_t MU_TEXCOMP_BW3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };

static float mu_texcomp_bc6h_clean(float f, int is_signed)
{
    uint32_t   x, ax;
    memcpy(&x, &f, 4);
    ax = x & 0x7FFFFFFFu;
    if ((ax & 0x7F800000u) == 0x7F800000u) {
        if (ax == 0x7F800000u) return (x & 0x80000000u) ? (is_signed ? -65504.0f : 0.0f) : 65504.0f;
        return 0.0f;
    }
    if (!is_signed && (x & 0x80000000u)) return 0.0f;
    return f;
}

/* exact 10-bit magnitude quantize: estimate + local search with the real */
/* unquantize/finish pipeline (signed magnitudes are 9-bit). */
static uint32_t mu_texcomp_bc6h_qmag(uint32_t m, int is_signed)
{
    uint32_t   maxc = is_signed ? 511u : 1023u;
    uint32_t   est, lo, hi, cc, best, besterr;
    if (m == 0) return 0;
    if (m > 0x7BFFu) m = 0x7BFFu;
    est = is_signed ? (m + 31u) / 62u : (m + 15u) / 31u;
    if (est > maxc) est = maxc;
    lo = est > 8u ? est - 8u : 0;
    hi = est + 8u > maxc ? maxc : est + 8u;
    best = lo;
    besterr = 0xFFFFFFFFu;
    for (cc = lo; cc <= hi; cc++) {
        int32_t u = mu_texcomp_bc6h_unq((int32_t)cc, 10, is_signed);
        uint32_t f = mu_texcomp_bc6h_finish(u, is_signed);
        uint32_t err = f > m ? f - m : m - f;
        if (err < besterr) { besterr = err; best = cc; }
    }
    return best;
}

static uint32_t mu_texcomp_bc6h_q10(float f, int is_signed)
{
    uint16_t   hh = mu_texcomp_float_to_half(f);
    if (!is_signed) return mu_texcomp_bc6h_qmag(hh, 0);
    {
        uint32_t s = hh & 0x8000u, cm = mu_texcomp_bc6h_qmag(hh & 0x7FFFu, 1);
        if (!s) return cm;
        return (1024u - cm) & 0x3FFu;
    }
}

static int32_t mu_texcomp_bc6h_pat_s(uint32_t pat, int is_signed, uint32_t bits)
{
    if (is_signed) return mu_texcomp_bc6h_sext(pat, bits);
    return (int32_t)pat;
}

/* quantized 10-bit endpoint pair -> float palette (single-subset pipeline). */
static void mu_texcomp_bc6h_pal10(float pal[16][3], uint32_t e0[3], uint32_t e1[3], int is_signed)
{
    uint32_t   c, i;
    for (c = 0; c < 3; c++) {
        int32_t v0 = mu_texcomp_bc6h_pat_s(e0[c], is_signed, 10);
        int32_t v1 = mu_texcomp_bc6h_pat_s(e1[c], is_signed, 10);
        int32_t u0 = mu_texcomp_bc6h_unq(v0, 10, is_signed);
        int32_t u1 = mu_texcomp_bc6h_unq(v1, 10, is_signed);
        for (i = 0; i < 16; i++) {
            int32_t ip = (u0 * (int32_t)(64u - MU_TEXCOMP_BW4[i]) + u1 * (int32_t)MU_TEXCOMP_BW4[i] + 32) >> 6;
            pal[i][c] = mu_texcomp_half_to_float(mu_texcomp_bc6h_finish(ip, is_signed));
        }
    }
}

static double mu_texcomp_bc6h_fit(float const src[16][3], float pal[16][3], uint32_t idx[16], uint32_t nidx)
{
    double     se = 0;
    uint32_t   k, q, c;
    for (k = 0; k < 16; k++) {
        uint32_t best = 0;
        double   bestd = 1e300;
        for (q = 0; q < nidx; q++) {
            double d = 0;
            for (c = 0; c < 3; c++) { double dd = (double)src[k][c] - (double)pal[q][c]; d += dd * dd; }
            if (d < bestd) { bestd = d; best = q; }
        }
        idx[k] = best;
        se += bestd;
    }
    return se;
}

static void mu_texcomp_bc6h_ls(float const src[16][3], uint32_t const idx[16], float E0[3], float E1[3],
    uint32_t const *W, uint32_t nidx, uint32_t part, int subset)
{
    uint32_t   c, k;
    for (c = 0; c < 3; c++) {
        double s_ww = 0, s_w = 0, s_wv = 0, s_v = 0, det, a0, a1;
        uint32_t cnt = 0;
        for (k = 0; k < 16; k++) {
            double w, v;
            if (subset >= 0 && (int)MU_TEXCOMP_BC6H_PART32[part * 16u + k] != subset) continue;
            w = (double)W[idx[k]] / 64.0;
            v = (double)src[k][c];
            s_ww += w * w; s_w += w; s_wv += w * v; s_v += v; cnt++;
        }
        if (!cnt) continue;
        det = s_ww * (double)cnt - s_w * s_w;
        if (det < 1e-6) continue;
        a1 = (s_wv * (double)cnt - s_w * s_v) / det;
        a0 = (s_v - a1 * s_w) / (double)cnt;
        E0[c] = (float)a0;
        E1[c] = (float)(a0 + a1);
    }
    (void)nidx;
}


typedef struct Mu_Texcomp_Bc6hCand {
    uint32_t   mn;
    uint32_t   part;
    uint32_t   e0[3];
    uint32_t   ex[3][3];
    uint32_t   idx[16];
    double     se;
} Mu_Texcomp_Bc6hCand;

/* candidate packers: MN3 single-subset, MN0 two-region. */
static void mu_texcomp_bc6h_pack3(uint8_t dst[16], uint32_t e0[3], uint32_t e1[3], uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 3, 5);
    mu_texcomp_bw_write(&w, e0[0], 10); mu_texcomp_bw_write(&w, e0[1], 10); mu_texcomp_bw_write(&w, e0[2], 10);
    mu_texcomp_bw_write(&w, e1[0], 10); mu_texcomp_bw_write(&w, e1[1], 10); mu_texcomp_bw_write(&w, e1[2], 10);
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], i == 0 ? 3u : 4u);
}

static void mu_texcomp_bc6h_pack0(uint8_t dst[16], uint32_t e0[3], uint32_t d1[3], uint32_t d2[3], uint32_t d3[3], uint32_t part, uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i, a1;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 0, 2);
    /* Table.F MN0 order: g2[4],b2[4],b3[4],r0,g0,b0,r1,g3[4],g2[3:0],g1,b3[0],g3[3:0],b1,b3[1],b2[3:0],r2,b3[2],r3,b3[3] */
    mu_texcomp_bw_write(&w, (d2[1] >> 4) & 1u, 1);
    mu_texcomp_bw_write(&w, (d2[2] >> 4) & 1u, 1);
    mu_texcomp_bw_write(&w, (d3[2] >> 4) & 1u, 1);
    mu_texcomp_bw_write(&w, e0[0], 10); mu_texcomp_bw_write(&w, e0[1], 10); mu_texcomp_bw_write(&w, e0[2], 10);
    mu_texcomp_bw_write(&w, d1[0], 5);
    mu_texcomp_bw_write(&w, (d3[1] >> 4) & 1u, 1);
    mu_texcomp_bw_write(&w, d2[1] & 15u, 4);
    mu_texcomp_bw_write(&w, d1[1], 5);
    mu_texcomp_bw_write(&w, d3[2] & 1u, 1);
    mu_texcomp_bw_write(&w, d3[1] & 15u, 4);
    mu_texcomp_bw_write(&w, d1[2], 5);
    mu_texcomp_bw_write(&w, (d3[2] >> 1) & 1u, 1);
    mu_texcomp_bw_write(&w, d2[2] & 15u, 4);
    mu_texcomp_bw_write(&w, d2[0], 5);
    mu_texcomp_bw_write(&w, (d3[2] >> 2) & 1u, 1);
    mu_texcomp_bw_write(&w, d3[0], 5);
    mu_texcomp_bw_write(&w, (d3[2] >> 3) & 1u, 1);
    mu_texcomp_bw_write(&w, part, 5);
    a1 = MU_TEXCOMP_BC6H_ANCHOR32[part];
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], (i == 0 || i == a1) ? 2u : 3u);
}
MU_TEXCOMP_API uint16_t mu_texcomp_pack565(uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t   rr = (r * 31u + 127u) / 255u;
    uint32_t   gg = (g * 63u + 127u) / 255u;
    uint32_t   bb = (b * 31u + 127u) / 255u;
    return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

static void mu_texcomp_unpack565(uint16_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    uint32_t   rr = (v >> 11) & 31u;
    uint32_t   gg = (v >> 5) & 63u;
    uint32_t   bb = v & 31u;
    *r = (rr * 255u + 15u) / 31u;
    *g = (gg * 255u + 31u) / 63u;
    *b = (bb * 255u + 15u) / 31u;
}

/* ---- BC4 helpers: alpha-style 8-value ramp ---- */

static void mu_texcomp_bc4_palette(uint8_t e0, uint8_t e1, uint8_t pal[8])
{
    pal[0] = e0;
    pal[1] = e1;
    if (e0 > e1) {
        pal[2] = (uint8_t)((6u * e0 + 1u * e1 + 3u) / 7u);
        pal[3] = (uint8_t)((5u * e0 + 2u * e1 + 3u) / 7u);
        pal[4] = (uint8_t)((4u * e0 + 3u * e1 + 3u) / 7u);
        pal[5] = (uint8_t)((3u * e0 + 4u * e1 + 3u) / 7u);
        pal[6] = (uint8_t)((2u * e0 + 5u * e1 + 3u) / 7u);
        pal[7] = (uint8_t)((1u * e0 + 6u * e1 + 3u) / 7u);
    } else {
        pal[2] = (uint8_t)((4u * e0 + 1u * e1 + 2u) / 5u);
        pal[3] = (uint8_t)((3u * e0 + 2u * e1 + 2u) / 5u);
        pal[4] = (uint8_t)((2u * e0 + 3u * e1 + 2u) / 5u);
        pal[5] = (uint8_t)((1u * e0 + 4u * e1 + 2u) / 5u);
        pal[6] = 0;
        pal[7] = 255;
    }
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc4(uint8_t const src[16], uint8_t dst[8])
{
    uint32_t   mn = 255;
    uint32_t   mx = 0;
    uint32_t   i;
    uint8_t    pal[8];
    uint64_t   bits = 0;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    for (i = 0; i < 16; i++) {
        uint32_t v = src[i];
        mn = v < mn ? v : mn;
        mx = v > mx ? v : mx;
    }
    /* 8-value mode needs e0 > e1; degenerate single-color uses e0==e1 trick. */
    if (mx == mn) {
        dst[0] = (uint8_t)mx; dst[1] = (uint8_t)mx;
        for (i = 2; i < 8; i++) dst[i] = 0;
        return MU_TEXCOMP_OK;
    }
    dst[0] = (uint8_t)mx;
    dst[1] = (uint8_t)mn;
    mu_texcomp_bc4_palette((uint8_t)mx, (uint8_t)mn, pal);
    for (i = 0; i < 16; i++) {
        uint32_t   best = 0;
        uint32_t   bestd = 0xFFFFFFFFu;
        uint32_t   k;
        for (k = 0; k < 8; k++) {
            uint32_t d = (uint32_t)pal[k] > src[i] ? (uint32_t)pal[k] - src[i] : src[i] - (uint32_t)pal[k];
            if (d < bestd) { bestd = d; best = k; }
        }
        bits |= (uint64_t)(best & 7u) << (3u * i);
    }
    for (i = 0; i < 6; i++) dst[2 + i] = (uint8_t)(bits >> (8u * i));
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc5(uint8_t const src_r[16], uint8_t const src_g[16], uint8_t dst[16])
{
    Mu_Texcomp_Error e;
    if (!src_r || !src_g || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    e = mu_texcomp_encode_block_bc4(src_r, dst);
    if (e != MU_TEXCOMP_OK) return e;
    return mu_texcomp_encode_block_bc4(src_g, dst + 8);
}

/* ---- BC1 fast encode: bbox endpoints in RGB, brute-force index select.
 * No partition search (BC1 has none), no least-squares iteration yet -
 * that is the ULTRA upgrade slot. preparation-free: 16 texels only. */

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc1(Mu_Texcomp_RgbaTile const *src, uint8_t dst[8], Mu_Texcomp_Quality quality)
{
    uint32_t   rmn = 255, rmx = 0, gmn = 255, gmx = 0, bmn = 255, bmx = 0;
    uint32_t   i;
    uint16_t   c0;
    uint16_t   c1;
    uint32_t   pal[4][3];
    uint32_t   idxbits = 0;
    (void)quality; /* FAST == ULTRA for now; ULTRA slot: refine endpoints by search */
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    for (i = 0; i < 16; i++) {
        uint32_t r = src->px[i][0], g = src->px[i][1], b = src->px[i][2];
        rmn = r < rmn ? r : rmn; rmx = r > rmx ? r : rmx;
        gmn = g < gmn ? g : gmn; gmx = g > gmx ? g : gmx;
        bmn = b < bmn ? b : bmn; bmx = b > bmx ? b : bmx;
    }
    c0 = mu_texcomp_pack565(rmx, gmx, bmx);
    c1 = mu_texcomp_pack565(rmn, gmn, bmn);
    /* force 4-color mode (c0 > c1); swap on tie to keep opaque path */
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (c0 == c1) c1 = (uint16_t)(c0 ^ (c0 & 0x1Fu ? 1u : 0x20u));
    mu_texcomp_unpack565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
    mu_texcomp_unpack565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
    for (i = 0; i < 3; i++) {
        pal[2][i] = (2u * pal[0][i] + 1u * pal[1][i] + 1u) / 3u;
        pal[3][i] = (1u * pal[0][i] + 2u * pal[1][i] + 1u) / 3u;
    }
    for (i = 0; i < 16; i++) {
        uint32_t   best = 0, bestd = 0xFFFFFFFFu, k;
        uint32_t   r = src->px[i][0], g = src->px[i][1], b = src->px[i][2];
        for (k = 0; k < 4; k++) {
            int32_t dr = (int32_t)r - (int32_t)pal[k][0];
            int32_t dg = (int32_t)g - (int32_t)pal[k][1];
            int32_t db = (int32_t)b - (int32_t)pal[k][2];
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < bestd) { bestd = d; best = k; }
        }
        idxbits |= best << (2u * i);
    }
    dst[0] = (uint8_t)(c0 & 0xFFu); dst[1] = (uint8_t)(c0 >> 8);
    dst[2] = (uint8_t)(c1 & 0xFFu); dst[3] = (uint8_t)(c1 >> 8);
    dst[4] = (uint8_t)(idxbits & 0xFFu); dst[5] = (uint8_t)((idxbits >> 8) & 0xFFu);
    dst[6] = (uint8_t)((idxbits >> 16) & 0xFFu); dst[7] = (uint8_t)((idxbits >> 24) & 0xFFu);
    return MU_TEXCOMP_OK;
}

/* BC2 = explicit 4-bit alpha + BC1 color. Legacy but trivial; BC3 supersedes
 * for smooth alpha, BC2 wins for sharp cutout alpha (no interpolation). */
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc2(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Quality quality)
{
    uint32_t   i;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    for (i = 0; i < 8; i++) {
        uint32_t a0 = src->px[i * 2][3] >> 4;
        uint32_t a1 = src->px[i * 2 + 1][3] >> 4;
        dst[i] = (uint8_t)(a0 | (a1 << 4));
    }
    return mu_texcomp_encode_block_bc1(src, dst + 8, quality);
}
/* BC3 = BC4 alpha + BC1 color. No new math, just composition. */
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc3(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Quality quality)
{
    uint8_t    alpha[16];
    uint32_t   i;
    Mu_Texcomp_Error e;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    for (i = 0; i < 16; i++) alpha[i] = src->px[i][3];
    e = mu_texcomp_encode_block_bc4(alpha, dst);
    if (e != MU_TEXCOMP_OK) return e;
    return mu_texcomp_encode_block_bc1(src, dst + 8, quality);
}
/* BC6H encode: float RGB in, half quantized internally. FAST = single-subset
 * MN3 (10-bit explicit endpoints, bbox + LS). ULTRA adds two-region MN0 search
 * over all 32 partitions with delta fitting. NaN->0, INF->max, neg->0 (UF). */
static double mu_texcomp_bc6h_meas0(float const src[16][3], uint32_t part, int32_t E[4][3], uint32_t idx[16], int is_signed)
{
    double     se = 0;
    uint32_t   k, qi, c;
    for (k = 0; k < 16; k++) {
        uint32_t s = MU_TEXCOMP_BC6H_PART32[part * 16u + k];
        uint32_t best = 0;
        double   bestd = 1e300;
        for (qi = 0; qi < 8; qi++) {
            double d = 0;
            for (c = 0; c < 3; c++) {
                int32_t u0 = mu_texcomp_bc6h_unq(E[s * 2][c], 10, is_signed);
                int32_t u1 = mu_texcomp_bc6h_unq(E[s * 2 + 1][c], 10, is_signed);
                int32_t ip = (u0 * (int32_t)(64u - MU_TEXCOMP_BW3[qi]) + u1 * (int32_t)MU_TEXCOMP_BW3[qi] + 32) >> 6;
                float pf = mu_texcomp_half_to_float(mu_texcomp_bc6h_finish(ip, is_signed));
                double dd = (double)src[k][c] - (double)pf;
                d += dd * dd;
            }
            if (d < bestd) { bestd = d; best = qi; }
        }
        idx[k] = best;
        se += bestd;
    }
    return se;
}

static void mu_texcomp_bc6h_deltas(uint32_t d1[3], uint32_t d2[3], uint32_t d3[3], int32_t base[3], uint32_t wB0[3], uint32_t wA1[3], uint32_t wB1[3], int is_signed)
{
    uint32_t   c;
    uint32_t const *wants[3] = { wB0, wA1, wB1 };
    uint32_t *dsts[3] = { d1, d2, d3 };
    for (c = 0; c < 3; c++) {
        int wi;
        for (wi = 0; wi < 3; wi++) {
            int32_t bestd = 0, besterr = 0x7FFFFFFF, sv;
            for (sv = 0; sv < 32; sv++) {
                int32_t sd = mu_texcomp_bc6h_sext((uint32_t)sv, 5);
                int32_t dec = mu_texcomp_bc6h_tinv(sd, base[c], 10, is_signed);
                int32_t udec = mu_texcomp_bc6h_unq(dec, 10, is_signed);
                int32_t uwant = mu_texcomp_bc6h_unq(mu_texcomp_bc6h_pat_s(wants[wi][c], is_signed, 10), 10, is_signed);
                int32_t err = udec > uwant ? udec - uwant : uwant - udec;
                if (err < besterr) { besterr = err; bestd = sv; }
            }
            dsts[wi][c] = (uint32_t)bestd;
        }
    }
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc6h(float const src_rgb[16][3], uint8_t dst[16], Mu_Texcomp_Quality quality, int is_signed)
{
    float      src[16][3];
    float      E0[3], E1[3];
    uint32_t   e0[3], e1[3];
    float      pal[16][3];
    uint32_t   idx[16];
    uint32_t   i, c, it, niter;
    Mu_Texcomp_Bc6hCand best;
    if (!src_rgb || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (is_signed != 0 && is_signed != 1) return MU_TEXCOMP_ERR_INVALID_INPUT;
    for (i = 0; i < 16; i++) for (c = 0; c < 3; c++) src[i][c] = mu_texcomp_bc6h_clean(src_rgb[i][c], is_signed);
    for (c = 0; c < 3; c++) {
        float mn = src[0][c], mx = src[0][c];
        for (i = 1; i < 16; i++) {
            if (src[i][c] < mn) mn = src[i][c];
            if (src[i][c] > mx) mx = src[i][c];
        }
        E0[c] = mn; E1[c] = mx;
    }
    niter = quality == MU_TEXCOMP_ULTRA ? 3u : 1u;
    {
        /* LS can walk to a worse local optimum through quantization; keep best. */
        double   loop_se = 1e300;
        uint32_t loop_e0[3] = { 0, 0, 0 }, loop_e1[3] = { 0, 0, 0 }, loop_idx[16] = { 0 };
        for (it = 0; it <= niter; it++) {
            double se;
            uint32_t six[16];
            for (c = 0; c < 3; c++) { e0[c] = mu_texcomp_bc6h_q10(E0[c], is_signed); e1[c] = mu_texcomp_bc6h_q10(E1[c], is_signed); }
            mu_texcomp_bc6h_pal10(pal, e0, e1, is_signed);
            se = mu_texcomp_bc6h_fit(src, pal, six, 16);
            if (se < loop_se) {
                loop_se = se;
                for (c = 0; c < 3; c++) { loop_e0[c] = e0[c]; loop_e1[c] = e1[c]; }
                for (i = 0; i < 16; i++) loop_idx[i] = six[i];
            }
            if (it >= niter) break;
            for (i = 0; i < 16; i++) idx[i] = six[i];
            mu_texcomp_bc6h_ls(src, idx, E0, E1, MU_TEXCOMP_BW4, 16, 0, -1);
        }
        for (c = 0; c < 3; c++) { e0[c] = loop_e0[c]; e1[c] = loop_e1[c]; }
        for (i = 0; i < 16; i++) idx[i] = loop_idx[i];
    }
    if (idx[0] > 7u) {
        for (c = 0; c < 3; c++) { float t = E0[c]; E0[c] = E1[c]; E1[c] = t; }
        for (i = 0; i < 16; i++) idx[i] = 15u - idx[i];
        for (c = 0; c < 3; c++) { e0[c] = mu_texcomp_bc6h_q10(E0[c], is_signed); e1[c] = mu_texcomp_bc6h_q10(E1[c], is_signed); }
    }
    best.mn = 3; best.part = 0; best.se = 1e300;
    {
        float p0[16][3];
        uint32_t ix[16];
        double se;
        mu_texcomp_bc6h_pal10(p0, e0, e1, is_signed);
        se = mu_texcomp_bc6h_fit(src, p0, ix, 16);
        best.se = se;
        for (c = 0; c < 3; c++) { best.e0[c] = e0[c]; best.ex[0][c] = e1[c]; best.ex[1][c] = 0; best.ex[2][c] = 0; }
        for (i = 0; i < 16; i++) best.idx[i] = ix[i];
    }
    if (quality == MU_TEXCOMP_ULTRA) {
        uint32_t   p;
        for (p = 0; p < 32; p++) {
            float sE0[2][3], sE1[2][3];
            uint32_t se0[2][3], se1[2][3];
            float spal[2][16][3];
            uint32_t s, k, cit;
            for (s = 0; s < 2; s++) {
                uint32_t first = 1;
                for (k = 0; k < 16; k++) {
                    if (MU_TEXCOMP_BC6H_PART32[p * 16u + k] != s) continue;
                    for (c = 0; c < 3; c++) {
                        if (first || src[k][c] < sE0[s][c]) sE0[s][c] = src[k][c];
                        if (first || src[k][c] > sE1[s][c]) sE1[s][c] = src[k][c];
                    }
                    first = 0;
                }
                if (first) { for (c = 0; c < 3; c++) { sE0[s][c] = 0.0f; sE1[s][c] = 0.0f; } }
                for (cit = 0; cit < 2; cit++) {
                    uint32_t six[16];
                    for (c = 0; c < 3; c++) { se0[s][c] = mu_texcomp_bc6h_q10(sE0[s][c], is_signed); se1[s][c] = mu_texcomp_bc6h_q10(sE1[s][c], is_signed); }
                    mu_texcomp_bc6h_pal10(spal[s], se0[s], se1[s], is_signed);
                    for (k = 0; k < 16; k++) {
                        uint32_t bq = 0;
                        double bd = 1e300, q;
                        if (MU_TEXCOMP_BC6H_PART32[p * 16u + k] != s) continue;
                        for (q = 0; q < 8; q++) {
                            double d = 0;
                            uint32_t qi = q * 2u;
                            for (c = 0; c < 3; c++) { double dd = (double)src[k][c] - (double)spal[s][qi][c]; d += dd * dd; }
                            if (d < bd) { bd = d; bq = q; }
                        }
                        six[k] = bq;
                    }
                    mu_texcomp_bc6h_ls(src, six, sE0[s], sE1[s], MU_TEXCOMP_BW4, 16, p, (int)s);
                }
            }
            {
                int32_t base[3];
                uint32_t d1[3], d2[3], d3[3];
                int32_t Ef[4][3];
                uint32_t fx[16];
                double fse;
                uint32_t a1 = MU_TEXCOMP_BC6H_ANCHOR32[p];
                for (c = 0; c < 3; c++) base[c] = mu_texcomp_bc6h_pat_s(se0[0][c], is_signed, 10);
                mu_texcomp_bc6h_deltas(d1, d2, d3, base, se1[0], se0[1], se1[1], is_signed);
                for (c = 0; c < 3; c++) {
                    Ef[0][c] = base[c];
                    Ef[1][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d1[c], 5), base[c], 10, is_signed);
                    Ef[2][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d2[c], 5), base[c], 10, is_signed);
                    Ef[3][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d3[c], 5), base[c], 10, is_signed);
                }
                fse = mu_texcomp_bc6h_meas0(src, p, Ef, fx, is_signed);
                if (fx[a1] > 3u) {
                    for (c = 0; c < 3; c++) { uint32_t t = d2[c]; d2[c] = d3[c]; d3[c] = t; }
                    for (c = 0; c < 3; c++) {
                        Ef[2][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d2[c], 5), base[c], 10, is_signed);
                        Ef[3][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d3[c], 5), base[c], 10, is_signed);
                    }
                    fse = mu_texcomp_bc6h_meas0(src, p, Ef, fx, is_signed);
                }
                if (fx[0] > 3u) {
                    for (c = 0; c < 3; c++) base[c] = Ef[1][c];
                    {
                        uint32_t nb0[3], nb1[3], na1[3];
                        for (c = 0; c < 3; c++) { nb0[c] = (uint32_t)(Ef[1][c] & 1023); nb1[c] = (uint32_t)(Ef[0][c] & 1023); na1[c] = (uint32_t)(Ef[2][c] & 1023); }
                        mu_texcomp_bc6h_deltas(d1, d2, d3, base, nb0, na1, nb1, is_signed);
                    }
                    for (c = 0; c < 3; c++) {
                        Ef[0][c] = base[c];
                        Ef[1][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d1[c], 5), base[c], 10, is_signed);
                        Ef[2][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d2[c], 5), base[c], 10, is_signed);
                        Ef[3][c] = mu_texcomp_bc6h_tinv(mu_texcomp_bc6h_sext(d3[c], 5), base[c], 10, is_signed);
                    }
                    fse = mu_texcomp_bc6h_meas0(src, p, Ef, fx, is_signed);
                }
                if (fse < best.se) {
                    best.mn = 0; best.part = p; best.se = fse;
                    for (c = 0; c < 3; c++) {
                        best.e0[c] = (uint32_t)(Ef[0][c] & 1023);
                        best.ex[0][c] = d1[c]; best.ex[1][c] = d2[c]; best.ex[2][c] = d3[c];
                    }
                    for (i = 0; i < 16; i++) best.idx[i] = fx[i];
                }
            }
        }
    }
    if (best.mn == 3) {
        mu_texcomp_bc6h_pack3(dst, best.e0, best.ex[0], best.idx);
    } else {
        uint32_t b0[3];
        for (c = 0; c < 3; c++) b0[c] = best.e0[c];
        mu_texcomp_bc6h_pack0(dst, b0, best.ex[0], best.ex[1], best.ex[2], best.part, best.idx);
    }
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_image_bc6h(Mu_Texcomp_ConstBytes src_f32rgb, uint32_t w, uint32_t h, uint32_t stride,
    Mu_Texcomp_Bytes dst_blocks, Mu_Texcomp_Quality quality, int is_signed)
{
    uint64_t   need;
    uint32_t   nbx, nby, by;
    if (!src_f32rgb.data || !dst_blocks.data) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (w == 0 || h == 0 || stride < w * 12u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (is_signed != 0 && is_signed != 1) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if ((uint32_t)quality > 1u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    need = mu_texcomp_image_block_count(w, h, MU_TEXCOMP_FMT_BC6H) * 16u;
    if (dst_blocks.size < need) return MU_TEXCOMP_ERR_DST_TOO_SMALL;
    if (src_f32rgb.size < (uint64_t)stride * (uint64_t)(h - 1) + (uint64_t)w * 12u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    nbx = (w + 3u) / 4u;
    nby = (h + 3u) / 4u;
    for (by = 0; by < nby; by++) {
        uint32_t bx;
        for (bx = 0; bx < nbx; bx++) {
            float tile[16][3];
            uint8_t *out = dst_blocks.data + ((uint64_t)by * nbx + bx) * 16u;
            uint32_t ty, tx;
            for (ty = 0; ty < 4; ty++) {
                uint32_t sy = by * 4u + ty, cy = sy < h ? sy : h - 1u;
                {
                    uint8_t const *brow = src_f32rgb.data + (uint64_t)cy * stride;
                    for (tx = 0; tx < 4; tx++) {
                        uint32_t sx = bx * 4u + tx, cx = sx < w ? sx : w - 1u;
                        float const *px = (float const *)(brow + (uint64_t)cx * 12u);
                        float tmp[3];
                        memcpy(tmp, px, 12);
                        tile[ty * 4u + tx][0] = tmp[0]; tile[ty * 4u + tx][1] = tmp[1]; tile[ty * 4u + tx][2] = tmp[2];
                    }
                }
            }
            mu_texcomp_encode_block_bc6h(tile, out, quality, is_signed);
        }
    }
    return MU_TEXCOMP_OK;
}
#if MU_TEXCOMP_ENABLE_DECODE

/* BC7 ultra search (tool-time; gated on the CPU decoder used for exact
 * candidate measurement). Tables below are exact copies of the spec tables. */
static const uint8_t MU_TEXCOMP_BC7E_P2[1024] = {
    0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1,
    0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1,
    0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1
};
static const uint8_t MU_TEXCOMP_BC7E_P3[1024] = {
    0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2, 0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1, 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0,
    0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2, 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0,
    0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2, 0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1,
    0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1,
    0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1, 0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2,
    0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0
};
static const uint8_t MU_TEXCOMP_BC7E_A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15
};
static const uint8_t MU_TEXCOMP_BC7E_A31[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3
};
static const uint8_t MU_TEXCOMP_BC7E_A32[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8
};
static const uint32_t MU_TEXCOMP_BC7E_W2[4] = { 0, 21, 43, 64 };
static const uint32_t MU_TEXCOMP_BC7E_W3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint32_t MU_TEXCOMP_BC7E_W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

/* dequantize N-bit stored value (+ optional p-bit) to 8 bits, per BC7 spec. */
static uint32_t mu_texcomp_bc7e_deq(uint32_t v, uint32_t p, uint32_t n, int has_p)
{
    uint32_t   t, w, sh;
    if (has_p) { t = (v << 1) | (p & 1u); w = n + 1u; }
    else { t = v; w = n; }
    sh = 8u - w;
    t <<= sh;
    return t | (t >> w);
}

/* quantize 8-bit value to N bits (no p-bit). */
static uint32_t mu_texcomp_bc7e_q(uint32_t e8, uint32_t n)
{
    uint32_t   q = (e8 * ((1u << n) - 1u) + 127u) / 255u;
    uint32_t   mx = (1u << n) - 1u;
    return q > mx ? mx : q;
}

/* Fit one subset: bbox + LS iters with p-bit search, best-tracked.
 * members[16]: 1 = texel in subset. Outputs quantized endpoint patterns
 * o_q0/o_q1 (N-bit per channel), pbits o_p0/o_p1, per-texel idx (valid at
 * member positions). Returns total SE over members.
 * has_pbit: 0 = none, 1 = one p-bit shared across the nch channels,
 * applied per endpoint (caller maps endpoints to pack slots). */
static double mu_texcomp_bc7e_fit_subset(uint8_t const px[16][4], uint8_t const *members,
    uint32_t nch, uint32_t epbits, uint32_t wbits, const uint32_t *W, int has_pbit,
    int p_joint, uint32_t iters, uint32_t o_q0[4], uint32_t o_q1[4], uint32_t *o_p0, uint32_t *o_p1,
    uint32_t idx[16])
{
    float      f0[4], f1[4];
    uint32_t   q0[4], q1[4], p0 = 0, p1 = 0;
    uint32_t   c, i, it, k;
    uint32_t   nidx = 1u << wbits;
    double     best_se = 1e300;
    uint32_t   best_q0[4] = { 0, 0, 0, 0 }, best_q1[4] = { 0, 0, 0, 0 };
    uint32_t   best_p0 = 0, best_p1 = 0, best_ix[16] = { 0 };
    uint32_t   cnt = 0;
    for (c = 0; c < nch; c++) { f0[c] = 255.0f; f1[c] = 0.0f; }
    for (i = 0; i < 16; i++) {
        if (!members[i]) continue;
        cnt++;
        for (c = 0; c < nch; c++) {
            float v = (float)px[i][c];
            if (v < f0[c]) f0[c] = v;
            if (v > f1[c]) f1[c] = v;
        }
    }
    if (!cnt) {
        for (c = 0; c < 4; c++) { o_q0[c] = 0; o_q1[c] = 0; }
        *o_p0 = 0; *o_p1 = 0;
        for (i = 0; i < 16; i++) idx[i] = 0;
        return 0.0;
    }
    for (it = 0; it <= iters; it++) {
        float pal[16][4];
        uint32_t six[16];
        double se;
        /* quantize endpoints (shared-pbit search per endpoint) */
        if (!has_pbit) {
            for (c = 0; c < nch; c++) {
                int32_t a0 = (int32_t)(f0[c] + 0.5f), a1 = (int32_t)(f1[c] + 0.5f);
                if (a0 < 0) { a0 = 0; } if (a0 > 255) { a0 = 255; }
                if (a1 < 0) { a1 = 0; } if (a1 > 255) { a1 = 255; }
                q0[c] = mu_texcomp_bc7e_q((uint32_t)a0, epbits);
                q1[c] = mu_texcomp_bc7e_q((uint32_t)a1, epbits);
            }
        } else if (p_joint) {
            uint32_t pp;
            double be = 1e300, bq0[4] = { 0, 0, 0, 0 }, bq1[4] = { 0, 0, 0, 0 };
            uint32_t bp = 0;
            uint32_t S = (1u << (epbits + 1u)) - 1u;
            for (pp = 0; pp < 2; pp++) {
                double d = 0;
                uint32_t t0[4], t1[4];
                for (c = 0; c < nch; c++) {
                    uint32_t T0 = (uint32_t)(f0[c] * (float)S / 255.0f + 0.5f);
                    uint32_t T1 = (uint32_t)(f1[c] * (float)S / 255.0f + 0.5f);
                    uint32_t mx;
                    if (T0 > S) { T0 = S; } if (T1 > S) { T1 = S; }
                    t0[c] = (T0 >= pp && ((T0 - pp) & 1u) == 0) ? (T0 - pp) >> 1 : (T0 >> 1);
                    t1[c] = (T1 >= pp && ((T1 - pp) & 1u) == 0) ? (T1 - pp) >> 1 : (T1 >> 1);
                    mx = (1u << epbits) - 1u;
                    if (t0[c] > mx) { t0[c] = mx; } if (t1[c] > mx) { t1[c] = mx; }
                    { double dd = (double)mu_texcomp_bc7e_deq(t0[c], pp, epbits, 1) - (double)f0[c]; d += dd * dd; }
                    { double dd = (double)mu_texcomp_bc7e_deq(t1[c], pp, epbits, 1) - (double)f1[c]; d += dd * dd; }
                }
                if (d < be) { be = d; bp = pp; for (c = 0; c < nch; c++) { bq0[c] = t0[c]; bq1[c] = t1[c]; } }
            }
            p0 = bp; p1 = bp;
            for (c = 0; c < nch; c++) { q0[c] = bq0[c]; q1[c] = bq1[c]; }
        } else {
            uint32_t pp;
            double be0 = 1e300, be1 = 1e300;
            for (pp = 0; pp < 2; pp++) {
                double d0 = 0, d1 = 0;
                uint32_t t0[4], t1[4];
                uint32_t S = (1u << (epbits + 1u)) - 1u;
                for (c = 0; c < nch; c++) {
                    uint32_t T0 = (uint32_t)(f0[c] * (float)S / 255.0f + 0.5f);
                    uint32_t T1 = (uint32_t)(f1[c] * (float)S / 255.0f + 0.5f);
                    if (T0 > S) { T0 = S; } if (T1 > S) { T1 = S; }
                    t0[c] = (T0 >= pp && ((T0 - pp) & 1u) == 0) ? (T0 - pp) >> 1 : (T0 >> 1);
                    t1[c] = (T1 >= pp && ((T1 - pp) & 1u) == 0) ? (T1 - pp) >> 1 : (T1 >> 1);
                    { uint32_t mx = (1u << epbits) - 1u; if (t0[c] > mx) t0[c] = mx; if (t1[c] > mx) t1[c] = mx; }
                    { double dd = (double)mu_texcomp_bc7e_deq(t0[c], pp, epbits, 1) - (double)f0[c]; d0 += dd * dd; }
                    { double dd = (double)mu_texcomp_bc7e_deq(t1[c], pp, epbits, 1) - (double)f1[c]; d1 += dd * dd; }
                }
                if (d0 < be0) { be0 = d0; p0 = pp; for (c = 0; c < nch; c++) q0[c] = t0[c]; }
                if (d1 < be1) { be1 = d1; p1 = pp; for (c = 0; c < nch; c++) q1[c] = t1[c]; }
            }
        }
        for (k = 0; k < nidx; k++) {
            for (c = 0; c < nch; c++) {
                uint32_t a = mu_texcomp_bc7e_deq(q0[c], p0, epbits, has_pbit);
                uint32_t b = mu_texcomp_bc7e_deq(q1[c], p1, epbits, has_pbit);
                pal[k][c] = (float)((a * (64u - W[k]) + b * W[k] + 32u) >> 6);
            }
            for (c = nch; c < 4; c++) pal[k][c] = 255.0f;
        }
        se = 0;
        for (i = 0; i < 16; i++) {
            uint32_t bq = 0, q;
            double bd = 1e300;
            if (!members[i]) { six[i] = 0; continue; }
            for (q = 0; q < nidx; q++) {
                double d = 0;
                for (c = 0; c < nch; c++) { double dd = (double)px[i][c] - (double)pal[q][c]; d += dd * dd; }
                if (d < bd) { bd = d; bq = q; }
            }
            six[i] = bq;
            se += bd;
        }
        if (se < best_se) {
            best_se = se;
            best_p0 = p0; best_p1 = p1;
            for (c = 0; c < 4; c++) { best_q0[c] = q0[c]; best_q1[c] = q1[c]; }
            for (i = 0; i < 16; i++) best_ix[i] = six[i];
        }
        if (it >= iters) break;
        for (i = 0; i < 16; i++) idx[i] = six[i];
        /* LS refine in float over members */
        for (c = 0; c < nch; c++) {
            double s_ww = 0, s_w = 0, s_wv = 0, s_v = 0, det, a0, a1;
            uint32_t n = 0;
            for (i = 0; i < 16; i++) {
                double w, v;
                if (!members[i]) continue;
                w = (double)W[idx[i]] / 64.0;
                v = (double)px[i][c];
                s_ww += w * w; s_w += w; s_wv += w * v; s_v += v; n++;
            }
            if (!n) continue;
            det = s_ww * (double)n - s_w * s_w;
            if (det < 1e-6) continue;
            a1 = (s_wv * (double)n - s_w * s_v) / det;
            a0 = (s_v - a1 * s_w) / (double)n;
            f0[c] = (float)a0;
            f1[c] = (float)(a0 + a1);
        }
    }
    for (c = 0; c < 4; c++) { o_q0[c] = best_q0[c]; o_q1[c] = best_q1[c]; }
    *o_p0 = best_p0; *o_p1 = best_p1;
    for (i = 0; i < 16; i++) idx[i] = best_ix[i];
    return best_se;
}

/* ---- BC7 ultra packers (field order mirrors the decode reader) ---- */
static void bc7e_pack1(uint8_t dst[16], uint32_t part, uint32_t e[4][3], uint32_t ps[2], uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i, c, k, a;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 2, 2); mu_texcomp_bw_write(&w, part, 6);
    for (c = 0; c < 3; c++) for (k = 0; k < 4; k++) mu_texcomp_bw_write(&w, e[k][c], 6);
    mu_texcomp_bw_write(&w, ps[0], 1); mu_texcomp_bw_write(&w, ps[1], 1);
    a = MU_TEXCOMP_BC7E_A2[part];
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], (i == 0 || i == a) ? 2u : 3u);
}

static void bc7e_pack3(uint8_t dst[16], uint32_t part, uint32_t e[4][3], uint32_t p4[4], uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i, c, k, a;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 8, 4); mu_texcomp_bw_write(&w, part, 6);
    for (c = 0; c < 3; c++) for (k = 0; k < 4; k++) mu_texcomp_bw_write(&w, e[k][c], 7);
    for (k = 0; k < 4; k++) mu_texcomp_bw_write(&w, p4[k], 1);
    a = MU_TEXCOMP_BC7E_A2[part];
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], (i == 0 || i == a) ? 1u : 2u);
}

static void bc7e_pack7(uint8_t dst[16], uint32_t part, uint32_t e[4][4], uint32_t p4[4], uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i, c, k, a;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 128, 8); mu_texcomp_bw_write(&w, part, 6);
    for (c = 0; c < 4; c++) for (k = 0; k < 4; k++) mu_texcomp_bw_write(&w, e[k][c], 5);
    for (k = 0; k < 4; k++) mu_texcomp_bw_write(&w, p4[k], 1);
    a = MU_TEXCOMP_BC7E_A2[part];
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], (i == 0 || i == a) ? 1u : 2u);
}

static void bc7e_pack2(uint8_t dst[16], uint32_t part, uint32_t e[6][3], uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i, c, k, a1, a2;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 4, 3); mu_texcomp_bw_write(&w, part, 6);
    for (c = 0; c < 3; c++) for (k = 0; k < 6; k++) mu_texcomp_bw_write(&w, e[k][c], 5);
    a1 = MU_TEXCOMP_BC7E_A31[part]; a2 = MU_TEXCOMP_BC7E_A32[part];
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], (i == 0 || i == a1 || i == a2) ? 1u : 2u);
}

static void bc7e_pack0(uint8_t dst[16], uint32_t part, uint32_t e[6][3], uint32_t p6[6], uint32_t idx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i, c, k, a1, a2;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 1, 1); mu_texcomp_bw_write(&w, part, 4);
    for (c = 0; c < 3; c++) for (k = 0; k < 6; k++) mu_texcomp_bw_write(&w, e[k][c], 4);
    for (k = 0; k < 6; k++) mu_texcomp_bw_write(&w, p6[k], 1);
    a1 = MU_TEXCOMP_BC7E_A31[part]; a2 = MU_TEXCOMP_BC7E_A32[part];
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, idx[i], (i == 0 || i == a1 || i == a2) ? 2u : 3u);
}

static void bc7e_pack5(uint8_t dst[16], uint32_t rot, uint32_t ce[2][3], uint32_t ae[2], uint32_t cidx[16], uint32_t aidx[16])
{
    Mu_Texcomp_BitWriter w;
    uint32_t   i;
    for (i = 0; i < 16; i++) dst[i] = 0;
    w.b = dst; w.pos = 0;
    mu_texcomp_bw_write(&w, 32, 6); mu_texcomp_bw_write(&w, rot, 2);
    mu_texcomp_bw_write(&w, ce[0][0], 7); mu_texcomp_bw_write(&w, ce[1][0], 7);
    mu_texcomp_bw_write(&w, ce[0][1], 7); mu_texcomp_bw_write(&w, ce[1][1], 7);
    mu_texcomp_bw_write(&w, ce[0][2], 7); mu_texcomp_bw_write(&w, ce[1][2], 7);
    mu_texcomp_bw_write(&w, ae[0], 8); mu_texcomp_bw_write(&w, ae[1], 8);
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, cidx[i], i == 0 ? 1u : 2u);
    for (i = 0; i < 16; i++) mu_texcomp_bw_write(&w, aidx[i], i == 0 ? 1u : 2u);
}

/* anchor fixup: anchor index must fit wb-1 bits; flip subset if violated. */
static void bc7e_afix(uint32_t q0[4], uint32_t q1[4], uint32_t *p0, uint32_t *p1,
    uint32_t idx[16], uint8_t const *members, uint32_t anchor, uint32_t nidx, uint32_t nch)
{
    uint32_t   c, i;
    if (idx[anchor] < nidx / 2u) return;
    for (c = 0; c < nch; c++) { uint32_t t = q0[c]; q0[c] = q1[c]; q1[c] = t; }
    { uint32_t t = *p0; *p0 = *p1; *p1 = t; }
    for (i = 0; i < 16; i++) if (members[i]) idx[i] = nidx - 1u - idx[i];
}

typedef struct Bc7eBest { uint8_t blk[16]; double se; } Bc7eBest;

static void bc7e_try(Mu_Texcomp_RgbaTile const *tile, uint8_t blk[16], Bc7eBest *b)
{
    Mu_Texcomp_RgbaTile dec;
    double   se = 0;
    uint32_t i, c;
    mu_texcomp_decode_block_bc7(blk, &dec);
    for (i = 0; i < 16; i++) for (c = 0; c < 4; c++) {
        double dd = (double)tile->px[i][c] - (double)dec.px[i][c];
        se += dd * dd;
    }
    if (se < b->se) { b->se = se; memcpy(b->blk, blk, 16); }
}

typedef struct Bc7eModeP { uint32_t nch, epbits, wbits, has_pbit, p_joint, iters; const uint32_t *W; } Bc7eModeP;

/* 2-subset search shared by modes 1/3/7 over the 64 P2 partitions. */
static void bc7e_ultra_sub2(Mu_Texcomp_RgbaTile const *tile, const Bc7eModeP *mp,
    void (*pack)(uint8_t*,uint32_t,uint32_t*,uint32_t*,uint32_t*), Bc7eBest *b)
{
    uint32_t   part, s, c, i;
    for (part = 0; part < 64; part++) {
        uint32_t   e[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
        uint32_t   pp[4] = { 0, 0, 0, 0 };
        uint32_t   ix[16] = { 0 };
        uint8_t    blk[16];
        uint8_t    m0[16], m1[16];
        uint32_t   a1 = MU_TEXCOMP_BC7E_A2[part];
        uint32_t   nidx = 1u << mp->wbits;
        for (s = 0; s < 2; s++) {
            uint8_t    mem[16];
            uint32_t   q0[4], q1[4], tix[16];
            uint32_t   p0 = 0, p1 = 0;
            for (i = 0; i < 16; i++) mem[i] = MU_TEXCOMP_BC7E_P2[part * 16u + i] == s ? 1u : 0u;
            mu_texcomp_bc7e_fit_subset(tile->px, mem, mp->nch, mp->epbits, mp->wbits, mp->W,
                (int)mp->has_pbit, (int)mp->p_joint, mp->iters, q0, q1, &p0, &p1, tix);
            for (c = 0; c < 4; c++) { e[s * 2u][c] = q0[c]; e[s * 2u + 1u][c] = q1[c]; }
            pp[s * 2u] = p0; pp[s * 2u + 1u] = p1;
            for (i = 0; i < 16; i++) if (mem[i]) ix[i] = tix[i];
        }
        for (i = 0; i < 16; i++) {
            m0[i] = MU_TEXCOMP_BC7E_P2[part * 16u + i] == 0 ? 1u : 0u;
            m1[i] = MU_TEXCOMP_BC7E_P2[part * 16u + i] == 1 ? 1u : 0u;
        }
        bc7e_afix(e[0], e[1], &pp[0], &pp[1], ix, m0, 0, nidx, mp->nch);
        bc7e_afix(e[2], e[3], &pp[2], &pp[3], ix, m1, a1, nidx, mp->nch);
        pack(blk, part, (uint32_t*)e, pp, ix);
        bc7e_try(tile, blk, b);
    }
}

/* 3-subset search shared by modes 2/0. */
static void bc7e_ultra_sub3(Mu_Texcomp_RgbaTile const *tile, const Bc7eModeP *mp, uint32_t npart,
    void (*pack)(uint8_t*,uint32_t,uint32_t*,uint32_t*,uint32_t*), Bc7eBest *b)
{
    uint32_t   part, s, c, i;
    for (part = 0; part < npart; part++) {
        uint32_t   e[6][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
                                { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
        uint32_t   pp[6] = { 0, 0, 0, 0, 0, 0 };
        uint32_t   ix[16] = { 0 };
        uint8_t    blk[16];
        uint32_t   anch[3];
        uint32_t   nidx = 1u << mp->wbits;
        for (s = 0; s < 3; s++) {
            uint8_t    mem[16];
            uint32_t   q0[4], q1[4], tix[16];
            uint32_t   p0 = 0, p1 = 0;
            for (i = 0; i < 16; i++) mem[i] = MU_TEXCOMP_BC7E_P3[part * 16u + i] == s ? 1u : 0u;
            mu_texcomp_bc7e_fit_subset(tile->px, mem, mp->nch, mp->epbits, mp->wbits, mp->W,
                (int)mp->has_pbit, (int)mp->p_joint, mp->iters, q0, q1, &p0, &p1, tix);
            for (c = 0; c < 4; c++) { e[s * 2u][c] = q0[c]; e[s * 2u + 1u][c] = q1[c]; }
            pp[s * 2u] = p0; pp[s * 2u + 1u] = p1;
            for (i = 0; i < 16; i++) if (mem[i]) ix[i] = tix[i];
        }
        anch[0] = 0; anch[1] = MU_TEXCOMP_BC7E_A31[part]; anch[2] = MU_TEXCOMP_BC7E_A32[part];
        for (s = 0; s < 3; s++) {
            uint8_t mem[16];
            for (i = 0; i < 16; i++) mem[i] = MU_TEXCOMP_BC7E_P3[part * 16u + i] == s ? 1u : 0u;
            bc7e_afix(e[s * 2u], e[s * 2u + 1u], &pp[s * 2u], &pp[s * 2u + 1u], ix, mem, anch[s], nidx, mp->nch);
        }
        pack(blk, part, (uint32_t*)e, pp, ix);
        bc7e_try(tile, blk, b);
    }
}

/* repack generic e/pp/ix into per-mode packer shapes. */
static void bc7e_pack1a(uint8_t *d, uint32_t p, uint32_t *e, uint32_t *pp, uint32_t *ix)
{
    uint32_t   ee[4][3];
    uint32_t   ps[2];
    uint32_t   k, c;
    for (k = 0; k < 4; k++) for (c = 0; c < 3; c++) ee[k][c] = e[k * 4u + c];
    ps[0] = pp[0]; ps[1] = pp[2];
    bc7e_pack1(d, p, ee, ps, ix);
}

static void bc7e_pack3a(uint8_t *d, uint32_t p, uint32_t *e, uint32_t *pp, uint32_t *ix)
{
    uint32_t   ee[4][3];
    uint32_t   k, c;
    for (k = 0; k < 4; k++) for (c = 0; c < 3; c++) ee[k][c] = e[k * 4u + c];
    bc7e_pack3(d, p, ee, pp, ix);
}

static void bc7e_pack7a(uint8_t *d, uint32_t p, uint32_t *e, uint32_t *pp, uint32_t *ix)
{
    uint32_t   ee[4][4];
    uint32_t   k, c;
    for (k = 0; k < 4; k++) for (c = 0; c < 4; c++) ee[k][c] = e[k * 4u + c];
    bc7e_pack7(d, p, ee, pp, ix);
}

static void bc7e_pack2a(uint8_t *d, uint32_t p, uint32_t *e, uint32_t *pp, uint32_t *ix)
{
    uint32_t   ee[6][3];
    uint32_t   k, c;
    (void)pp;
    for (k = 0; k < 6; k++) for (c = 0; c < 3; c++) ee[k][c] = e[k * 4u + c];
    bc7e_pack2(d, p, ee, ix);
}

static void bc7e_pack0a(uint8_t *d, uint32_t p, uint32_t *e, uint32_t *pp, uint32_t *ix)
{
    uint32_t   ee[6][3];
    uint32_t   k, c;
    for (k = 0; k < 6; k++) for (c = 0; c < 3; c++) ee[k][c] = e[k * 4u + c];
    bc7e_pack0(d, p, ee, pp, ix);
}

/* mode 5: rotation search, independent color + alpha fits. */
static void bc7e_ultra_m5(Mu_Texcomp_RgbaTile const *tile, Bc7eBest *b)
{
    uint32_t   rot, i;
    for (rot = 0; rot < 4; rot++) {
        uint8_t    wt[16][4];
        uint8_t    at[16][4];
        uint8_t    mem[16];
        uint32_t   cq0[4], cq1[4], aq0[4], aq1[4], cidx[16], aidx[16];
        uint32_t   cp0 = 0, cp1 = 0, ap0 = 0, ap1 = 0;
        uint32_t   ce[2][3], ae[2];
        uint8_t    blk[16];
        uint8_t    mall[16];
        for (i = 0; i < 16; i++) {
            wt[i][0] = tile->px[i][0]; wt[i][1] = tile->px[i][1]; wt[i][2] = tile->px[i][2]; wt[i][3] = tile->px[i][3];
            if (rot == 1) { uint8_t t = wt[i][0]; wt[i][0] = wt[i][3]; wt[i][3] = t; }
            else if (rot == 2) { uint8_t t = wt[i][1]; wt[i][1] = wt[i][3]; wt[i][3] = t; }
            else if (rot == 3) { uint8_t t = wt[i][2]; wt[i][2] = wt[i][3]; wt[i][3] = t; }
            at[i][0] = wt[i][3]; at[i][1] = 0; at[i][2] = 0; at[i][3] = 0;
            mem[i] = 1; mall[i] = 1;
        }
        mu_texcomp_bc7e_fit_subset(wt, mem, 3, 7, 2, MU_TEXCOMP_BC7E_W2, 0, 0, 2, cq0, cq1, &cp0, &cp1, cidx);
        mu_texcomp_bc7e_fit_subset(at, mem, 1, 8, 2, MU_TEXCOMP_BC7E_W2, 0, 0, 2, aq0, aq1, &ap0, &ap1, aidx);
        bc7e_afix(cq0, cq1, &cp0, &cp1, cidx, mall, 0, 4, 3);
        bc7e_afix(aq0, aq1, &ap0, &ap1, aidx, mall, 0, 4, 1);
        ce[0][0] = cq0[0]; ce[0][1] = cq0[1]; ce[0][2] = cq0[2];
        ce[1][0] = cq1[0]; ce[1][1] = cq1[1]; ce[1][2] = cq1[2];
        ae[0] = aq0[0]; ae[1] = aq1[0];
        bc7e_pack5(blk, rot, ce, ae, cidx, aidx);
        bc7e_try(tile, blk, b);
    }
}

static Mu_Texcomp_Error bc7e_ultra(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16])
{
    Bc7eBest   b;
    uint8_t    tmp[16];
    uint32_t   i, opaque = 1;
    Bc7eModeP  m1 = { 3, 6, 3, 1, 1, 2, MU_TEXCOMP_BC7E_W3 };
    Bc7eModeP  m3 = { 3, 7, 2, 1, 0, 2, MU_TEXCOMP_BC7E_W2 };
    Bc7eModeP  m7 = { 4, 5, 2, 1, 0, 2, MU_TEXCOMP_BC7E_W2 };
    Bc7eModeP  m2 = { 3, 5, 2, 0, 0, 2, MU_TEXCOMP_BC7E_W2 };
    Bc7eModeP  m0 = { 3, 4, 3, 1, 0, 2, MU_TEXCOMP_BC7E_W3 };
    b.se = 1e300;
    for (i = 0; i < 16; i++) if (src->px[i][3] != 255) { opaque = 0; break; }
    mu_texcomp_encode_block_bc7(src, tmp, MU_TEXCOMP_FAST);
    bc7e_try(src, tmp, &b);
    bc7e_ultra_m5(src, &b);
    bc7e_ultra_sub2(src, &m7, bc7e_pack7a, &b);
    if (opaque) {
        bc7e_ultra_sub2(src, &m1, bc7e_pack1a, &b);
        bc7e_ultra_sub2(src, &m3, bc7e_pack3a, &b);
        bc7e_ultra_sub3(src, &m2, 64, bc7e_pack2a, &b);
        bc7e_ultra_sub3(src, &m0, 16, bc7e_pack0a, &b);
    }
    for (i = 0; i < 16; i++) dst[i] = b.blk[i];
    return MU_TEXCOMP_OK;
}

#endif /* MU_TEXCOMP_ENABLE_DECODE bc7 ultra */

/* BC7 mode-6 fast encode: single-subset RGBA, 7-bit endpoints + shared p-bit,
 * 4-bit indices (texel 0 anchor: 3 bits). Bbox endpoints + 1 least-squares
 * pass + shared-pbit fit. ULTRA slot: multi-mode/partition search. */
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_bc7(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Quality quality)
{
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (quality == MU_TEXCOMP_ULTRA) {
#if MU_TEXCOMP_ENABLE_DECODE
        return bc7e_ultra(src, dst);
#else
        quality = MU_TEXCOMP_FAST;
#endif
    }
    {
    uint32_t   mn[4] = {255,255,255,255}, mx[4] = {0,0,0,0};
    uint32_t   i, c;
    uint32_t   E0[4], E1[4];
    uint32_t   idx[16];
    int        iters;
    static const uint32_t w4tab[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    for (i = 0; i < 16; i++) for (c = 0; c < 4; c++) {
        uint32_t v = src->px[i][c];
        mn[c] = v < mn[c] ? v : mn[c];
        mx[c] = v > mx[c] ? v : mx[c];
    }
    for (c = 0; c < 4; c++) { E0[c] = mn[c]; E1[c] = mx[c]; }
    iters = quality == MU_TEXCOMP_ULTRA ? 3 : 1;
    for (i = 0; i < (uint32_t)(iters + 1); i++) {
        /* select indices on current endpoints */
        uint32_t k;
        for (k = 0; k < 16; k++) {
            uint32_t best = 0, bestd = 0xFFFFFFFFu, q;
            uint32_t r = src->px[k][0], g = src->px[k][1], b = src->px[k][2], a = src->px[k][3];
            uint32_t pal[16][4];
            uint32_t t;
            for (t = 0; t < 16; t++) {
                uint32_t w = w4tab[t];
                pal[t][0] = (E0[0] * (64u - w) + E1[0] * w + 32u) >> 6;
                pal[t][1] = (E0[1] * (64u - w) + E1[1] * w + 32u) >> 6;
                pal[t][2] = (E0[2] * (64u - w) + E1[2] * w + 32u) >> 6;
                pal[t][3] = (E0[3] * (64u - w) + E1[3] * w + 32u) >> 6;
            }
            for (q = 0; q < 16; q++) {
                int32_t dr = (int32_t)r - (int32_t)pal[q][0];
                int32_t dg = (int32_t)g - (int32_t)pal[q][1];
                int32_t db = (int32_t)b - (int32_t)pal[q][2];
                int32_t da = (int32_t)a - (int32_t)pal[q][3];
                uint32_t d = (uint32_t)(dr*dr + dg*dg + db*db + da*da);
                if (d < bestd) { bestd = d; best = q; }
            }
            idx[k] = best;
        }
        if ((int)i >= iters) break;
        /* least-squares refine endpoints per channel given indices */
        for (c = 0; c < 4; c++) {
            double s_ww = 0, s_w = 0, s_wv = 0, s_v = 0;
            double n = 16.0, det, a0, a1;
            uint32_t k;
            for (k = 0; k < 16; k++) {
                double w = (double)w4tab[idx[k]] / 64.0;
                double v = (double)src->px[k][c];
                s_ww += w*w; s_w += w; s_wv += w*v; s_v += v;
            }
            det = (s_ww * n - s_w * s_w);
            if (det < 1e-6) continue;
            a1 = (s_wv * n - s_w * s_v) / det;   /* E1 weight */
            a0 = (s_v - a1 * s_w) / n;           /* E0 weight */
            if (a0 < 0.0) { a0 = 0.0; }
            if (a0 > 255.0) { a0 = 255.0; }
            if (a1 < -255.0) { a1 = -255.0; }
            if (a1 > 255.0) { a1 = 255.0; }
            {
                double   e1 = a0 + a1;
                if (e1 < 0.0) { e1 = 0.0; }
                if (e1 > 255.0) { e1 = 255.0; }
                E0[c] = (uint32_t)(a0 + 0.5);
                E1[c] = (uint32_t)(e1 + 0.5);
            }
        }
    }
    /* anchor fixup: texel 0 index must be < 8; flip endpoints if needed */
    if (idx[0] > 7) {
        for (c = 0; c < 4; c++) { uint32_t t = E0[c]; E0[c] = E1[c]; E1[c] = t; }
        for (i = 0; i < 16; i++) idx[i] = 15u - idx[i];
    }
    /* fit shared p-bits: try 4 combos, keep best total error */
    {
        uint32_t   bestp0 = 0, bestp1 = 0, bestd = 0xFFFFFFFFu;
        uint32_t   p0, p1;
        uint32_t   Q0[4], Q1[4];
        for (p0 = 0; p0 < 2; p0++) for (p1 = 0; p1 < 2; p1++) {
            uint32_t F0[4], F1[4], d = 0;
            for (c = 0; c < 4; c++) {
                uint32_t v0 = (E0[c] & ~1u) | p0, v1 = (E1[c] & ~1u) | p1;
                /* nearest representable within +-1 when clamping at ends */
                if ((E0[c] & 1u) != p0) v0 = E0[c] == 0 ? 1u : E0[c] == 255 ? 254u : E0[c] + (E0[c] < 128 ? 1 : (uint32_t)-1);
                if ((E1[c] & 1u) != p1) v1 = E1[c] == 0 ? 1u : E1[c] == 255 ? 254u : E1[c] + (E1[c] < 128 ? 1 : (uint32_t)-1);
                F0[c] = v0; F1[c] = v1;
                { uint32_t dd = F0[c] > E0[c] ? F0[c]-E0[c] : E0[c]-F0[c]; d += dd*dd; }
                { uint32_t dd = F1[c] > E1[c] ? F1[c]-E1[c] : E1[c]-F1[c]; d += dd*dd; }
            }
            if (d < bestd) {
                bestd = d; bestp0 = p0; bestp1 = p1;
                for (c = 0; c < 4; c++) { Q0[c] = F0[c] >> 1; Q1[c] = F1[c] >> 1; }
            }
        }
        /* pack: lo = mode(7) + endpoints(8x7) + p0(1); hi = p1(1) + indices */
        {
            uint64_t lo = 64u;
            uint64_t hi = bestp1 & 1u;
            lo |= (uint64_t)(Q0[0] & 0x7Fu) << 7;
            lo |= (uint64_t)(Q1[0] & 0x7Fu) << 14;
            lo |= (uint64_t)(Q0[1] & 0x7Fu) << 21;
            lo |= (uint64_t)(Q1[1] & 0x7Fu) << 28;
            lo |= (uint64_t)(Q0[2] & 0x7Fu) << 35;
            lo |= (uint64_t)(Q1[2] & 0x7Fu) << 42;
            lo |= (uint64_t)(Q0[3] & 0x7Fu) << 49;
            lo |= (uint64_t)(Q1[3] & 0x7Fu) << 56;
            lo |= (uint64_t)(bestp0 & 1u) << 63;
            /* indices: texel (x,y) -> nibble stream, texel0 = 3 bits */
            for (i = 0; i < 16; i++) {
                uint32_t x = i & 3u, y = (i >> 2) & 3u;
                uint32_t v = idx[y * 4u + x];
                if (i == 0) { hi |= (uint64_t)(v & 7u) << 1; }
                else {
                    uint32_t bit = 1u + 3u + (i - 1u) * 4u;
                    hi |= (uint64_t)(v & 15u) << bit;
                }
            }
            for (i = 0; i < 8; i++) dst[i] = (uint8_t)(lo >> (8u * i));
            for (i = 0; i < 8; i++) dst[8 + i] = (uint8_t)(hi >> (8u * i));
        }
    }
    return MU_TEXCOMP_OK;
    }
}
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_etc1(Mu_Texcomp_RgbaTile const *src, uint8_t dst[8], Mu_Texcomp_Quality quality)
{ (void)src; (void)dst; (void)quality; return MU_TEXCOMP_ERR_NOT_IMPLEMENTED; }
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_etc2(Mu_Texcomp_RgbaTile const *src, uint8_t dst[8], Mu_Texcomp_Quality quality)
{ (void)src; (void)dst; (void)quality; return MU_TEXCOMP_ERR_NOT_IMPLEMENTED; }
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_eac(uint8_t const src[16], uint8_t dst[8], int is_signed)
{ (void)src; (void)dst; (void)is_signed; return MU_TEXCOMP_ERR_NOT_IMPLEMENTED; }
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_block_astc(Mu_Texcomp_RgbaTile const *src, uint8_t dst[16], Mu_Texcomp_Format fmt, Mu_Texcomp_Quality quality)
{ (void)src; (void)dst; (void)fmt; (void)quality; return MU_TEXCOMP_ERR_NOT_IMPLEMENTED; }

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_encode_image(Mu_Texcomp_ConstBytes src_rgba8, uint32_t w, uint32_t h, uint32_t stride,
    Mu_Texcomp_Bytes dst_blocks, Mu_Texcomp_EncodeParams params)
{
    uint64_t   need;
    uint32_t   bw;
    uint32_t   bh;
    uint32_t   nbx;
    uint32_t   nby;
    uint32_t   by;
    if (!src_rgba8.data || !dst_blocks.data) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (w == 0 || h == 0 || stride < w * 4u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if ((uint32_t)params.format >= (uint32_t)MU_TEXCOMP_FMT_COUNT) return MU_TEXCOMP_ERR_INVALID_INPUT;
    need = mu_texcomp_image_bytes(w, h, params.format);
    if (dst_blocks.size < need) return MU_TEXCOMP_ERR_DST_TOO_SMALL;
    if (src_rgba8.size < (uint64_t)stride * (uint64_t)(h - 1) + (uint64_t)w * 4u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    mu_texcomp_block_dim(params.format, &bw, &bh);
    nbx = (w + bw - 1u) / bw;
    nby = (h + bh - 1u) / bh;
    MU_TEXCOMP_ASSERT(bw == 4 && bh == 4); /* v1 slice: only 4x4 formats execute */
    if (bw != 4 || bh != 4) return MU_TEXCOMP_ERR_NOT_IMPLEMENTED;

    for (by = 0; by < nby; by++) {
        uint32_t   bx;
        for (bx = 0; bx < nbx; bx++) {
            Mu_Texcomp_RgbaTile tile;
            uint8_t             *out;
            uint32_t            ty;
            uint64_t            block_idx = (uint64_t)by * nbx + bx;
            /* gather + clamp pad edge (matches DDS/KTX convention) */
            for (ty = 0; ty < 4; ty++) {
                uint32_t tx;
                uint32_t sy = by * 4u + ty;
                uint32_t cy = sy < h ? sy : h - 1u;
                uint8_t const *row = src_rgba8.data + (uint64_t)cy * stride;
                for (tx = 0; tx < 4; tx++) {
                    uint32_t sx = bx * 4u + tx;
                    uint32_t cx = sx < w ? sx : w - 1u;
                    tile.px[ty * 4u + tx][0] = row[cx * 4u + 0];
                    tile.px[ty * 4u + tx][1] = row[cx * 4u + 1];
                    tile.px[ty * 4u + tx][2] = row[cx * 4u + 2];
                    tile.px[ty * 4u + tx][3] = row[cx * 4u + 3];
                }
            }
            if (params.format != MU_TEXCOMP_FMT_BC1 && params.format != MU_TEXCOMP_FMT_BC2 && params.format != MU_TEXCOMP_FMT_BC3 && params.format != MU_TEXCOMP_FMT_BC4 && params.format != MU_TEXCOMP_FMT_BC5 && params.format != MU_TEXCOMP_FMT_BC7)
                return MU_TEXCOMP_ERR_NOT_IMPLEMENTED;
            out = dst_blocks.data + block_idx * mu_texcomp_block_bytes(params.format);
            if (params.format == MU_TEXCOMP_FMT_BC1) {
                mu_texcomp_encode_block_bc1(&tile, out, params.quality);
            } else if (params.format == MU_TEXCOMP_FMT_BC7) {
                mu_texcomp_encode_block_bc7(&tile, out, params.quality);
            } else if (params.format == MU_TEXCOMP_FMT_BC2) {
                mu_texcomp_encode_block_bc2(&tile, out, params.quality);
            } else if (params.format == MU_TEXCOMP_FMT_BC3) {
                mu_texcomp_encode_block_bc3(&tile, out, params.quality);
            } else if (params.format == MU_TEXCOMP_FMT_BC4) {
                uint8_t ch[16];
                uint32_t k;
                for (k = 0; k < 16; k++) ch[k] = tile.px[k][0];
                mu_texcomp_encode_block_bc4(ch, out);
            } else {
                uint8_t cr[16], cg[16];
                uint32_t k;
                for (k = 0; k < 16; k++) { cr[k] = tile.px[k][0]; cg[k] = tile.px[k][1]; }
                mu_texcomp_encode_block_bc5(cr, cg, out);
            }
        }
    }
    return MU_TEXCOMP_OK;
}

#if MU_TEXCOMP_ENABLE_DECODE

#include <math.h>

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc1(uint8_t const src[8], Mu_Texcomp_RgbaTile *dst)
{
    uint16_t   c0;
    uint16_t   c1;
    uint32_t   idx;
    uint32_t   pal[4][4];
    uint32_t   i;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    c0 = (uint16_t)(src[0] | (src[1] << 8));
    c1 = (uint16_t)(src[2] | (src[3] << 8));
    idx = (uint32_t)src[4] | ((uint32_t)src[5] << 8) | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
    mu_texcomp_unpack565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
    mu_texcomp_unpack565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
    pal[0][3] = 255; pal[1][3] = 255;
    if (c0 > c1) {
        for (i = 0; i < 3; i++) {
            pal[2][i] = (2u * pal[0][i] + pal[1][i] + 1u) / 3u;
            pal[3][i] = (pal[0][i] + 2u * pal[1][i] + 1u) / 3u;
        }
        pal[2][3] = 255; pal[3][3] = 255;
    } else {
        for (i = 0; i < 3; i++) {
            pal[2][i] = (pal[0][i] + pal[1][i] + 1u) / 2u;
            pal[3][i] = 0;
        }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    for (i = 0; i < 16; i++) {
        uint32_t s = (idx >> (2u * i)) & 3u;
        dst->px[i][0] = (uint8_t)pal[s][0];
        dst->px[i][1] = (uint8_t)pal[s][1];
        dst->px[i][2] = (uint8_t)pal[s][2];
        dst->px[i][3] = (uint8_t)pal[s][3];
    }
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc4(uint8_t const src[8], uint8_t dst[16])
{
    uint8_t    pal[8];
    uint64_t   bits;
    uint32_t   i;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    mu_texcomp_bc4_palette(src[0], src[1], pal);
    bits = 0;
    for (i = 0; i < 6; i++) bits |= (uint64_t)src[2 + i] << (8u * i);
    for (i = 0; i < 16; i++) dst[i] = pal[(bits >> (3u * i)) & 7u];
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc5(uint8_t const src[16], uint8_t dst_r[16], uint8_t dst_g[16])
{
    if (!src || !dst_r || !dst_g) return MU_TEXCOMP_ERR_INVALID_INPUT;
    mu_texcomp_decode_block_bc4(src, dst_r);
    mu_texcomp_decode_block_bc4(src + 8, dst_g);
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc3(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst)
{
    uint8_t    alpha[16];
    Mu_Texcomp_RgbaTile color;
    uint32_t   i;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    mu_texcomp_decode_block_bc4(src, alpha);
    mu_texcomp_decode_block_bc1(src + 8, &color);
    for (i = 0; i < 16; i++) {
        dst->px[i][0] = color.px[i][0];
        dst->px[i][1] = color.px[i][1];
        dst->px[i][2] = color.px[i][2];
        dst->px[i][3] = alpha[i];
    }
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc2(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst)
{
    Mu_Texcomp_RgbaTile color;
    uint32_t   i;
    if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
    mu_texcomp_decode_block_bc1(src + 8, &color);
    for (i = 0; i < 16; i++) {
        uint32_t a4 = (i & 1) ? (src[i >> 1] >> 4) : (src[i >> 1] & 15u);
        dst->px[i][0] = color.px[i][0];
        dst->px[i][1] = color.px[i][1];
        dst->px[i][2] = color.px[i][2];
        dst->px[i][3] = (uint8_t)(a4 * 17u);
    }
    return MU_TEXCOMP_OK;
}

/* ---- BC7 decode: tables (spec values, cf. bc7decomp reference) ---- */
static const uint8_t MU_TEXCOMP_BC7_PART2[1024] = {
0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,		0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,		0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,		0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1,		0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1,		0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1,		0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1,		0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1,		0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1,		0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1,		0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1,		0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1,		0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,		0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,		0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,
	0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1,		0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0,		0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0,		0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0,		0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0,		0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0,		0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0,		0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1,
	0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0,		0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0,		0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,		0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0,		0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0,		0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,		0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0,		0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0,
	0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,		0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1,		0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0,		0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0,		0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0,		0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0,		0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,		0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1,
	0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0,		0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0,		0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0,		0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0,		0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,		0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1,		0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1,		0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0,
	0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0,		0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,		0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,		0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0,		0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1,		0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1,		0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0,		0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0,
	0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1,		0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1,		0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1,		0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1,		0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1,		0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0,		0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0,		0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1
};
static const uint8_t MU_TEXCOMP_BC7_PART3[1024] = {
0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2,		0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1,		0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1,		0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1,		0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2,		0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2,		0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1,		0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1,
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,		0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,		0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2,		0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,		0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2,		0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2,		0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2,		0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0,
	0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2,		0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0,		0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2,		0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1,		0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2,		0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1,		0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2,		0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0,
	0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0,		0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2,		0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0,		0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1,		0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2,		0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2,		0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1,		0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1,
	0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2,		0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1,		0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2,		0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0,		0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0,		0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,		0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0,		0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1,
	0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1,		0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2,		0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1,		0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2,		0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1,		0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1,		0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1,		0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1,
	0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2,		0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1,		0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2,		0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2,		0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2,		0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2,		0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2,		0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2,
	0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2,		0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2,		0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2,		0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2,		0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1,		0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2,		0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2,		0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0,
};
static const uint8_t MU_TEXCOMP_BC7_ANCHOR2[64] = { 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2, 15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6, 6, 2, 6, 8,15,15, 2, 2, 15,15,15,15,15, 2, 2,15 };
static const uint8_t MU_TEXCOMP_BC7_ANCHOR3_1[64] = {
3, 3,15,15, 8, 3,15,15,		8, 8, 6, 6, 6, 5, 3, 3,		3, 3, 8,15, 3, 3, 6,10,		5, 8, 8, 6, 8, 5,15,15,		8,15, 3, 5, 6,10, 8,15,		15, 3,15, 5,15,15,15,15,		3,15, 5, 5, 5, 8, 5,10,		5,10, 8,13,15,12, 3, 3
};
static const uint8_t MU_TEXCOMP_BC7_ANCHOR3_2[64] = {
15, 8, 8, 3,15,15, 3, 8,		15,15,15,15,15,15,15, 8,		15, 8,15, 3,15, 8,15, 8,		3,15, 6,10,15,15,10, 8,		15, 3,15,10,10, 8, 9,10,		6,15, 8,15, 3, 6, 6, 8,		15, 3,15,15,15,15,15,15,		15,15,15,15, 3,15,15, 8
};
static const uint8_t MU_TEXCOMP_BC7_MODE_OF_BYTE[256] = {
8, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
	4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
};
static const uint32_t MU_TEXCOMP_BC7_W2[4] = { 0, 21, 43, 64 };
static const uint32_t MU_TEXCOMP_BC7_W3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint32_t MU_TEXCOMP_BC7_W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

static uint64_t mu_texcomp_ld64le(uint8_t const b[8])
{ uint64_t v = 0; uint32_t i; for (i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8u * i); return v; }
static uint64_t mu_texcomp_bc7_wzero(uint64_t bits, uint32_t bpi, uint32_t off)
{ uint64_t lom = ((((uint64_t)1) << (bpi * (off + 1u) - 1u)) - 1u); return ((bits & ~lom) << 1) | (bits & lom); }
static uint32_t mu_texcomp_bc7_deq1(uint32_t v, uint32_t p, uint32_t bits)
{ uint32_t t = bits + 1u; v = (v << 1) | (p & 1u); v <<= (8u - t); return v | (v >> t); }
static uint32_t mu_texcomp_bc7_deq0(uint32_t v, uint32_t bits)
{ v <<= (8u - bits); return v | (v >> bits); }
static uint32_t mu_texcomp_bc7_interp(uint32_t l, uint32_t h, uint32_t w, uint32_t bits)
{ const uint32_t *tw = bits == 2 ? MU_TEXCOMP_BC7_W2 : bits == 3 ? MU_TEXCOMP_BC7_W3 : MU_TEXCOMP_BC7_W4;
  return (l * (64u - tw[w]) + h * tw[w] + 32u) >> 6; }

static int mu_texcomp_bc7_mode02(uint32_t mode, uint64_t lo, uint64_t hi, Mu_Texcomp_RgbaTile *dst)
{
 uint32_t wb = mode == 0 ? 3u : 2u, wm = (1u << wb) - 1u, eb = mode == 0 ? 4u : 5u, em = (1u << eb) - 1u;
 uint32_t pb = mode == 0 ? 6u : 0u, part = (uint32_t)((lo >> (mode + 1u)) & (mode == 0 ? 15u : 63u));
 uint64_t ch[3]; uint32_t ep[6][4]; uint32_t pbits[6]; uint64_t wbits; uint32_t w[16]; uint32_t e, c, i;
 uint32_t a1 = MU_TEXCOMP_BC7_ANCHOR3_1[part], a2x = MU_TEXCOMP_BC7_ANCHOR3_2[part];
 uint32_t pal[3][8][4];
 if (mode == 0) { ch[0] = lo >> 5; ch[1] = lo >> 29; ch[2] = (lo >> 53) | (hi << 11); }
 else { ch[0] = lo >> 9; ch[1] = (lo >> 39) | (hi << 25); ch[2] = hi >> 5; }
 for (c = 0; c < 3; c++) for (e = 0; e < 6; e++) { ep[e][c] = (uint32_t)(ch[c] & em); ch[c] >>= eb; }
 for (e = 0; e < 6; e++) pbits[e] = 0;
 if (mode == 0) { uint32_t pbk = (uint32_t)((hi >> 13) & 0xFFu); for (e = 0; e < 6; e++) pbits[e] = (pbk >> e) & 1u; }
 wbits = hi >> (67u - 16u * wb);
 wbits = mu_texcomp_bc7_wzero(wbits, wb, 0);
 wbits = mu_texcomp_bc7_wzero(wbits, wb, a1 < a2x ? a1 : a2x);
 wbits = mu_texcomp_bc7_wzero(wbits, wb, a1 < a2x ? a2x : a1);
 for (i = 0; i < 16; i++) { w[i] = (uint32_t)(wbits & wm); wbits >>= wb; }
 for (e = 0; e < 6; e++) { ep[e][3] = 255; for (c = 0; c < 3; c++) ep[e][c] = pb ? mu_texcomp_bc7_deq1(ep[e][c], pbits[e], eb) : mu_texcomp_bc7_deq0(ep[e][c], eb); }
 for (e = 0; e < 3; e++) { uint32_t k; for (k = 0; k < (1u << wb); k++) { for (c = 0; c < 3; c++) pal[e][k][c] = (uint8_t)mu_texcomp_bc7_interp(ep[e*2][c], ep[e*2+1][c], k, wb); pal[e][k][3] = 255; } }
 for (i = 0; i < 16; i++) { uint32_t s = MU_TEXCOMP_BC7_PART3[part * 16u + i], q = w[i]; dst->px[i][0] = (uint8_t)pal[s][q][0]; dst->px[i][1] = (uint8_t)pal[s][q][1]; dst->px[i][2] = (uint8_t)pal[s][q][2]; dst->px[i][3] = 255; }
 return 1; }

static int mu_texcomp_bc7_mode137(uint32_t mode, uint64_t lo, uint64_t hi, Mu_Texcomp_RgbaTile *dst)
{
 uint32_t ncomp = mode == 7 ? 4u : 3u, wb = mode == 1 ? 3u : 2u, wm = (1u << wb) - 1u;
 uint32_t eb = mode == 7 ? 5u : mode == 1 ? 6u : 7u, em = (1u << eb) - 1u;
 uint32_t nbp = mode == 1 ? 2u : 4u, shared = mode == 1 ? 1u : 0u;
 uint32_t part = (uint32_t)((lo >> (mode + 1u)) & 63u);
 uint64_t ch[4] = {0,0,0,0}; uint64_t pread = 0, wbits = 0; uint32_t ep[4][4]; uint32_t pbits[4]; uint32_t w[16]; uint32_t e, c, i;
 uint32_t pal[2][8][4];
 ch[0] = lo >> (mode + 7u);
 if (mode == 1) { ch[1] = lo >> 32; ch[2] = (lo >> 56) | (hi << 8); pread = hi >> 16; wbits = hi >> 18; }
 else if (mode == 3) { ch[1] = (lo >> 38) | (hi << 26); ch[2] = hi >> 2; pread = hi >> 30; wbits = hi >> 34; }
 else if (mode == 7) { ch[1] = lo >> 34; ch[2] = (lo >> 54) | (hi << 10); ch[3] = hi >> 10; pread = hi >> 30; wbits = hi >> 34; }
 else return 0;
 for (c = 0; c < ncomp; c++) for (e = 0; e < 4; e++) { ep[e][c] = (uint32_t)(ch[c] & em); ch[c] >>= eb; }
 for (e = 0; e < 4; e++) pbits[e] = 0;
 for (e = 0; e < nbp; e++) pbits[e] = (uint32_t)((pread >> e) & 1u);
 wbits = mu_texcomp_bc7_wzero(wbits, wb, 0);
 wbits = mu_texcomp_bc7_wzero(wbits, wb, MU_TEXCOMP_BC7_ANCHOR2[part]);
 for (i = 0; i < 16; i++) { w[i] = (uint32_t)(wbits & wm); wbits >>= wb; }
 for (e = 0; e < 4; e++) for (c = 0; c < 4; c++) ep[e][c] = (mode != 7 && c == 3) ? 255 : mu_texcomp_bc7_deq1(ep[e][c], pbits[shared ? (e >> 1) : e], eb);
 for (e = 0; e < 2; e++) { uint32_t k; for (k = 0; k < (1u << wb); k++) { for (c = 0; c < ncomp; c++) pal[e][k][c] = (uint8_t)mu_texcomp_bc7_interp(ep[e*2][c], ep[e*2+1][c], k, wb); pal[e][k][3] = ncomp == 3 ? 255 : pal[e][k][3]; } }
 if (ncomp == 3) for (e = 0; e < 2; e++) { uint32_t k; for (k = 0; k < (1u << wb); k++) pal[e][k][3] = 255; }
 for (i = 0; i < 16; i++) { uint32_t s = MU_TEXCOMP_BC7_PART2[part * 16u + i], q = w[i]; dst->px[i][0] = (uint8_t)pal[s][q][0]; dst->px[i][1] = (uint8_t)pal[s][q][1]; dst->px[i][2] = (uint8_t)pal[s][q][2]; dst->px[i][3] = (uint8_t)pal[s][q][3]; }
 return 1; }

static int mu_texcomp_bc7_mode45(uint32_t mode, uint64_t lo, uint64_t hi, Mu_Texcomp_RgbaTile *dst)
{
 uint32_t wb = 2u, wm = 3u, awb = mode == 4 ? 3u : 2u, awm = (1u << awb) - 1u;
 uint32_t eb = mode == 4 ? 5u : 7u, em = (1u << eb) - 1u, aeb = mode == 4 ? 6u : 8u, aem = (1u << aeb) - 1u;
 uint32_t rot = (uint32_t)((lo >> (mode + 1u)) & 3u), im = mode == 4 ? (uint32_t)((lo >> 7) & 1u) : 0u;
 uint64_t cr = lo >> 8; uint32_t ep[2][4]; uint64_t wrgb, wa; uint32_t w[16], aw[16]; uint32_t e, c, i;
 uint32_t cpal[8][3]; uint32_t apal[8]; uint32_t bw0 = im ? awb : wb, bw1 = im ? wb : awb; uint32_t m0 = im ? awm : wm, m1 = im ? wm : awm;
 for (c = 0; c < 3; c++) for (e = 0; e < 2; e++) { ep[e][c] = (uint32_t)(cr & em); cr >>= eb; }
 if (mode == 4) { ep[0][3] = (uint32_t)(cr & aem); ep[1][3] = (uint32_t)((cr >> aeb) & aem); wrgb = (lo >> 50) | (hi << 14); wa = hi >> 17; }
 else if (mode == 5) { ep[0][3] = (uint32_t)(cr & aem); ep[1][3] = (uint32_t)(((lo >> 58) | (hi << 6)) & aem); wrgb = hi >> 2; wa = hi >> 33; }
 else return 0;
 wrgb = mu_texcomp_bc7_wzero(wrgb, wb, 0); wa = mu_texcomp_bc7_wzero(wa, awb, 0);
 if (im) { uint64_t t = wrgb; wrgb = wa; wa = t; }
 for (i = 0; i < 16; i++) { w[i] = (uint32_t)(wrgb & m0); wrgb >>= bw0; }
 for (i = 0; i < 16; i++) { aw[i] = (uint32_t)(wa & m1); wa >>= bw1; }
 for (e = 0; e < 2; e++) for (c = 0; c < 4; c++) ep[e][c] = mu_texcomp_bc7_deq0(ep[e][c], c == 3 ? aeb : eb);
 for (i = 0; i < (1u << bw0); i++) for (c = 0; c < 3; c++) cpal[i][c] = mu_texcomp_bc7_interp(ep[0][c], ep[1][c], i, bw0);
 for (i = 0; i < (1u << bw1); i++) apal[i] = mu_texcomp_bc7_interp(ep[0][3], ep[1][3], i, bw1);
 for (i = 0; i < 16; i++) { uint32_t r = cpal[w[i]][0], g = cpal[w[i]][1], b = cpal[w[i]][2], a = apal[aw[i]];
  if (rot == 1) { uint32_t t = a; a = r; r = t; } else if (rot == 2) { uint32_t t = a; a = g; g = t; } else if (rot == 3) { uint32_t t = a; a = b; b = t; }
  dst->px[i][0] = (uint8_t)r; dst->px[i][1] = (uint8_t)g; dst->px[i][2] = (uint8_t)b; dst->px[i][3] = (uint8_t)a; }
 return 1; }

static int mu_texcomp_bc7_mode6(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst)
{
 uint64_t lo = mu_texcomp_ld64le(src), hi = mu_texcomp_ld64le(src + 8); uint32_t i;
 uint32_t e0[4], e1[4]; uint32_t vals[16][4]; uint32_t idx[16]; uint64_t wb;
 if ((lo & 0x7Fu) != 64u) return 0;
 e0[0] = (uint32_t)(((lo >> 7) & 0x7Fu) << 1) | (uint32_t)((lo >> 63) & 1u);
 e1[0] = (uint32_t)(((lo >> 14) & 0x7Fu) << 1) | (uint32_t)((hi >> 0) & 1u);
 e0[1] = (uint32_t)(((lo >> 21) & 0x7Fu) << 1) | (uint32_t)((lo >> 63) & 1u);
 e1[1] = (uint32_t)(((lo >> 28) & 0x7Fu) << 1) | (uint32_t)((hi >> 0) & 1u);
 e0[2] = (uint32_t)(((lo >> 35) & 0x7Fu) << 1) | (uint32_t)((lo >> 63) & 1u);
 e1[2] = (uint32_t)(((lo >> 42) & 0x7Fu) << 1) | (uint32_t)((hi >> 0) & 1u);
 e0[3] = (uint32_t)(((lo >> 49) & 0x7Fu) << 1) | (uint32_t)((lo >> 63) & 1u);
 e1[3] = (uint32_t)(((lo >> 56) & 0x7Fu) << 1) | (uint32_t)((hi >> 0) & 1u);
 for (i = 0; i < 16; i++) { uint32_t c; for (c = 0; c < 4; c++) { uint32_t w = MU_TEXCOMP_BC7_W4[i]; vals[i][c] = (e0[c] * (64u - w) + e1[c] * w + 32u) >> 6; } }
 wb = hi >> 1;
 wb = mu_texcomp_bc7_wzero(wb, 4u, 0);
 for (i = 0; i < 16; i++) { idx[i] = (uint32_t)(wb & 15u); wb >>= 4; }
 for (i = 0; i < 16; i++) { dst->px[i][0] = (uint8_t)vals[idx[i]][0]; dst->px[i][1] = (uint8_t)vals[idx[i]][1]; dst->px[i][2] = (uint8_t)vals[idx[i]][2]; dst->px[i][3] = (uint8_t)vals[idx[i]][3]; }
 return 1; }

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc7(uint8_t const src[16], Mu_Texcomp_RgbaTile *dst)
{
 uint32_t mode; uint64_t lo, hi; int ok = 0;
 if (!src || !dst) return MU_TEXCOMP_ERR_INVALID_INPUT;
 mode = MU_TEXCOMP_BC7_MODE_OF_BYTE[src[0]];
 if (mode > 7) return MU_TEXCOMP_ERR_INVALID_INPUT;
 lo = mu_texcomp_ld64le(src); hi = mu_texcomp_ld64le(src + 8);
 if (mode == 0 || mode == 2) ok = mu_texcomp_bc7_mode02(mode, lo, hi, dst);
 else if (mode == 1 || mode == 3 || mode == 7) ok = mu_texcomp_bc7_mode137(mode, lo, hi, dst);
 else if (mode == 4 || mode == 5) ok = mu_texcomp_bc7_mode45(mode, lo, hi, dst);
 else if (mode == 6) ok = mu_texcomp_bc7_mode6(src, dst);
 return ok ? MU_TEXCOMP_OK : MU_TEXCOMP_ERR_INVALID_INPUT; }

/* BC6H decode: Table.F field order, transcribed from ARB BPTC spec + bcdec.
 * Two-region: endpoints(72/75) -> partition(5) -> indices(46, 3-bit, 2 anchors).
 * One-region: endpoints(60) -> indices(63, 4-bit, 1 anchor). Reserved -> zeros. */
#define MU_TEXCOMP_BR(n) mu_texcomp_br_read(&br, (n))
#define MU_TEXCOMP_BRR(n) mu_texcomp_br_read_rev(&br, (n))
MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_block_bc6h(uint8_t const src[16], float dst_rgb[16][3], int is_signed)
{
    Mu_Texcomp_BitReader br;
    uint32_t   b0, mn, part = 0, i, k, c, nEp;
    const Mu_Texcomp_Bc6hMode *md;
    int32_t    r[4] = { 0, 0, 0, 0 }, g[4] = { 0, 0, 0, 0 }, b[4] = { 0, 0, 0, 0 };
    int32_t    E[4][3];
    if (!src || !dst_rgb) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (is_signed != 0 && is_signed != 1) return MU_TEXCOMP_ERR_INVALID_INPUT;
    br.b = src; br.pos = 0;
    b0 = MU_TEXCOMP_BR(2);
    mn = b0 < 2u ? b0 : (b0 | (MU_TEXCOMP_BR(3) << 2));
    md = mu_texcomp_bc6h_mode(mn);
    if (!md) {
        for (i = 0; i < 16; i++) for (c = 0; c < 3; c++) dst_rgb[i][c] = 0.0f;
        return MU_TEXCOMP_OK;
    }
    switch (mn) {
    case 0:
        g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(5); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[2] |= (int32_t)MU_TEXCOMP_BR(4);
        g[1] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1); g[3] |= (int32_t)MU_TEXCOMP_BR(4);
        b[1] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(4);
        r[2] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 1:
        g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 5; g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 5;
        r[0] |= (int32_t)MU_TEXCOMP_BR(7); b[3] |= (int32_t)MU_TEXCOMP_BR(1); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1;
        b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[0] |= (int32_t)MU_TEXCOMP_BR(7); b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 5;
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[0] |= (int32_t)MU_TEXCOMP_BR(7);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 5; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[1] |= (int32_t)MU_TEXCOMP_BR(6); g[2] |= (int32_t)MU_TEXCOMP_BR(4); g[1] |= (int32_t)MU_TEXCOMP_BR(6);
        g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(6); b[2] |= (int32_t)MU_TEXCOMP_BR(4);
        r[2] |= (int32_t)MU_TEXCOMP_BR(6); r[3] |= (int32_t)MU_TEXCOMP_BR(6);
        break;
    case 2:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(5); r[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10; g[2] |= (int32_t)MU_TEXCOMP_BR(4);
        g[1] |= (int32_t)MU_TEXCOMP_BR(4); g[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10; b[3] |= (int32_t)MU_TEXCOMP_BR(1);
        g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(4); b[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(4); r[2] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 6:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(4); r[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10; g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        g[2] |= (int32_t)MU_TEXCOMP_BR(4); g[1] |= (int32_t)MU_TEXCOMP_BR(5); g[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(4); b[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(4); r[2] |= (int32_t)MU_TEXCOMP_BR(4);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(4);
        g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 10:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(4); r[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10; b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        g[2] |= (int32_t)MU_TEXCOMP_BR(4); g[1] |= (int32_t)MU_TEXCOMP_BR(4); g[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        b[3] |= (int32_t)MU_TEXCOMP_BR(1); g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(5);
        b[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10; b[2] |= (int32_t)MU_TEXCOMP_BR(4); r[2] |= (int32_t)MU_TEXCOMP_BR(4);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(4);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 14:
        r[0] |= (int32_t)MU_TEXCOMP_BR(9); b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[0] |= (int32_t)MU_TEXCOMP_BR(9);
        g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[0] |= (int32_t)MU_TEXCOMP_BR(9); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[1] |= (int32_t)MU_TEXCOMP_BR(5); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[2] |= (int32_t)MU_TEXCOMP_BR(4);
        g[1] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1); g[3] |= (int32_t)MU_TEXCOMP_BR(4);
        b[1] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(4);
        r[2] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 18:
        r[0] |= (int32_t)MU_TEXCOMP_BR(8); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        g[0] |= (int32_t)MU_TEXCOMP_BR(8); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        b[0] |= (int32_t)MU_TEXCOMP_BR(8); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[1] |= (int32_t)MU_TEXCOMP_BR(6); g[2] |= (int32_t)MU_TEXCOMP_BR(4); g[1] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1); g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(4); r[2] |= (int32_t)MU_TEXCOMP_BR(6);
        r[3] |= (int32_t)MU_TEXCOMP_BR(6);
        break;
    case 22:
        r[0] |= (int32_t)MU_TEXCOMP_BR(8); b[3] |= (int32_t)MU_TEXCOMP_BR(1); b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        g[0] |= (int32_t)MU_TEXCOMP_BR(8); g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 5; g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        b[0] |= (int32_t)MU_TEXCOMP_BR(8); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 5; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[1] |= (int32_t)MU_TEXCOMP_BR(5); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[2] |= (int32_t)MU_TEXCOMP_BR(4);
        g[1] |= (int32_t)MU_TEXCOMP_BR(6); g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(4); r[2] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 26:
        r[0] |= (int32_t)MU_TEXCOMP_BR(8); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        g[0] |= (int32_t)MU_TEXCOMP_BR(8); b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 5; g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        b[0] |= (int32_t)MU_TEXCOMP_BR(8); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 5; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[1] |= (int32_t)MU_TEXCOMP_BR(5); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[2] |= (int32_t)MU_TEXCOMP_BR(4);
        g[1] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1); g[3] |= (int32_t)MU_TEXCOMP_BR(4);
        b[1] |= (int32_t)MU_TEXCOMP_BR(6); b[2] |= (int32_t)MU_TEXCOMP_BR(4); r[2] |= (int32_t)MU_TEXCOMP_BR(5);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2; r[3] |= (int32_t)MU_TEXCOMP_BR(5); b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3;
        break;
    case 30:
        r[0] |= (int32_t)MU_TEXCOMP_BR(6); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[3] |= (int32_t)MU_TEXCOMP_BR(1);
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 1; b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; g[0] |= (int32_t)MU_TEXCOMP_BR(6);
        g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 5; b[2] |= (int32_t)MU_TEXCOMP_BR(1) << 5; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 2;
        g[2] |= (int32_t)MU_TEXCOMP_BR(1) << 4; b[0] |= (int32_t)MU_TEXCOMP_BR(6); g[3] |= (int32_t)MU_TEXCOMP_BR(1) << 5;
        b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 3; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 5; b[3] |= (int32_t)MU_TEXCOMP_BR(1) << 4;
        r[1] |= (int32_t)MU_TEXCOMP_BR(6); g[2] |= (int32_t)MU_TEXCOMP_BR(4); g[1] |= (int32_t)MU_TEXCOMP_BR(6);
        g[3] |= (int32_t)MU_TEXCOMP_BR(4); b[1] |= (int32_t)MU_TEXCOMP_BR(6); b[2] |= (int32_t)MU_TEXCOMP_BR(4);
        r[2] |= (int32_t)MU_TEXCOMP_BR(6); r[3] |= (int32_t)MU_TEXCOMP_BR(6);
        break;
    case 3:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(10); g[1] |= (int32_t)MU_TEXCOMP_BR(10); b[1] |= (int32_t)MU_TEXCOMP_BR(10);
        break;
    case 7:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(9); r[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        g[1] |= (int32_t)MU_TEXCOMP_BR(9); g[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        b[1] |= (int32_t)MU_TEXCOMP_BR(9); b[0] |= (int32_t)MU_TEXCOMP_BR(1) << 10;
        break;
    case 11:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(8); r[0] |= (int32_t)MU_TEXCOMP_BRR(2) << 10;
        g[1] |= (int32_t)MU_TEXCOMP_BR(8); g[0] |= (int32_t)MU_TEXCOMP_BRR(2) << 10;
        b[1] |= (int32_t)MU_TEXCOMP_BR(8); b[0] |= (int32_t)MU_TEXCOMP_BRR(2) << 10;
        break;
    case 15:
        r[0] |= (int32_t)MU_TEXCOMP_BR(10); g[0] |= (int32_t)MU_TEXCOMP_BR(10); b[0] |= (int32_t)MU_TEXCOMP_BR(10);
        r[1] |= (int32_t)MU_TEXCOMP_BR(4); r[0] |= (int32_t)MU_TEXCOMP_BRR(6) << 10;
        g[1] |= (int32_t)MU_TEXCOMP_BR(4); g[0] |= (int32_t)MU_TEXCOMP_BRR(6) << 10;
        b[1] |= (int32_t)MU_TEXCOMP_BR(4); b[0] |= (int32_t)MU_TEXCOMP_BRR(6) << 10;
        break;
    default:
        for (i = 0; i < 16; i++) for (c = 0; c < 3; c++) dst_rgb[i][c] = 0.0f;
        return MU_TEXCOMP_OK;
    }
    if (md->nsub == 2) part = MU_TEXCOMP_BR(5);
    nEp = md->nsub == 2 ? 4u : 2u;
    if (is_signed) {
        r[0] = mu_texcomp_bc6h_sext((uint32_t)r[0], md->epb);
        g[0] = mu_texcomp_bc6h_sext((uint32_t)g[0], md->epb);
        b[0] = mu_texcomp_bc6h_sext((uint32_t)b[0], md->epb);
    }
    if (md->tr || is_signed) {
        for (k = 1; k < nEp; k++) {
            r[k] = mu_texcomp_bc6h_sext((uint32_t)r[k], md->dr);
            g[k] = mu_texcomp_bc6h_sext((uint32_t)g[k], md->dg);
            b[k] = mu_texcomp_bc6h_sext((uint32_t)b[k], md->db);
        }
    }
    if (md->tr) {
        for (k = 1; k < nEp; k++) {
            r[k] = mu_texcomp_bc6h_tinv(r[k], r[0], md->epb, is_signed);
            g[k] = mu_texcomp_bc6h_tinv(g[k], g[0], md->epb, is_signed);
            b[k] = mu_texcomp_bc6h_tinv(b[k], b[0], md->epb, is_signed);
        }
    }
    for (k = 0; k < nEp; k++) {
        r[k] = mu_texcomp_bc6h_unq(r[k], md->epb, is_signed);
        g[k] = mu_texcomp_bc6h_unq(g[k], md->epb, is_signed);
        b[k] = mu_texcomp_bc6h_unq(b[k], md->epb, is_signed);
    }
    E[0][0] = r[0]; E[0][1] = g[0]; E[0][2] = b[0];
    E[1][0] = r[1]; E[1][1] = g[1]; E[1][2] = b[1];
    E[2][0] = r[2]; E[2][1] = g[2]; E[2][2] = b[2];
    E[3][0] = r[3]; E[3][1] = g[3]; E[3][2] = b[3];
    {
        const uint32_t *W = md->nsub == 1 ? MU_TEXCOMP_BC7_W4 : MU_TEXCOMP_BC7_W3;
        uint32_t   ibits = md->nsub == 1 ? 4u : 3u;
        for (i = 0; i < 16; i++) {
            uint32_t s, qi, ep, nb;
            int an;
            if (md->nsub == 1) { s = 0; an = i == 0; }
            else { s = MU_TEXCOMP_BC7_PART2[part * 16u + i]; an = (i == 0) || (i == MU_TEXCOMP_BC7_ANCHOR2[part]); }
            nb = an ? ibits - 1u : ibits;
            qi = MU_TEXCOMP_BR(nb);
            ep = s * 2u;
            for (c = 0; c < 3; c++) {
                int32_t ip = (E[ep][c] * (int32_t)(64u - W[qi]) + E[ep + 1][c] * (int32_t)W[qi] + 32) >> 6;
                dst_rgb[i][c] = mu_texcomp_half_to_float(mu_texcomp_bc6h_finish(ip, is_signed));
            }
        }
    }
    return MU_TEXCOMP_OK;
}
#undef MU_TEXCOMP_BR
#undef MU_TEXCOMP_BRR

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_image_bc6h(Mu_Texcomp_ConstBytes src_blocks, uint32_t w, uint32_t h,
    Mu_Texcomp_Bytes dst_f32rgb, uint32_t stride, int is_signed)
{
    uint64_t   need_src;
    uint32_t   nbx, nby, by;
    if (!src_blocks.data || !dst_f32rgb.data) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (w == 0 || h == 0 || stride < w * 12u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (is_signed != 0 && is_signed != 1) return MU_TEXCOMP_ERR_INVALID_INPUT;
    need_src = mu_texcomp_image_block_count(w, h, MU_TEXCOMP_FMT_BC6H) * 16u;
    if (src_blocks.size < need_src) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (dst_f32rgb.size < (uint64_t)stride * (uint64_t)(h - 1) + (uint64_t)w * 12u) return MU_TEXCOMP_ERR_DST_TOO_SMALL;
    nbx = (w + 3u) / 4u;
    nby = (h + 3u) / 4u;
    for (by = 0; by < nby; by++) {
        uint32_t bx;
        for (bx = 0; bx < nbx; bx++) {
            uint8_t const *blk = src_blocks.data + ((uint64_t)by * nbx + bx) * 16u;
            float tile[16][3];
            uint32_t ty, tx;
            Mu_Texcomp_Error e = mu_texcomp_decode_block_bc6h(blk, tile, is_signed);
            if (e != MU_TEXCOMP_OK) return e;
            for (ty = 0; ty < 4; ty++) {
                uint32_t sy = by * 4u + ty;
                if (sy >= h) continue;
                for (tx = 0; tx < 4; tx++) {
                    uint32_t sx = bx * 4u + tx;
                    float *d;
                    if (sx >= w) continue;
                    d = (float *)(dst_f32rgb.data + (uint64_t)sy * stride + (uint64_t)sx * 12u);
                    d[0] = tile[ty * 4u + tx][0]; d[1] = tile[ty * 4u + tx][1]; d[2] = tile[ty * 4u + tx][2];
                }
            }
        }
    }
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API Mu_Texcomp_Error mu_texcomp_decode_image(Mu_Texcomp_ConstBytes src_blocks, uint32_t w, uint32_t h,
    Mu_Texcomp_Bytes dst_rgba8, uint32_t stride, Mu_Texcomp_Format fmt)
{
    uint64_t   need_src;
    uint32_t   nbx;
    uint32_t   nby;
    uint32_t   by;
    if (!src_blocks.data || !dst_rgba8.data) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (w == 0 || h == 0 || stride < w * 4u) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (fmt != MU_TEXCOMP_FMT_BC1 && fmt != MU_TEXCOMP_FMT_BC2 && fmt != MU_TEXCOMP_FMT_BC3 && fmt != MU_TEXCOMP_FMT_BC4 && fmt != MU_TEXCOMP_FMT_BC5 && fmt != MU_TEXCOMP_FMT_BC7)
        return MU_TEXCOMP_ERR_NOT_IMPLEMENTED;
    need_src = mu_texcomp_image_bytes(w, h, fmt);
    if (src_blocks.size < need_src) return MU_TEXCOMP_ERR_INVALID_INPUT;
    if (dst_rgba8.size < (uint64_t)stride * (uint64_t)(h - 1) + (uint64_t)w * 4u) return MU_TEXCOMP_ERR_DST_TOO_SMALL;
    nbx = (w + 3u) / 4u;
    nby = (h + 3u) / 4u;
    for (by = 0; by < nby; by++) {
        uint32_t bx;
        for (bx = 0; bx < nbx; bx++) {
            uint8_t const *blk = src_blocks.data + ((uint64_t)by * nbx + bx) * mu_texcomp_block_bytes(fmt);
            uint32_t ty;
            if (fmt == MU_TEXCOMP_FMT_BC1 || fmt == MU_TEXCOMP_FMT_BC2 || fmt == MU_TEXCOMP_FMT_BC3 || fmt == MU_TEXCOMP_FMT_BC7) {
                Mu_Texcomp_RgbaTile tile;
                if (fmt == MU_TEXCOMP_FMT_BC1) mu_texcomp_decode_block_bc1(blk, &tile);
                else if (fmt == MU_TEXCOMP_FMT_BC2) mu_texcomp_decode_block_bc2(blk, &tile);
                else if (fmt == MU_TEXCOMP_FMT_BC3) mu_texcomp_decode_block_bc3(blk, &tile);
                else mu_texcomp_decode_block_bc7(blk, &tile);
                for (ty = 0; ty < 4; ty++) {
                    uint32_t sy = by * 4u + ty;
                    uint32_t tx;
                    if (sy >= h) continue;
                    for (tx = 0; tx < 4; tx++) {
                        uint32_t sx = bx * 4u + tx;
                        uint8_t *d;
                        if (sx >= w) continue;
                        d = dst_rgba8.data + (uint64_t)sy * stride + (uint64_t)sx * 4u;
                        d[0] = tile.px[ty * 4u + tx][0];
                        d[1] = tile.px[ty * 4u + tx][1];
                        d[2] = tile.px[ty * 4u + tx][2];
                        d[3] = tile.px[ty * 4u + tx][3];
                    }
                }
            } else if (fmt == MU_TEXCOMP_FMT_BC4) {
                uint8_t ch[16];
                mu_texcomp_decode_block_bc4(blk, ch);
                for (ty = 0; ty < 4; ty++) {
                    uint32_t sy = by * 4u + ty;
                    uint32_t tx;
                    if (sy >= h) continue;
                    for (tx = 0; tx < 4; tx++) {
                        uint32_t sx = bx * 4u + tx;
                        uint8_t *d;
                        if (sx >= w) continue;
                        d = dst_rgba8.data + (uint64_t)sy * stride + (uint64_t)sx * 4u;
                        d[0] = ch[ty * 4u + tx]; d[1] = ch[ty * 4u + tx];
                        d[2] = ch[ty * 4u + tx]; d[3] = 255;
                    }
                }
            } else {
                uint8_t cr[16], cg[16];
                mu_texcomp_decode_block_bc5(blk, cr, cg);
                for (ty = 0; ty < 4; ty++) {
                    uint32_t sy = by * 4u + ty;
                    uint32_t tx;
                    if (sy >= h) continue;
                    for (tx = 0; tx < 4; tx++) {
                        uint32_t sx = bx * 4u + tx;
                        uint8_t *d;
                        if (sx >= w) continue;
                        d = dst_rgba8.data + (uint64_t)sy * stride + (uint64_t)sx * 4u;
                        d[0] = cr[ty * 4u + tx]; d[1] = cg[ty * 4u + tx];
                        d[2] = 0; d[3] = 255;
                    }
                }
            }
        }
    }
    return MU_TEXCOMP_OK;
}

MU_TEXCOMP_API double mu_texcomp_psnr_rgba(Mu_Texcomp_ConstBytes a_rgba8, Mu_Texcomp_ConstBytes b_rgba8)
{
    /* needs -lm for log10; metrics live behind ENABLE_DECODE (tools only) */
    uint64_t   n;
    uint64_t   i;
    double     sse = 0.0;
    double     mse;
    if (!a_rgba8.data || !b_rgba8.data || a_rgba8.size != b_rgba8.size || a_rgba8.size == 0) return -1.0;
    n = a_rgba8.size;
    for (i = 0; i < n; i++) {
        double d = (double)a_rgba8.data[i] - (double)b_rgba8.data[i];
        sse += d * d;
    }
    mse = sse / (double)n;
    if (mse <= 0.0) return 99.0;
    return 10.0 * log10(65025.0 / mse);
}

#endif /* MU_TEXCOMP_ENABLE_DECODE */

#ifdef __cplusplus
}
#endif

#endif /* MU_TEXCOMP_H_IMPL */
#endif /* MU_TEXCOMP_IMPLEMENTATION */

/*
 * decode vs loading - the distinction that decides whether you need decode:
 *
 * LOADING (runtime, always):
 *   file/DDS/KTX --blocks--> RAM --vkCopyBufferToImage--> VRAM (still blocks)
 *   --sampler--> shader gets filtered texel. No CPU expansion. O(blocks) bandwidth.
 *   This is why BCn/ETC/ASTC exist: fixed bytes-per-block, block independence,
 *   random access by (x,y) without touching neighbours.
 *
 * DECODE (CPU expansion, tools only):
 *   blocks --unpack endpoints + lerp--> RGBA8/half --measure/preview/transcode-->
 *   O(pixels) bandwidth, ~4-16x bigger. Needed to compute PSNR, to preview without
 *   GPU, to transcode BC7->ASTC, or to fall back when hardware lacks the format.
 *
 * So: ship blocks, never ship decoded pixels. Keep MU_TEXCOMP_ENABLE_DECODE=1 while
 * tuning quality, =0 in shipping runtime builds.
 */
