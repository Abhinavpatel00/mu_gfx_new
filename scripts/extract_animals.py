#!/usr/bin/env python3
"""Slice the AI-generated animal sheet into individual RGBA sprites.

data/chatgpt/animals.png is a 12-row poster on a near-uniform dark background,
not a real grid: rows and columns are irregular and sprites have no cell pitch.
So we segment it instead:

  1. chroma-key the background (measured border colour, tight tolerance)
  2. connected components, dilated so shadows/legs join their animal
  3. bucket components into rows by y centre, sort each row by x
  4. crop each component from the un-dilated mask and key its own background

Rows are named in the visual order of the sheet. Output: data/farm/*.png plus
an optional contact sheet for eyeballing the result.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

# Row order as drawn in data/chatgpt/animals.png.
ROWS = [
    "cow", "chicken", "duck", "rabbit", "deer", "goat",
    "sheep", "pig", "dog", "cat", "wild", "pond",
]

# Row y-centres measured from the sheet (12 rows, irregular pitch).
ROW_CENTERS = [50, 136, 220, 300, 392, 484, 572, 644, 720, 796, 875, 955]

BG_TOL = 14       # border pixels sit at dist 0-3, sprite bodies start near 18
MIN_AREA = 350
MIN_W = 18
MIN_H = 18


def background_key(rgb):
    """Per-pixel distance from the measured sheet background."""
    border = np.concatenate([
        rgb[0:3].reshape(-1, 3), rgb[-3:].reshape(-1, 3),
        rgb[:, 0:3].reshape(-1, 3), rgb[:, -3:].reshape(-1, 3),
    ])
    bg = np.median(border, axis=0)
    dist = np.abs(rgb.astype(np.float32) - bg).max(axis=2)
    return dist <= BG_TOL, dist


def segment(is_bg):
    """Label the non-background into animals. Dilated so parts stay together."""
    fg = ~is_bg
    fg = ndimage.binary_opening(fg, np.ones((2, 2)))
    fg = ndimage.binary_closing(fg, np.ones((5, 5)))
    fg = ndimage.binary_dilation(fg, np.ones((5, 5)))
    lab, _ = ndimage.label(fg)

    comps = []
    for i, sl in enumerate(ndimage.find_objects(lab)):
        ys, xs = sl
        h, w = ys.stop - ys.start, xs.stop - xs.start
        area = int((lab[sl] == i + 1).sum())
        if area < MIN_AREA or w < MIN_W or h < MIN_H:
            continue
        comps.append({
            "x": xs.start, "y": ys.start, "w": w, "h": h, "area": area,
            "cx": xs.start + w / 2.0, "cy": ys.start + h / 2.0,
        })
    return comps


def bucket_rows(comps):
    edges = [0]
    for a, b in zip(ROW_CENTERS, ROW_CENTERS[1:]):
        edges.append((a + b) / 2.0)
    edges.append(10 ** 6)

    rows = [[] for _ in ROW_CENTERS]
    strays = 0
    for c in comps:
        for r in range(len(ROW_CENTERS)):
            if edges[r] <= c["cy"] < edges[r + 1]:
                rows[r].append(c)
                break
        else:
            strays += 1

    for r in rows:
        r.sort(key=lambda c: c["cx"])
    if strays:
        print(f"  dropped {strays} component(s) outside every row band")
    return rows


def crop_sprite(img, is_bg, comp):
    """Crop the component, key out its background, trim the empty border."""
    x0 = max(comp["x"] - 3, 0)
    y0 = max(comp["y"] - 3, 0)
    x1 = min(comp["x"] + comp["w"] + 3, img.shape[1])
    y1 = min(comp["y"] + comp["h"] + 3, img.shape[0])

    rgb = img[y0:y1, x0:x1]
    alpha = np.where(is_bg[y0:y1, x0:x1], 0, 255).astype(np.uint8)

    # 1px blur on the alpha only: the key is a hard per-pixel decision, so the
    # silhouette would otherwise show staircase edges once scaled up.
    alpha = ndimage.gaussian_filter(alpha.astype(np.float32), 0.7)
    alpha = np.clip(alpha, 0, 255).astype(np.uint8)

    rgba = np.dstack([rgb, alpha])
    opaque = alpha > 8
    if not opaque.any():
        return None
    ys, xs = np.where(opaque)
    return rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def contact_sheet(sprites, path):
    """Grid of everything we extracted, for visual verification."""
    if not sprites:
        return
    cell = 110
    pad = 8
    cols = 16
    entries = sorted(sprites, key=lambda s: (s["row"], s["idx"]))
    rows_n = (len(entries) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell + pad, rows_n * cell + pad),
                      (34, 44, 40, 255))
    for i, s in enumerate(entries):
        im = Image.fromarray(s["rgba"])
        scale = min((cell - 2 * pad) / im.width, (cell - 2 * pad) / im.height, 1.0)
        im = im.resize((max(int(im.width * scale), 1), max(int(im.height * scale), 1)),
                       Image.NEAREST)
        cx = (i % cols) * cell + cell // 2
        cy = (i // cols) * cell + cell // 2
        sheet.alpha_composite(im, (cx - im.width // 2, cy - im.height // 2))
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet", default="data/chatgpt/animals.png")
    ap.add_argument("--out", default="data/farm")
    ap.add_argument("--contact", default="")
    args = ap.parse_args()

    img = np.asarray(Image.open(args.sheet).convert("RGB"))
    is_bg, _ = background_key(img)
    print(f"background fraction: {is_bg.mean():.3f}")

    comps = segment(is_bg)
    print(f"components: {len(comps)}")
    rows = bucket_rows(comps)

    os.makedirs(args.out, exist_ok=True)
    for stale in os.listdir(args.out):
        if stale.endswith(".png"):
            os.remove(os.path.join(args.out, stale))

    sprites = []
    for r, row in enumerate(rows):
        name = ROWS[r] if r < len(ROWS) else f"row{r}"
        kept = 0
        for i, comp in enumerate(row):
            rgba = crop_sprite(img, is_bg, comp)
            if rgba is None:
                continue
            file = f"{name}_{kept:02d}.png"
            Image.fromarray(rgba).save(os.path.join(args.out, file))
            sprites.append({"row": r, "idx": kept, "rgba": rgba})
            kept += 1
        print(f"  {name:8s} {kept:3d} sprites")

    total = sum(s["idx"] + 1 for s in [])  # noqa: F841
    print(f"total written: {len(sprites)} -> {args.out}/")

    if args.contact:
        contact_sheet(sprites, args.contact)
        print(f"contact sheet -> {args.contact}")


if __name__ == "__main__":
    sys.exit(main())
