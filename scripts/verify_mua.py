#!/usr/bin/env python3
"""Decode a .mua back to triangles and sanity-check it.

Independent of the cooker and of the renderer on purpose: if this says the blob
is fine, the bug is not in the format or the cook pipeline.
"""
import struct, sys, math, json

def half(u):
    s = (u >> 15) & 1; e = (u >> 10) & 0x1F; m = u & 0x3FF
    if e == 0: v = m * 2.0**-24
    elif e == 31: v = float('inf')
    else: v = (1024 + m) * 2.0**(e - 25)
    return -v if s else v

def oct(o):
    x = ((o & 0xFFFF) ^ 0x8000) - 0x8000
    y = ((o >> 16) ^ 0x8000) - 0x8000
    ex = max(x / 32767.0, -1.0); ey = max(y / 32767.0, -1.0)
    z = 1.0 - abs(ex) - abs(ey)
    if z < 0.0:
        t = max(-z, 0.0)
        ex += (-t if ex >= 0 else t); ey += (-t if ey >= 0 else t)
    l = math.sqrt(ex*ex + ey*ey + z*z)
    return (ex/l, ey/l, z/l) if l else (0,1,0)

def load(path):
    d = open(path,'rb').read()
    magic, ver, nsec, secoff, total, flags = struct.unpack_from('<IIIIII', d, 0)
    assert magic == 0x2E41554D, hex(magic)
    S = {}
    for i in range(nsec):
        tag, rs, rc, off, size, ck, _, _ = struct.unpack_from('<IIIIIIII', d, secoff + i*32)
        S[tag] = (rs, rc, off, size)
    return d, S, flags

def tri(d, S, i0):
    return struct.unpack_from('<H', d, S[2][2] + i0*2)[0]

def main(path):
    d, S, flags = load(path)
    print(f"{path}  sections={sorted(S)}  flags=0x{flags:X}")
    meshes = []
    for m in range(S[3][1]):
        o = S[3][2] + m*32
        sph = struct.unpack_from('<4f', d, o)
        lf, lc, mat, batch = struct.unpack_from('<4I', d, o+16)
        meshes.append((sph, lf, lc, mat, batch))
    print(f"meshes={len(meshes)}  verts={S[1][1]}  idx={S[2][1]}  lods={S[4][1]}")

    worst_wind = 1.0
    for m, (sph, lf, lc, mat, batch) in enumerate(meshes):
        for k in range(lc):
            fi, ic, fv, err = struct.unpack_from('<IIIf', d, S[4][2] + (lf+k)*16)
            lo = [1e30]*3; hi = [-1e30]*3
            area = 0.0; outward = 0; inward = 0; nonfinite = 0
            maxidx = 0
            for t in range(0, ic, 3):
                a, b, c = (fi+t) and tri(d,S,fi+t), tri(d,S,fi+t+1), tri(d,S,fi+t+2)
                maxidx = max(maxidx, a, b, c)
                P = []
                for vi in (a,b,c):
                    r = S[1][2] + (fv+vi)*16
                    xy, z = struct.unpack_from('<II', d, r)
                    p = (half(xy & 0xFFFF), half(xy >> 16), half(z & 0xFFFF))
                    if not all(map(math.isfinite, p)): nonfinite += 1
                    for kk in range(3):
                        lo[kk] = min(lo[kk], p[kk]); hi[kk] = max(hi[kk], p[kk])
                    P.append(p)
                u = [P[1][i]-P[0][i] for i in range(3)]
                v = [P[2][i]-P[0][i] for i in range(3)]
                n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
                ar = 0.5*math.sqrt(sum(x*x for x in n))
                area += ar
                # does the geometric normal point away from the mesh centre?
                cen = tuple(sum(P[q][i] for q in range(3))/3 for i in range(3))
                mc  = tuple(sph[i]-cen[i] for i in range(3))
                d_ = sum(n[i]*mc[i] for i in range(3))
                if ar > 1e-12:
                    if d_ > 0: outward += 1
                    else: inward += 1
            frac_out = outward/max(1, outward+inward)
            worst_wind = min(worst_wind, frac_out if frac_out > 0.5 else 1-frac_out)
            if m < 2:
                print(f"  mesh{m} rung{k} tris={ic//3:5d} verts_used={maxidx+1:5d} "
                      f"area={area:8.3f} outward={frac_out*100:5.1f}% nonfinite={nonfinite} "
                      f"bbox=({lo[0]:.2f},{lo[1]:.2f},{lo[2]:.2f})..({hi[0]:.2f},{hi[1]:.2f},{hi[2]:.2f})")
    print(f"worst winding consistency across all meshes/rungs: {worst_wind*100:.1f}%")

if __name__ == '__main__':
    for p in sys.argv[1:] or ['assets/Knight.mua']:
        main(p); print()