"""Generate the PLACEHOLDER Sea Otter boot logo: app/install/usb/ddcsv4/logo.bmp.

The controller's boot logo is a 1024 x 600, 24-bit, uncompressed Windows BMP named
logo.bmp in the ddcsv4 folder (same format as the vendor's). Sea Otter shows it while it
starts. To use your own artwork, just replace logo.bmp with a 1024 x 600 24-bit BMP -
this script is only the stand-in until then.

    python app/install/logo/make-logo.py        (needs Pillow)
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(APP, "install", "usb", "ddcsv4", "logo.bmp")
FONTS = os.path.join(APP, "tools", "fonts")
W, H = 1024, 600

# the panel UI's marine palette (app/src/ui/gfx.h)
BG0, BG1 = (0x14, 0x40, 0x7A), (0x06, 0x17, 0x2F)
CYAN, TEAL, AMBER, WHITE, DIM = (0x5C, 0xC8, 0xFF), (0x3D, 0xDB, 0xB0), (0xFF, 0xB5, 0x47), (255, 255, 255), (0x8F, 0xA7, 0xCA)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


img = Image.new("RGB", (W, H))
d = ImageDraw.Draw(img)
for y in range(H):                                  # deep sea: lighter at the surface
    d.line([(0, y), (W, y)], fill=lerp(BG0, BG1, min(1, y / (H * 0.85))))

# swell lines
for k, (yy, amp, col) in enumerate([(150, 6, (0x24, 0x56, 0x92)), (205, 8, (0x1E, 0x4C, 0x84)),
                                     (470, 7, (0x10, 0x34, 0x63)), (520, 9, (0x0C, 0x2A, 0x52))]):
    pts = [(x, yy + amp * math.sin(x / (55 + 9 * k) + k)) for x in range(0, W + 8, 8)]
    d.line(pts, fill=col, width=3)

# a few bubbles
for x, y, r in [(780, 110, 6), (800, 80, 4), (770, 60, 3), (140, 300, 5), (160, 270, 3)]:
    d.ellipse([x - r, y - r, x + r, y + r], outline=(0x3C, 0x7C, 0xA8), width=2)

# --- the controller (front panel, stylised) ---
cx, cy, cw, ch = 590, 330, 330, 190
d.rounded_rectangle([cx, cy, cx + cw, cy + ch], 10, fill=(0x1A, 0x1E, 0x24), outline=(0x35, 0x5F, 0x99), width=2)
d.rectangle([cx + 16, cy + 18, cx + 230, cy + 140], fill=(0x0A, 0x22, 0x44), outline=(0x24, 0x47, 0x78))
for i in range(4):                                  # DRO lines on its screen
    d.rectangle([cx + 28, cy + 30 + i * 26, cx + 120, cy + 42 + i * 26], fill=(0x5C, 0xC8, 0xFF))
    d.rectangle([cx + 140, cy + 30 + i * 26, cx + 215, cy + 42 + i * 26], fill=(0x3D, 0x6B, 0x9E))
for r in range(4):                                  # key block right of the screen
    for c in range(2):
        d.rounded_rectangle([cx + 246 + c * 38, cy + 18 + r * 31, cx + 276 + c * 38, cy + 42 + r * 31], 4,
                            fill=(0x2B, 0x8F, 0xE0) if c == 0 else (0xC9, 0xCF, 0xD6))
key_cols = [(0x38, 0xD3, 0x9F), (0xF2, 0xD1, 0x3E), (0xE0, 0x4B, 0x4B)] + [(0x2E, 0x34, 0x3C)] * 4
for i, col in enumerate(key_cols):                  # the soft-key row
    d.rounded_rectangle([cx + 16 + i * 31, cy + 152, cx + 42 + i * 31, cy + 176], 4, fill=col)
d.text((cx + 250, cy + 152), "DDCS V4.1", fill=DIM, font=ImageFont.truetype(os.path.join(FONTS, "BarlowSemiCondensed-SemiBold.ttf"), 15))

# --- the otter, sitting on top of the controller ---
fur, belly, dark = (0x6B, 0x4A, 0x33), (0xC9, 0xA9, 0x82), (0x2A, 0x1C, 0x14)
ox, base = 760, cy                                   # sits on the top edge
d.ellipse([ox - 70, base - 150, ox + 70, base + 8], fill=fur)               # body
d.ellipse([ox - 42, base - 120, ox + 42, base - 4], fill=belly)             # belly
d.ellipse([ox - 52, base - 230, ox + 52, base - 128], fill=fur)             # head
d.ellipse([ox - 34, base - 196, ox + 34, base - 140], fill=belly)           # face
for ex in (-40, 40):                                                         # ears
    d.ellipse([ox + ex - 12, base - 230, ox + ex + 12, base - 206], fill=fur)
for ex in (-17, 17):                                                         # eyes
    d.ellipse([ox + ex - 6, base - 196, ox + ex + 6, base - 184], fill=dark)
    d.ellipse([ox + ex - 2, base - 194, ox + ex + 1, base - 191], fill=WHITE)
d.ellipse([ox - 9, base - 180, ox + 9, base - 168], fill=dark)             # nose
for s in (-1, 1):                                                            # whiskers
    for k in range(3):
        d.line([(ox + s * 14, base - 166 + k * 4), (ox + s * 50, base - 172 + k * 8)], fill=(0xE8, 0xDD, 0xCC), width=1)
for px in (-30, 30):                                                         # paws holding the edge
    d.ellipse([ox + px - 16, base - 14, ox + px + 16, base + 10], fill=dark)
d.ellipse([ox + 50, base - 30, ox + 140, base + 4], fill=fur)               # tail over the side
d.polygon([(ox + 120, base - 20), (ox + 150, base + 30), (ox + 130, base + 34)], fill=fur)

# --- title ---
big = ImageFont.truetype(os.path.join(FONTS, "BarlowSemiCondensed-SemiBold.ttf"), 92)
sub = ImageFont.truetype(os.path.join(FONTS, "BarlowSemiCondensed-Medium.ttf"), 26)
small = ImageFont.truetype(os.path.join(FONTS, "BarlowSemiCondensed-Medium.ttf"), 18)
d.text((70, 250), "SEA OTTER", fill=WHITE, font=big)
d.text((74, 352), "open controller software for the DDCS V4.1", fill=CYAN, font=sub)
d.rectangle([74, 396, 110, 400], fill=TEAL)
d.text((74, 540), "placeholder boot logo - replace ddcsv4/logo.bmp (1024 x 600, 24-bit BMP)", fill=(0x5D, 0x78, 0x9F), font=small)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
img.save(OUT, "BMP")
print("wrote", OUT, os.path.getsize(OUT), "bytes")
img.save(os.path.join(HERE, "logo-preview.png"))
