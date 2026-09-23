"""Lean the beam over.

The game has no slanted shaft as a sprite — the ones in its dungeons are painted into the floor
tiles. But the DC6 is ours to build, so the game's own vertical beam can be sheared: each row is
pushed sideways in proportion to how far above the ground it is, which is exactly what a shaft of
sunlight does. The art, its colours and its animation stay the game's.
"""
import sys
sys.path.insert(0, '.')
from dcc import decode
from tools import raw_from_mpq, palette, png


def shear(frames, w, h, lean):
    """lean is pixels of sideways push per pixel of height; 0.5 is about 27 degrees."""
    push = int(abs(lean) * h) + 1
    nw = w + push
    out = []
    for f in frames:
        pixels = bytearray(nw * h)
        for y in range(h):
            shift = int(round((h - 1 - y) * lean))
            if lean < 0:
                shift += push
            for x in range(w):
                v = f.pixels[y * w + x]
                if v:
                    tx = x + shift
                    if 0 <= tx < nw:
                        pixels[y * nw + tx] = v
        out.append(pixels)
    return out, nw, h


if __name__ == "__main__":
    raw = raw_from_mpq("d2data.mpq", "data\\global\\overlays\\HoradricLightBeam.dcc")
    frames, w, h, box = decode(raw)[0]
    pal = palette()
    for tag, lean in (("slant35", 0.7), ("slant27", 0.5), ("slant18", 0.32)):
        px, nw, nh = shear(frames, w, h, lean)
        cols = min(len(px), 8)
        rows = (len(px) + cols - 1) // cols
        canvas = [[0, 0, 0, 255] * (cols * nw) for _ in range(rows * nh)]
        for i, p in enumerate(px):
            ox, oy = (i % cols) * nw, (i // cols) * nh
            for y in range(nh):
                row = canvas[oy + y]
                for x in range(nw):
                    v = p[y * nw + x]
                    if v:
                        r, g, b = pal[v]
                        q = (ox + x) * 4
                        row[q], row[q+1], row[q+2], row[q+3] = r, g, b, 255
        png(f"{tag}.png", cols * nw, rows * nh, canvas)
        print(f"{tag}: lean {lean} -> {nw}x{nh}, {len(px)} frames")
