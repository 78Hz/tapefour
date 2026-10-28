#!/usr/bin/env python3
"""
Makes the app icons. Needs Pillow.
  TF-P1 Pocket: one supply reel, its hub head-on in a pack of tape.
  TF-4: a cassette in the deck, its two reels through the window, the teal of the deck's
  displays beneath.

    python3 Tools/make_icons.py <hub.png>

<hub.png> is a hub drawn by the TF-4 (the art/hub.png of a TYPE I cassette's look, from the
SD60 shell: its leader clamp in that shell's orange). It writes:

    Player/Icons/icon_ios.png        1024 px, square and opaque: iOS rounds the corners itself
    Player/Icons/icon_desktop.png    1024 px, a rounded square with the usual margin (macOS, Windows, Linux)
    Player/Android/res/...           Android's adaptive icon (background, foreground and the one-tone
                                     layer for themed icons) and the pre-Android 8 square and round icons
    Resources/Icons/tf4_desktop.png  the TF-4's, 1024 px, a rounded square with the usual margin

The art is drawn on Android's 108 dp adaptive canvas, everything that matters inside its 66 dp
safe circle; launchers show the middle 72 dp, and the other icons are that middle.
"""
import math
import os
import sys
from PIL import Image, ImageDraw

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = 1024 * 3 // 2          # the 108 dp canvas, drawn large and scaled down
C = S / 2
VISIBLE = S * 72 // 108    # what a launcher shows of it
BG_TOP, BG_BOTTOM = (52, 48, 58), (30, 28, 34)


def background():
    img = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(img)
    for y in range(S):
        t = y / (S - 1)
        d.line([(0, y), (S, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)) + (255,))
    return img


def foreground(hub):
    """The pack of tape and the hub in it; clear elsewhere."""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    R = S * 0.293          # the pack: just inside the safe circle (66 / 108 / 2 = 0.306)
    d = ImageDraw.Draw(img)
    for i in range(160):   # darker toward the hub, lit at the edge
        r = R * (1 - i / 160 * 0.55)
        t = i / 160
        d.ellipse([C - r, C - r, C + r, C + r],
                  fill=tuple(round(a + (b - a) * t) for a, b in zip((74, 50, 32), (34, 21, 13))) + (255,))
    d.ellipse([C - R, C - R, C + R, C + R], outline=(112, 80, 52, 255), width=round(R * 0.025))
    rings = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rings)
    rr = R * 0.93
    while rr > R * 0.5:    # the faint lines between the turns
        rd.ellipse([C - rr, C - rr, C + rr, C + rr], outline=(255, 225, 190, 10), width=3)
        rr -= R * 0.045
    img.alpha_composite(rings)
    # the hub: its disc is 208 of the image's 224 units, about 55% of the pack
    disc = R * 2 * 0.55
    box = round(disc * 224 / 208)
    h = hub.rotate(-35, resample=Image.BICUBIC).resize((box, box), Image.LANCZOS)
    img.alpha_composite(h, (round(C - box / 2), round(C - box / 2)))
    return img


def monochrome(hub):
    """Android's themed icon: one tone, from the hub's light plastic and its clamp (the launcher colours it)."""
    R = S * 0.293
    disc = R * 2 * 0.55
    box = round(disc * 224 / 208)
    h = hub.rotate(-35, resample=Image.BICUBIC).resize((box, box), Image.LANCZOS)
    lum = h.convert("L")
    alpha = h.getchannel("A")
    keep = lum.point(lambda v: 255 if v > 90 else 0)
    a = Image.new("L", (box, box), 0)
    a.paste(alpha, (0, 0), keep)
    u = box / 224          # the core open, as the spindle hole is: a solid disc reads as a button
    ImageDraw.Draw(a).ellipse([box / 2 - 50 * u, box / 2 - 50 * u, box / 2 + 50 * u, box / 2 + 50 * u], fill=0)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    layer = Image.new("RGBA", (box, box), (255, 255, 255, 255))
    layer.putalpha(a)
    img.alpha_composite(layer, (round(C - box / 2), round(C - box / 2)))
    # the pack's edge, so the reel still reads as a reel
    d = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(d).ellipse([C - R, C - R, C + R, C + R], outline=(255, 255, 255, 255), width=round(R * 0.06))
    img.alpha_composite(d)
    return img


def pack_disc(img, cx, cy, R):
    """A pack of tape: darker toward the hub, lit at the edge."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i in range(160):
        r = R * (1 - i / 160 * 0.55)
        t = i / 160
        d.ellipse([cx - r, cy - r, cx + r, cy + r],
                  fill=tuple(round(a + (b - a) * t) for a, b in zip((74, 50, 32), (34, 21, 13))) + (255,))
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=(112, 80, 52, 255), width=max(2, round(R * 0.025)))
    return layer


def tf4_face(hub):
    """The TF-4's: two reels through a cassette window (more tape on the left), a teal stripe beneath. 1024 px, full bleed."""
    n = 1024
    c = n / 2
    img = Image.new("RGBA", (n, n))
    d = ImageDraw.Draw(img)
    for y in range(n):
        t = y / (n - 1)
        d.line([(0, y), (n, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)) + (255,))
    window = [c - 400, c - 175, c + 400, c + 175]
    d.rounded_rectangle(window, radius=60, fill=(22, 18, 20, 255))
    reels = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    reels.alpha_composite(pack_disc(reels, c - 200, c, 190))
    reels.alpha_composite(pack_disc(reels, c + 200, c, 145))
    inside = Image.new("L", (n, n), 0)
    ImageDraw.Draw(inside).rounded_rectangle(window, radius=60, fill=255)
    reels.putalpha(Image.composite(reels.getchannel("A"), Image.new("L", (n, n), 0), inside))
    img.alpha_composite(reels)
    box = round(230 * 224 / 208)
    for x, angle in ((c - 200, 35), (c + 200, 95)):
        h = hub.rotate(-angle, resample=Image.BICUBIC).resize((box, box), Image.LANCZOS)
        img.alpha_composite(h, (round(x - box / 2), round(c - box / 2)))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(window, radius=60, outline=(120, 112, 130, 255), width=8)
    d.rounded_rectangle([c - 400, c + 215, c + 400, c + 250], radius=10, fill=(47, 165, 154, 255))
    return img


def desktop(face):
    """A rounded square in 824 of 1024 px, as macOS's own icons are."""
    out = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
    out.paste(face.resize((824, 824), Image.LANCZOS), (100, 100), mask(824, "rounded"))
    return out


def middle(img):
    o = (S - VISIBLE) // 2
    return img.crop((o, o, o + VISIBLE, o + VISIBLE))


def mask(size, shape):
    m = Image.new("L", (size * 4, size * 4), 0)
    d = ImageDraw.Draw(m)
    if shape == "circle":
        d.ellipse([0, 0, size * 4 - 1, size * 4 - 1], fill=255)
    else:
        d.rounded_rectangle([0, 0, size * 4 - 1, size * 4 - 1], radius=round(size * 4 * 0.2237), fill=255)
    return m.resize((size, size), Image.LANCZOS)


def save(img, path, size):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.resize((size, size), Image.LANCZOS).save(path, optimize=True)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    hub = Image.open(sys.argv[1]).convert("RGBA")
    bg, fg, mono = background(), foreground(hub), monochrome(hub)
    full = bg.copy()
    full.alpha_composite(fg)
    face = middle(full)

    icons = os.path.join(root, "Player", "Icons")
    os.makedirs(icons, exist_ok=True)
    face.convert("RGB").resize((1024, 1024), Image.LANCZOS).save(os.path.join(icons, "icon_ios.png"), optimize=True)
    desktop(face).save(os.path.join(icons, "icon_desktop.png"), optimize=True)

    tf4 = os.path.join(root, "Resources", "Icons")
    os.makedirs(tf4, exist_ok=True)
    desktop(tf4_face(hub)).save(os.path.join(tf4, "tf4_desktop.png"), optimize=True)

    res = os.path.join(root, "Player", "Android", "res")
    for name, scale in (("mdpi", 1), ("hdpi", 1.5), ("xhdpi", 2), ("xxhdpi", 3), ("xxxhdpi", 4)):
        folder = os.path.join(res, "mipmap-" + name)
        save(bg, os.path.join(folder, "ic_launcher_background.png"), round(108 * scale))
        save(fg, os.path.join(folder, "ic_launcher_foreground.png"), round(108 * scale))
        save(mono, os.path.join(folder, "ic_launcher_monochrome.png"), round(108 * scale))
        n = round(48 * scale)
        small = face.resize((n, n), Image.LANCZOS)
        for shape, file in (("rounded", "ic_launcher.png"), ("circle", "ic_launcher_round.png")):
            out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
            out.paste(small, (0, 0), mask(n, shape))
            out.save(os.path.join(folder, file), optimize=True)

    adaptive = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@mipmap/ic_launcher_background"/>\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
                '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome"/>\n'
                '</adaptive-icon>\n')
    folder = os.path.join(res, "mipmap-anydpi-v26")
    os.makedirs(folder, exist_ok=True)
    for file in ("ic_launcher.xml", "ic_launcher_round.xml"):
        with open(os.path.join(folder, file), "w") as f:
            f.write(adaptive)


if __name__ == "__main__":
    main()
