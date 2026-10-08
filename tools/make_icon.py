"""Draws Refractory's logo (a faceted prism splitting a white beam into a spectrum) and writes
res/Refractory.ico (16, 24, 32, 48, 64, 128 and 256 px) and res/logo-256.png.

The geometry is in a 180 x 180 design space, the same as res/logo.svg. Sizes up to 32 px use a
simpler drawing (enlarged, three thicker rays, no facet highlights), because six 1 px rays merge into mud.

Run: python tools/make_icon.py  (needs Pillow)
"""
import io
import os
import struct

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "res")
SS = 8  # supersampling factor

TILE = (0x1F, 0x20, 0x23)
LIT = (0x36, 0x3C, 0x48)
SHADE = (0x26, 0x2A, 0x32)
EDGE = (0xE8, 0xE8, 0xEC)
WHITE = (0xFF, 0xFF, 0xFF)

APEX, LEFT, RIGHT, MID = (90, 34), (38, 138), (142, 138), (104, 138)
ENTRY, EXIT = (63, 88), (120, 94)

RAYS = [  # end point, colour
    ((164, 64), (0xE5, 0x48, 0x4D)),
    ((168, 80), (0xF2, 0xA3, 0x3A)),
    ((169, 96), (0xE8, 0xD4, 0x4D)),
    ((168, 112), (0x46, 0xA7, 0x58)),
    ((166, 128), (0x3E, 0x8E, 0xD0)),
    ((162, 143), (0x8E, 0x4E, 0xC6)),
]
RAYS_SMALL = [RAYS[0], RAYS[3], RAYS[5]]


class Canvas:
    def __init__(self, size, zoom=1.0):
        self.px = size * SS
        self.k = self.px / 180.0
        self.zoom = zoom
        self.img = Image.new("RGBA", (self.px, self.px), (0, 0, 0, 0))

    def p(self, pt):
        # zoom enlarges the drawing about the tile's centre (the tile itself is drawn in pixels).
        return (((pt[0] - 90) * self.zoom + 90) * self.k, ((pt[1] - 90) * self.zoom + 90) * self.k)

    def layer(self, opacity, fn):
        """Draws fn onto its own layer, composited at the given opacity."""
        lay = Image.new("RGBA", self.img.size, (0, 0, 0, 0))
        fn(ImageDraw.Draw(lay))
        if opacity < 1:
            a = lay.getchannel("A").point(lambda v: int(v * opacity + 0.5))
            lay.putalpha(a)
        self.img.alpha_composite(lay)

    def polygon(self, pts, fill, opacity=1.0):
        self.layer(opacity, lambda d: d.polygon([self.p(q) for q in pts], fill=fill))

    def line(self, a, b, colour, width, opacity=1.0, round_caps=False):
        w = max(1, round(width * self.k * self.zoom))

        def fn(d):
            d.line([self.p(a), self.p(b)], fill=colour, width=w)
            if round_caps:
                for q in (a, b):
                    x, y = self.p(q)
                    r = w / 2
                    d.ellipse([x - r, y - r, x + r, y + r], fill=colour)

        self.layer(opacity, fn)

    def outline(self, pts, colour, width):
        # Joins are rounded with discs at the corners, as stroke-linejoin="round" in the SVG.
        for i in range(len(pts)):
            self.line(pts[i], pts[(i + 1) % len(pts)], colour, width, round_caps=True)

    def result(self, size):
        return self.img.resize((size, size), Image.LANCZOS)


def draw(size):
    small = size <= 32
    c = Canvas(size, 1.15 if small else 1.0)
    tile = Image.new("L", c.img.size, 0)
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, c.px - 1, c.px - 1], radius=36 * c.k, fill=255)
    c.img.paste(TILE + (255,), mask=tile)
    c.polygon([APEX, LEFT, MID], LIT)
    c.polygon([APEX, MID, RIGHT], SHADE)
    if not small:
        c.line((86, 44), (46, 128), WHITE, 2, opacity=0.18)
        c.line(APEX, MID, EDGE, 1.5, opacity=0.35)
    c.outline([APEX, LEFT, RIGHT], EDGE, 4 if small else 2.5)
    c.line((10, 104), ENTRY, WHITE, 7 if small else 4.5, round_caps=True)
    c.line(ENTRY, EXIT, WHITE, 5 if small else 3, opacity=0.55)
    for end, colour in (RAYS_SMALL if small else RAYS):
        c.line(EXIT, end, colour, 9 if small else 4, round_caps=True)
    c.img.putalpha(tile)  # the zoomed rays run past the tile's corners
    return c.result(size)


def write_ico(path, images):
    """An .ico whose entries are PNGs (Windows Vista and later), one drawing per size."""
    blobs = []
    for im in images:
        buf = io.BytesIO()
        im.save(buf, "PNG")
        blobs.append(buf.getvalue())
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        w = im.width if im.width < 256 else 0
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [draw(s) for s in sizes]
    write_ico(os.path.join(OUT, "Refractory.ico"), images)
    images[-1].save(os.path.join(OUT, "logo-256.png"))
    print("wrote", os.path.join(OUT, "Refractory.ico"))
