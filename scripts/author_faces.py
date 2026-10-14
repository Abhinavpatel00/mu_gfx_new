#!/usr/bin/env python3
"""Author cute slime face SDF textures (our own, replacing the SR rips).

Each face is a 256x256 RGBA PNG with standard-polarity SDF channels:
  R = eyes, A = mouth, B = eye whites, G = eye highlight ramp.
0.5 = shape edge, 1.0 = deep inside. The runtime resolves edges with
screen-space derivatives, so these stay crisp at any distance.

Design language: big oval dark eyes with an upper-left sheen ramp + hot
glint core, thick rounded mouth strokes, dark outline band added in-shader.
UV space is y-up, face centre (0.5, 0.5).
"""
import numpy as np
from PIL import Image
import os

N = 256
SPREAD = 0.035  # SDF dynamic range in UV units (~9px)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "slime_faces_ours")

ys, xs = np.mgrid[0:N, 0:N]
PX = (xs + 0.5) / N
PY = 1.0 - (ys + 0.5) / N  # y-up


def to_sdf(d):
    return np.clip(0.5 + 0.5 * (d / SPREAD), 0.0, 1.0)


def ellipse(cx, cy, rx, ry, rot=0.0):
    dx, dy = PX - cx, PY - cy
    if rot:
        c, s = np.cos(rot), np.sin(rot)
        dx, dy = c * dx + s * dy, -s * dx + c * dy
    k = np.sqrt((dx / rx) ** 2 + (dy / ry) ** 2)
    return (1.0 - k) * min(rx, ry)


def circle(cx, cy, r):
    return r - np.sqrt((PX - cx) ** 2 + (PY - cy) ** 2)


def seg_field(ax, ay, bx, by):
    abx, aby = bx - ax, by - ay
    t = np.clip(((PX - ax) * abx + (PY - ay) * aby) / (abx * abx + aby * aby + 1e-12), 0.0, 1.0)
    return np.sqrt((PX - (ax + abx * t)) ** 2 + (PY - (ay + aby * t)) ** 2)


def arc(cx, cy, r, a0, a1, n=40):
    """Distance to a circular arc (radians, y-up)."""
    ts = np.linspace(a0, a1, n)
    d = np.full_like(PX, 1e9)
    px, py = np.cos(ts) * r + cx, np.sin(ts) * r + cy
    for i in range(n - 1):
        d = np.minimum(d, seg_field(px[i], py[i], px[i + 1], py[i + 1]))
    return d


def stroke(dist, w):
    return w - dist


def smax(a, b, k=0.02):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return a + (b - a) * h + k * h * (1.0 - h)


EY = 0.57       # eye height
EX = 0.225      # eye half-spacing
ERX, ERY = 0.105, 0.135


def open_eyes(rot=0.0):
    l = ellipse(0.5 - EX, EY, ERX, ERY, rot)
    r = ellipse(0.5 + EX, EY, ERX, ERY, -rot)
    return smax(l, r)


def glints():
    l = circle(0.5 - EX - 0.036, EY + 0.055, 0.034)
    r = circle(0.5 + EX - 0.036, EY + 0.055, 0.034)
    return smax(l, r)


def smile():
    return stroke(arc(0.5, 0.44, 0.125, np.deg2rad(205), np.deg2rad(335)), 0.023)


def frown():
    return stroke(arc(0.5, 0.20, 0.115, np.deg2rad(25), np.deg2rad(155)), 0.023)


def lid_arc(cx, down=True):
    if down:  # u-shaped closed eye (sleepy)
        return stroke(arc(cx, 0.64, 0.088, np.deg2rad(205), np.deg2rad(335)), 0.020)
    return stroke(arc(cx, 0.50, 0.095, np.deg2rad(25), np.deg2rad(155)), 0.024)  # n-shaped (elated)


def save(name, eyes, mouth, whites=None, glint=None):
    r = to_sdf(eyes)
    a = to_sdf(mouth)
    b = to_sdf(whites) if whites is not None else np.zeros_like(r)
    g = to_sdf(glint) if glint is not None else np.zeros_like(r)
    img = np.stack([r, g, b, a], axis=-1)
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray((img * 255.0 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(OUT, name))
    print("wrote", name)


def main():
    # 0 happy: oval eyes + smile
    save("face_mood_happy.png", open_eyes(), smile(), glint=glints())
    # 1 blink: closed lids + smile
    save("face_mood_blink.png",
         smax(stroke(arc(0.5 - EX, 0.60, 0.085, np.deg2rad(200), np.deg2rad(340)), 0.020),
              stroke(arc(0.5 + EX, 0.60, 0.085, np.deg2rad(200), np.deg2rad(340)), 0.020)),
         smile())
    # 2 elated: ^^ eyes + open smile
    save("face_mood_elated.png", smax(lid_arc(0.5 - EX, False), lid_arc(0.5 + EX, False)),
         ellipse(0.5, 0.335, 0.095, 0.080), glint=None)
    # 3 scared: whites + small high pupils + o mouth
    wl = ellipse(0.5 - EX, EY, 0.105, 0.135)
    wr = ellipse(0.5 + EX, EY, 0.105, 0.135)
    pl = ellipse(0.5 - EX, EY + 0.035, 0.045, 0.060)
    pr = ellipse(0.5 + EX, EY + 0.035, 0.045, 0.060)
    save("face_mood_scared.png", smax(pl, pr), circle(0.5, 0.32, 0.048),
         whites=smax(wl, wr), glint=glints())
    # 4 hungry: eager eyes + big open mouth
    save("face_mood_hungry.png", open_eyes(), ellipse(0.5, 0.32, 0.078, 0.100), glint=glints())
    # 5 sad: pupils + sad brows + frown
    bl = 0.020 - seg_field(0.5 - 0.26, 0.70, 0.5 - 0.10, 0.79)
    br = 0.020 - seg_field(0.5 + 0.10, 0.79, 0.5 + 0.26, 0.70)
    save("face_mood_sad.png", smax(open_eyes(), smax(bl, br)), frown(), glint=glints())
    # 6 angry: tilted pupils + angry brows + flat mouth
    angry = smax(open_eyes(0.42), smax(0.024 - seg_field(0.5 - 0.27, 0.82, 0.5 - 0.07, 0.70),
                                       0.024 - seg_field(0.5 + 0.07, 0.70, 0.5 + 0.27, 0.82)))
    save("face_mood_angry.png", angry,
         0.020 - seg_field(0.5 - 0.10, 0.30, 0.5 + 0.10, 0.30), glint=None)
    # 7 sleepy: u-lids + tiny mouth
    save("face_mood_sleepy.png", smax(lid_arc(0.5 - EX, True), lid_arc(0.5 + EX, True)),
         circle(0.5, 0.32, 0.022))


if __name__ == "__main__":
    main()
