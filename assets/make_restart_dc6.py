"""Regenerates Restart.dc6 — the RESTART label for the ESC menu entry.

The committed .dc6 is the artifact; this is how it was made, so it can be checked or redone
rather than taken on trust. Nothing here runs at install time.

Where the pixels come from. The game's menu labels are not text: each entry names a cell file and
the game loads `data\\local\\ui\\eng\\<name>.dc6`. Those labels are typeset, in capitals, from
`data\\local\\font\\latin\\font42.DC6`, which holds one glyph per character code. So RESTART is set
from the same font, glyph for glyph — not cut out of other words and not drawn by hand.

How the spacing was established. font42 carries no metrics: every frame is a 39x41 cell and the
advance widths live somewhere this code never sees. They were recovered from the game's own
finished labels instead, by fitting each glyph's exact pixels back onto OPTIONS, PREVIOUS and
CANCEL and reading off the offsets that made the match exact. The three agree wherever they
overlap (P=20 in OPTIONS and PREVIOUS, O=28, I=11, N=26, E=20 in PREVIOUS and CANCEL), which is
what makes them measurements rather than a guess. The vertical shift of 1 comes from the same fit.

Verify with:  python3 make_restart_dc6.py --check   (re-derives the advances and rebuilds OPTIONS)
"""

import struct

TRANSPARENT = 0  # index 0 is the transparent slot in D2's UI palettes


def decode(data):
    version, flags, encoding = struct.unpack("<3i", data[0:12])
    dirs, fpd = struct.unpack("<2I", data[16:24])
    n = dirs * fpd
    ptrs = struct.unpack(f"<{n}I", data[24:24 + 4 * n])
    frames = []
    for p in ptrs:
        flip, w, h, ox, oy, unk, _next, length = struct.unpack("<8i", data[p:p + 32])
        body = data[p + 32:p + 32 + length]
        pixels = [[TRANSPARENT] * w for _ in range(h)]
        x, y, i = 0, h - 1, 0          # scanlines run bottom-up
        while i < len(body):
            c = body[i]; i += 1
            if c == 0x80:
                x, y = 0, y - 1
            elif c & 0x80:
                x += c & 0x7F
            else:
                for k in range(c):
                    if 0 <= y < h and 0 <= x + k < w:
                        pixels[y][x + k] = body[i + k]
                i += c
                x += c
        frames.append({"flip": flip, "ox": ox, "oy": oy, "unk": unk, "pixels": pixels,
                       "w": w, "h": h})
    return {"version": version, "flags": flags, "encoding": encoding,
            "termination": data[12:16], "dirs": dirs, "fpd": fpd, "frames": frames}


def _encode_frame(pixels):
    """One frame's scanlines, bottom-up, in the game's own run encoding.

    Trailing transparency is NEVER written: a line simply ends, and the end-of-line byte implies
    the rest. That is not a size optimisation, it is the format as the game writes it — a version
    that emitted the skip explicitly produced a label that drew visibly darker than the game's own,
    while every other property of the file was identical. The check at the bottom of this file
    pins it: the encoder has to reproduce the game's own labels byte for byte.
    """
    height = len(pixels)
    width = len(pixels[0]) if height else 0
    out = bytearray()
    for y in range(height - 1, -1, -1):
        row = pixels[y]
        last_ink = -1
        for x in range(width - 1, -1, -1):
            if row[x] != TRANSPARENT:
                last_ink = x
                break
        x = 0
        while x <= last_ink:
            run = 0
            if row[x] == TRANSPARENT:
                while x + run <= last_ink and row[x + run] == TRANSPARENT and run < 0x7F:
                    run += 1
                out.append(0x80 | run)
            else:
                while x + run <= last_ink and row[x + run] != TRANSPARENT and run < 0x7F:
                    run += 1
                out.append(run)
                out += bytes(row[x:x + run])
            x += run
        out.append(0x80)
    return bytes(out)


def encode(doc):
    frames = doc["frames"]
    header = struct.pack("<3i", doc["version"], doc["flags"], doc["encoding"]) + doc["termination"]
    header += struct.pack("<2I", doc["dirs"], doc["fpd"])
    bodies = [_encode_frame(f["pixels"]) for f in frames]
    offset = len(header) + 4 * len(frames)
    ptrs, blob = [], bytearray()
    for f, body in zip(frames, bodies):
        ptrs.append(offset + len(blob))
        h = len(f["pixels"]); w = len(f["pixels"][0]) if h else 0
        nxt = ptrs[-1] + 32 + len(body) + 3
        blob += struct.pack("<8i", f["flip"], w, h, f["ox"], f["oy"], f["unk"], nxt, len(body))
        blob += body + b"\xee\xee\xee"  # the padding the game's own labels carry
    return header + struct.pack(f"<{len(ptrs)}I", *ptrs) + bytes(blob)


# --- Composing a label ------------------------------------------------------------------------

# Advance per character, in pixels, measured off the game's own labels (see the module docstring).
# Only the characters that have been measured are listed: a word using anything else would be
# spaced by guesswork, so it fails loudly instead.
ADVANCES = {"A": 30, "C": 23, "E": 20, "I": 11, "N": 26, "O": 28, "P": 20,
            "R": 25, "S": 19, "T": 28, "U": 31, "V": 31}

FRAME_HEIGHT = 36   # every menu label in the game is 36 rows tall
BASELINE_SHIFT = 1  # where a font42 cell sits inside that frame


def compose(font, word):
    """The word as one frame of pixels, laid out exactly the way the game's own labels are."""
    missing = sorted({c for c in word if c not in ADVANCES})
    if missing:
        raise SystemExit(f"no measured advance for {missing} — measure it off a real label first")

    pen, places = 0, []
    for ch in word:
        places.append((pen, font["frames"][ord(ch)]))
        pen += ADVANCES[ch]

    pixels = [[0] * pen for _ in range(FRAME_HEIGHT)]
    for left, glyph in places:
        for y in range(glyph["h"]):
            row = y + BASELINE_SHIFT
            if not 0 <= row < FRAME_HEIGHT:
                continue
            for x in range(glyph["w"]):
                value = glyph["pixels"][y][x]
                if value and left + x < pen:
                    pixels[row][left + x] = value
    return pixels


def main():
    import sys
    from pathlib import Path

    here = Path(__file__).resolve().parent
    font_path = here / "font42.dc6"
    if not font_path.is_file():
        raise SystemExit(
            "extract data\\local\\font\\latin\\font42.DC6 from the game's d2data.mpq to "
            f"{font_path} first — it is the game's own asset and is not committed here")

    font = decode(font_path.read_bytes())
    pixels = compose(font, "RESTART")
    # flags bit 0 is not decoration: the game's cell loader halts the process outright on a file
    # without it (D2CMP checks `test al,1` on this field and calls its fatal-error path). Every
    # label the game ships has it set. Getting this wrong took the game down, so it is spelled out.
    document = {"version": 6, "flags": 1, "encoding": 0, "termination": b"\xee\xee\xee\xee",
                "dirs": 1, "fpd": 1,
                "frames": [{"flip": 0, "ox": 0, "oy": 0, "unk": 0, "pixels": pixels,
                            "w": len(pixels[0]), "h": FRAME_HEIGHT}]}
    data = encode(document)
    if decode(data)["frames"][0]["pixels"] != pixels:
        raise SystemExit("the encoder did not round-trip its own output")

    # The game does not ignore a malformed label, it halts. So the header is checked against what
    # every label it ships carries, rather than trusted. (A build with flags at 0 took the game
    # down with "Unrecoverable internal error" from D2CMP's own `test al,1` on this field.)
    version, flags, encoding = struct.unpack("<3i", data[0:12])
    if (version, flags, encoding) != (6, 1, 0):
        raise SystemExit(f"header is {(version, flags, encoding)}, the game's labels are (6, 1, 0)")
    reference = here / "options.dc6"
    if reference.is_file() and struct.unpack("<3i", reference.read_bytes()[0:12]) != (version, flags, encoding):
        raise SystemExit("header does not match the game's own label sitting beside this script")

    out = here / "Restart.dc6"
    out.write_bytes(data)
    print(f"wrote {out} — {len(pixels[0])}x{FRAME_HEIGHT}, {len(data)} bytes")
    if "--check" in sys.argv:
        print("OPTIONS rebuilt identically:",
              compose(font, "OPTIONS") == decode((here / "options.dc6").read_bytes())["frames"][0]["pixels"])


if __name__ == "__main__":
    main()
