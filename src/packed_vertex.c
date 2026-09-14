#include "../external/mu/mu.h"
/* ============================================================================
   Packed Vertex Formats
   ============================================================================ */

typedef struct PackedVertex
{
    uint16_t vx, vy, vz;
    uint16_t tp;
    uint32_t np;
    uint16_t tu, tv;
} PackedVertex; /* 16B */

typedef struct PackedSkinVertex
{
    uint16_t vx, vy, vz;
    uint16_t tp;
    uint32_t np;
    uint16_t tu, tv;
    uint16_t joints[4];
    uint16_t weights[4];
} PackedSkinVertex; /* 32B */

_Static_assert(sizeof(PackedVertex) == 16, "PackedVertex shader layout mismatch");
_Static_assert(sizeof(PackedSkinVertex) == 32, "PackedSkinVertex shader layout mismatch");

MU_INLINE PackedVertex mu_pack_vertex(float px, float py, float pz, float nx, float ny, float nz, float tx, float ty, float u, float v, int bitangent_sign)
{
    PackedVertex pv;

    pv.vx = mu_quantize_half(px);
    pv.vy = mu_quantize_half(py);
    pv.vz = mu_quantize_half(pz);

    int qnx = mu_quantize_snorm(nx, 10);
    int qny = mu_quantize_snorm(ny, 10);
    int qnz = mu_quantize_snorm(nz, 10);

    pv.np = ((uint32_t)(qnx & 1023)) | ((uint32_t)(qny & 1023) << 10) | ((uint32_t)(qnz & 1023) << 20)
            | ((uint32_t)(bitangent_sign & 3) << 30);

    int qtx = mu_quantize_snorm(tx, 8);
    int qty = mu_quantize_snorm(ty, 8);

    pv.tp = ((uint16_t)(qtx & 255) << 8) | ((uint16_t)(qty & 255));

    pv.tu = mu_quantize_half(u);
    pv.tv = mu_quantize_half(v);

    return pv;
}


MU_INLINE PackedSkinVertex mu_pack_skin_vertex(float    px,
                                               float    py,
                                               float    pz,
                                               float    nx,
                                               float    ny,
                                               float    nz,
                                               float    tx,
                                               float    ty,
                                               float    u,
                                               float    v,
                                               uint16_t j0,
                                               uint16_t j1,
                                               uint16_t j2,
                                               uint16_t j3,
                                               uint16_t w0,
                                               uint16_t w1,
                                               uint16_t w2,
                                               uint16_t w3,
                                               int      bitangent_sign)
{
    PackedSkinVertex pv;

    pv.vx = mu_quantize_half(px);
    pv.vy = mu_quantize_half(py);
    pv.vz = mu_quantize_half(pz);

    int qnx = mu_quantize_snorm(nx, 10);
    int qny = mu_quantize_snorm(ny, 10);
    int qnz = mu_quantize_snorm(nz, 10);

    pv.np = ((uint32_t)(qnx & 1023)) | ((uint32_t)(qny & 1023) << 10) | ((uint32_t)(qnz & 1023) << 20)
            | ((uint32_t)(bitangent_sign & 3) << 30);

    int qtx = mu_quantize_snorm(tx, 8);
    int qty = mu_quantize_snorm(ty, 8);

    pv.tp = ((uint16_t)(qtx & 255) << 8) | ((uint16_t)(qty & 255));
    pv.tu = mu_quantize_half(u);
    pv.tv = mu_quantize_half(v);

    pv.joints[0] = j0;
    pv.joints[1] = j1;
    pv.joints[2] = j2;
    pv.joints[3] = j3;

    pv.weights[0] = w0;
    pv.weights[1] = w1;
    pv.weights[2] = w2;
    pv.weights[3] = w3;

    return pv;
}

