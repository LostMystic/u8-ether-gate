#!/usr/bin/env python3
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
"""Draws the program icon: our own "U8" monogram (not Origin's logo), gold on a dark red roundel
with a bronze rim and four small rivets.

    make_icon.py ENGINE_DIR

Writes ENGINE_DIR/data/icon/pagan_icon_<N>.png (N = 16 32 48 64 128 256), favicon.ico (16/32/48),
src/platform/icon_data.h (the 64 x 64 picture as RGBA bytes, set as the window icon by
platform.cpp) and shots/icon_preview.png (the sizes enlarged with square pixels).
The letters use Liberation Serif Bold (SIL Open Font License) when it is installed, else a font
PIL can find; they are drawn at 1024 x 1024 and scaled down per size (the 16 and 32 pixel
versions get a little sharpening so the strokes stay crisp).
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024
FONTS = ["/usr/share/fonts/TTF/liberation/LiberationSerif-Bold.ttf",
         "/usr/share/fonts/liberation/LiberationSerif-Bold.ttf",
         "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
         "/usr/share/fonts/TTF/dejavu/DejaVuSerif-Bold.ttf"]


def radial(size, inner, outer):
    img = Image.new("RGBA", (size, size))
    px = img.load()
    c = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            t = min(1.0, math.hypot(x - c * 0.85, y - c * 0.8) / (size * 0.62))
            px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3)) + (255,)
    return img


def master():
    big = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse((16, 16, S - 16, S - 16), fill=255)
    # Bronze rim, then the dark red field inside it.
    rim = radial(S, (238, 196, 110), (122, 72, 26))
    big.paste(rim, (0, 0), mask)
    field_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(field_mask).ellipse((96, 96, S - 96, S - 96), fill=255)
    big.paste(radial(S, (128, 22, 18), (40, 4, 4)), (0, 0), field_mask)
    d = ImageDraw.Draw(big)
    d.ellipse((96, 96, S - 96, S - 96), outline=(30, 8, 4, 255), width=14)
    d.ellipse((16, 16, S - 16, S - 16), outline=(46, 24, 8, 255), width=12)
    # Four rivets on the rim.
    for a in (45, 135, 225, 315):
        cx = S / 2 + math.cos(math.radians(a)) * (S / 2 - 56)
        cy = S / 2 + math.sin(math.radians(a)) * (S / 2 - 56)
        d.ellipse((cx - 22, cy - 22, cx + 22, cy + 22), fill=(255, 232, 160, 255), outline=(70, 40, 10, 255), width=6)
    # "U8": gold letters with a dark outline and a soft shadow.
    font = None
    for f in FONTS:
        if os.path.exists(f):
            font = ImageFont.truetype(f, 640)
            break
    if font is None:
        font = ImageFont.load_default(size=640)
    text = "U8"
    l, t, r, b = d.textbbox((0, 0), text, font=font, stroke_width=0)
    tw, th = r - l, b - t
    scale_x = 0.82  # a little condensed so both letters fit the field
    layer = Image.new("RGBA", (tw + 120, th + 120), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.text((60 - l, 60 - t), text, font=font, fill=(255, 255, 255, 255), stroke_width=34, stroke_fill=(0, 0, 0, 255))
    w2 = int(layer.width * scale_x)
    ox, oy = (S - w2) // 2, (S - layer.height) // 2 + 8
    shape = layer.resize((w2, layer.height), Image.LANCZOS)
    alpha = shape.split()[3]
    # Shadow.
    shadow = Image.new("RGBA", shape.size, (10, 0, 0, 200))
    shadow.putalpha(alpha.filter(ImageFilter.GaussianBlur(18)).point(lambda v: v * 0.8))
    big.alpha_composite(shadow, (ox + 16, oy + 22))
    # Outline (dark), then the gold fill with a top-to-bottom gradient.
    lum = shape.convert("L")
    outline = Image.new("RGBA", shape.size, (34, 12, 2, 255))
    outline.putalpha(alpha)
    big.alpha_composite(outline, (ox, oy))
    grad = Image.new("RGBA", shape.size)
    gp = grad.load()
    for y in range(shape.height):
        t = y / max(1, shape.height - 1)
        c = (int(255 - 40 * t), int(226 - 96 * t), int(120 - 84 * t))
        for x in range(shape.width):
            gp[x, y] = c + (255,)
    grad.putalpha(lum.point(lambda v: 255 if v > 128 else 0).filter(ImageFilter.GaussianBlur(1.5)))
    big.alpha_composite(grad, (ox, oy))
    return big


def sized(big, n):
    img = big.resize((n, n), Image.LANCZOS)
    if n <= 32:
        img = img.filter(ImageFilter.UnsharpMask(radius=1, percent=80 if n == 16 else 50, threshold=0))
    return img


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..")
    out = os.path.join(root, "data", "icon")
    os.makedirs(out, exist_ok=True)
    big = master()
    imgs = {}
    for n in (16, 32, 48, 64, 128, 256):
        imgs[n] = sized(big, n)
        imgs[n].save(os.path.join(out, "pagan_icon_%d.png" % n))
    imgs[48].save(os.path.join(out, "favicon.ico"), sizes=[(16, 16), (32, 32), (48, 48)],
                  append_images=[imgs[16], imgs[32]])
    # The window icon, embedded in the program.
    px = imgs[64].tobytes()
    with open(os.path.join(root, "src", "platform", "icon_data.h"), "w") as f:
        f.write("// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.\n")
        f.write("// Generated by tools/make_icon.py: the 64 x 64 window icon, RGBA bytes (row by row).\n")
        f.write("#pragma once\n#include <cstdint>\n\nnamespace pg {\n")
        f.write("constexpr int kIconSize = 64;\nconst uint8_t kIconRGBA[64 * 64 * 4] = {\n")
        for i in range(0, len(px), 24):
            f.write("    " + ",".join(str(b) for b in px[i:i + 24]) + ",\n")
        f.write("};\n}  // namespace pg\n")
    # Preview: 16, 32, 64 enlarged x8 / x4 / x2 with square pixels, and the 256.
    pv = Image.new("RGBA", (128 * 3 + 256 + 50, 276), (40, 36, 34, 255))
    x = 10
    for n, k in ((16, 8), (32, 4), (64, 2)):
        pv.alpha_composite(imgs[n].resize((n * k, n * k), Image.NEAREST), (x, 10))
        pv.alpha_composite(imgs[n], (x, 150))
        x += 128 + 10
    pv.alpha_composite(imgs[256], (x, 10))
    shots = os.path.join(root, "shots")
    os.makedirs(shots, exist_ok=True)
    pv.save(os.path.join(shots, "icon_preview.png"))
    print("wrote", out, "and src/platform/icon_data.h")


if __name__ == "__main__":
    main()
