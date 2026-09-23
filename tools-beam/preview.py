import sys
from dcc import decode
from tools import raw_from_mpq, palette, png

name = sys.argv[1]
data = raw_from_mpq("d2data.mpq", f"data\\global\\overlays\\{name}.dcc")
print(name, "bytes", len(data))
dirs = decode(data)
print("directions", len(dirs))
frames, w, h, box = dirs[0]
print("frames", len(frames), "size", w, "x", h, "box", box)

pal = palette()
cols = min(len(frames), 8)
rows = (len(frames) + cols - 1) // cols
sheet_w, sheet_h = cols * w, rows * h
canvas = [[0, 0, 0, 255] * sheet_w for _ in range(sheet_h)]
for i, f in enumerate(frames):
    ox, oy = (i % cols) * w, (i // cols) * h
    for y in range(h):
        row = canvas[oy + y]
        for x in range(w):
            v = f.pixels[y * w + x]
            if v:
                r, g, b = pal[v]
                p = (ox + x) * 4
                row[p], row[p+1], row[p+2], row[p+3] = r, g, b, 255
png(f"{name}.png", sheet_w, sheet_h, canvas)
print("wrote", f"{name}.png", sheet_w, sheet_h)
