"""Draws the Toronja icon (a grapefruit half) and writes it as a 256x256 JPEG.

Usage: python make_icon.py <out.jpg> [<out.png>]
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 4  # supersampling
N = 256 * S
img = Image.new("RGB", (N, N), (0, 0, 0))
d = ImageDraw.Draw(img)

# Background: deep green, lighter towards the top left.
for y in range(N):
    t = y / N
    d.line([(0, y), (N, y)], fill=(int(22 + 10 * (1 - t)), int(52 + 34 * (1 - t)), int(46 + 22 * (1 - t))))

cx, cy = N // 2, N // 2 + 2 * S
R = 104 * S


def disc(r, colour, dx=0, dy=0):
    d.ellipse([cx - r + dx, cy - r + dy, cx + r + dx, cy + r + dy], fill=colour)


# Soft shadow under the fruit.
shadow = Image.new("L", (N, N), 0)
ImageDraw.Draw(shadow).ellipse([cx - R, cy - R + 10 * S, cx + R, cy + R + 10 * S], fill=150)
shadow = shadow.filter(ImageFilter.GaussianBlur(8 * S))
img.paste((8, 22, 20), mask=shadow)
d = ImageDraw.Draw(img)

disc(R, (247, 168, 60))            # rind
disc(R - 7 * S, (252, 232, 196))   # pith
disc(R - 15 * S, (240, 96, 92))    # flesh

# Segments: lighter membranes radiating from the centre, with rounded outer corners.
SEGMENTS = 10
inner = 12 * S
outer = R - 15 * S
for i in range(SEGMENTS):
    a = 2 * math.pi * i / SEGMENTS + math.pi / SEGMENTS
    x0, y0 = cx + inner * math.cos(a), cy + inner * math.sin(a)
    x1, y1 = cx + outer * math.cos(a), cy + outer * math.sin(a)
    d.line([(x0, y0), (x1, y1)], fill=(252, 216, 190), width=5 * S)

# Juice highlights inside each segment.
for i in range(SEGMENTS):
    a = 2 * math.pi * i / SEGMENTS
    for k, rr in enumerate((0.52, 0.74)):
        r = outer * rr
        x, y = cx + r * math.cos(a), cy + r * math.sin(a)
        w = (9 - 3 * k) * S
        d.ellipse([x - w, y - w, x + w, y + w], fill=(246, 122, 112))

disc(inner, (252, 226, 200))       # core
disc(R, None)

# Gloss: a pale arc on the upper left of the rind.
gloss = Image.new("L", (N, N), 0)
g = ImageDraw.Draw(gloss)
g.arc([cx - R + 3 * S, cy - R + 3 * S, cx + R - 3 * S, cy + R - 3 * S], 200, 262, fill=150, width=4 * S)
gloss = gloss.filter(ImageFilter.GaussianBlur(1.5 * S))
img.paste((255, 244, 214), mask=gloss)

out = img.resize((256, 256), Image.LANCZOS)
out.save(sys.argv[1], "JPEG", quality=93)
if len(sys.argv) > 2:
    out.save(sys.argv[2], "PNG")
print("ok", sys.argv[1])
