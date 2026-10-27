#!/usr/bin/env python3
"""
Draws a cassette from its look (art/look.json and its images), as a player would:
the reference for docs/formats/cass-v1.md, "The look". Needs Pillow.

    python3 Tools/cass_look.py <unpacked cassette folder> <out.png> [--side A|B] [--fraction 0.35] [--angle 0]

--fraction is how much of the tape has played (0: all of it on the left hub, as at the start of the side);
--angle turns both hubs, in radians, clockwise.
"""
import argparse
import json
import math
import os
from PIL import Image, ImageDraw


def colour(text):
    text = text.lstrip("#")
    r, g, b = int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)
    a = int(text[6:8], 16) if len(text) >= 8 else 255
    return (r, g, b, a)


def mix(c0, c1, t):
    return tuple(round(c0[i] + (c1[i] - c0[i]) * t) for i in range(4))


def pack_diameter(look, f):
    lo = look["pack"]["minDiameter"]
    hi = max(lo, look["pack"]["fullDiameter"])
    f = min(1.0, max(0.0, f))
    return math.sqrt(lo * lo + (hi * hi - lo * lo) * f)


def tangent(c, r1, g, r2, side):
    """Where the tape leaves a pack (centre c, radius r1) on its outside, and meets the guide (g, r2)."""
    dx, dy = g[0] - c[0], g[1] - c[1]
    length = math.hypot(dx, dy)
    th = math.atan2(dy, dx)
    a = math.acos(max(-1.0, min(1.0, (r1 - r2) / length)))
    n1 = (math.cos(th + a), math.sin(th + a))
    n2 = (math.cos(th - a), math.sin(th - a))
    n = n1 if (side < 0) == (n1[0] < n2[0]) else n2
    return (c[0] + n[0] * r1, c[1] + n[1] * r1), (g[0] + n[0] * r2, g[1] + n[1] * r2)


def over(base, layer):
    base.alpha_composite(layer)


def draw(folder, side="A", fraction=0.35, angle=0.0):
    look = json.load(open(os.path.join(folder, "art", "look.json")))
    s = look["scale"]
    w, h = round(look["shell"]["width"] * s), round(look["shell"]["height"] * s)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))

    # 1. the shell behind the works
    over(out, Image.open(os.path.join(folder, look["layers"]["back"])).convert("RGBA"))

    # 2. the packs: the tape on each hub, a radial gradient with fine rings
    reels = [(r["x"], r["y"]) for r in look["reels"]]
    diameters = [pack_diameter(look, 1.0 - fraction), pack_diameter(look, fraction)]
    pack = look["pack"]
    inner, outer = colour(pack["inner"]), colour(pack["outer"])
    rings = pack["rings"]
    for (cx, cy), d in zip(reels, diameters):
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        pen = ImageDraw.Draw(layer)
        radius = d * 0.5 * s
        steps = max(1, int(radius))
        for k in range(steps, 0, -1):
            r = radius * k / steps
            pen.ellipse([cx * s - r, cy * s - r, cx * s + r, cy * s + r], fill=mix(inner, outer, r / radius))
        over(out, layer)
        ring_layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        pen = ImageDraw.Draw(ring_layer)
        rr = d * 0.5 - rings["inset"]
        while rr > rings["stop"]:
            pen.ellipse([(cx - rr) * s, (cy - rr) * s, (cx + rr) * s, (cy + rr) * s],
                        outline=colour(rings["colour"]), width=max(1, round(rings["width"] * s)))
            rr -= rings["spacing"]
        over(out, ring_layer)

    # 3. the tape: off the outside of each pack, round under its guide, straight between the guides
    tape = look["tape"]
    gl, gr = [(g["x"], g["y"]) for g in tape["guides"]]
    wrap = tape["wrapRadius"]
    tl = tangent(reels[0], diameters[0] * 0.5, gl, wrap, -1)
    tr = tangent(reels[1], diameters[1] * 0.5, gr, wrap, 1)
    points = [tl[0], tl[1]]
    a_left = math.atan2(tl[1][1] - gl[1], tl[1][0] - gl[0]) % (2 * math.pi)
    for k in range(1, 33):     # round the left guide to its bottom
        a = a_left + (math.pi / 2 - a_left) * k / 32
        points.append((gl[0] + wrap * math.cos(a), gl[1] + wrap * math.sin(a)))
    a_right = math.atan2(tr[1][1] - gr[1], tr[1][0] - gr[0])
    for k in range(0, 33):     # from the bottom of the right guide, round to where the tape leaves it
        a = math.pi / 2 + (a_right - math.pi / 2) * k / 32
        points.append((gr[0] + wrap * math.cos(a), gr[1] + wrap * math.sin(a)))
    points.append(tr[0])
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(layer).line([(x * s, y * s) for x, y in points], fill=colour(tape["colour"]),
                               width=max(1, round(tape["width"] * s)), joint="curve")
    over(out, layer)

    # 4. the hubs, turned
    hub_info = look["layers"]["hub"]
    hub = Image.open(os.path.join(folder, hub_info["image"])).convert("RGBA")
    turned = hub.rotate(-math.degrees(angle), resample=Image.BICUBIC)
    half = hub_info["size"] * 0.5
    for cx, cy in reels:
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        layer.paste(turned, (round((cx - half) * s), round((cy - half) * s)))
        over(out, layer)

    # 5. everything in front, the label included
    over(out, Image.open(os.path.join(folder, look["layers"]["front"][side])).convert("RGBA"))
    return out


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("folder")
    p.add_argument("out")
    p.add_argument("--side", default="A", choices=["A", "B"])
    p.add_argument("--fraction", type=float, default=0.35)
    p.add_argument("--angle", type=float, default=0.0)
    args = p.parse_args()
    draw(args.folder, args.side, args.fraction, args.angle).save(args.out)
