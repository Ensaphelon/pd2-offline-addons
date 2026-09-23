"""A DCC reader, ported from OpenDiablo2/dcc (Go, MIT) closely enough to be checkable against it.

DCC is Diablo II's animation format: one bitstream per direction, frames packed into 4x4 cells
that reuse each other's pixels. Nothing here is guessed -- the structure, the bit widths and the
cell arithmetic all come from that implementation.
"""
from __future__ import annotations

CRAZY = [0, 1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 26, 28, 30, 32]
PIXEL_MASK_LOOKUP = [0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4]
CELL = 4


class Bits:
    """Least-significant bit first, which is how the game packs them."""

    def __init__(self, data: bytes, bit_position: int = 0):
        self.data = data
        self.p = bit_position
        self.read = 0

    def take(self, n: int) -> int:
        value = 0
        for i in range(n):
            value |= ((self.data[self.p >> 3] >> (self.p & 7)) & 1) << i
            self.p += 1
        self.read += n
        return value

    def take_signed(self, n: int) -> int:
        value = self.take(n)
        if n and (value & (1 << (n - 1))):
            value -= 1 << n
        return value


class Frame:
    __slots__ = ("width", "height", "xoff", "yoff", "box", "cells", "hcells", "vcells", "pixels")


class Cell:
    __slots__ = ("w", "h", "x", "y", "lw", "lh", "lx", "ly")

    def __init__(self, w=0, h=0, x=0, y=0):
        self.w, self.h, self.x, self.y = w, h, x, y
        self.lw = self.lh = -1
        self.lx = self.ly = 0


def _frame_cell_counts(frame: Frame, dir_box) -> None:
    first_w = CELL - ((frame.box[0] - dir_box[0]) % CELL)
    first_h = CELL - ((frame.box[1] - dir_box[1]) % CELL)

    rw = frame.width - first_w - 1
    rh = frame.height - first_h - 1
    frame.hcells = 2 + rw // CELL - (1 if rw % CELL == 0 else 0)
    frame.vcells = 2 + rh // CELL - (1 if rh % CELL == 0 else 0)
    frame.hcells = max(frame.hcells, 1)
    frame.vcells = max(frame.vcells, 1)

    widths = [frame.width]
    if frame.hcells > 1:
        widths = [first_w] + [CELL] * (frame.hcells - 2)
        widths.append(frame.width - first_w - CELL * (frame.hcells - 2))
    heights = [frame.height]
    if frame.vcells > 1:
        heights = [first_h] + [CELL] * (frame.vcells - 2)
        heights.append(frame.height - first_h - CELL * (frame.vcells - 2))

    frame.cells = []
    y = frame.box[1] - dir_box[1]
    for cy in range(frame.vcells):
        x = frame.box[0] - dir_box[0]
        for cx in range(frame.hcells):
            frame.cells.append(Cell(widths[cx], heights[cy], x, y))
            x += widths[cx]
        y += heights[cy]


def decode(data: bytes):
    """Returns (frames, width, height, box) where each frame is a bytearray of palette indices."""
    head = Bits(data)
    if head.take(8) != 0x74:
        raise ValueError("not a DCC")
    head.take(8)                      # version
    num_dirs = head.take(8)
    frames_per_dir = head.take(32)
    if head.take(32) != 1:
        raise ValueError("DCC sanity check failed")
    head.take(32)                     # total size, coded
    dir_offsets = [head.take(32) for _ in range(num_dirs)]

    directions = [_decode_direction(data, off * 8, frames_per_dir) for off in dir_offsets]
    return directions


def _decode_direction(data: bytes, bit_position: int, frame_count: int):
    s = Bits(data, bit_position)
    s.take(32)                        # output size, coded
    flags = s.take(2)
    var0_bits = CRAZY[s.take(4)]
    width_bits = CRAZY[s.take(4)]
    height_bits = CRAZY[s.take(4)]
    xoff_bits = CRAZY[s.take(4)]
    yoff_bits = CRAZY[s.take(4)]
    optional_bits = CRAZY[s.take(4)]
    coded_bytes_bits = CRAZY[s.take(4)]

    frames = []
    for _ in range(frame_count):
        f = Frame()
        s.take(var0_bits)
        f.width = s.take(width_bits)
        f.height = s.take(height_bits)
        f.xoff = s.take_signed(xoff_bits)
        f.yoff = s.take_signed(yoff_bits)
        s.take(optional_bits)
        s.take(coded_bytes_bits)
        if s.take(1):
            raise ValueError("bottom-up frames are not supported")
        f.box = (f.xoff, f.yoff - f.height + 1, f.xoff + f.width, f.yoff + 1)
        frames.append(f)

    if optional_bits:
        raise ValueError("optional per-frame data is not supported")

    box = (
        min(f.box[0] for f in frames),
        min(f.box[1] for f in frames),
        max(f.box[2] for f in frames),
        max(f.box[3] for f in frames),
    )
    dx, dy = box[2] - box[0], box[3] - box[1]

    equal_cells_size = s.take(20) if flags & 2 else 0
    pixel_mask_size = s.take(20)
    encoding_type_size = s.take(20) if flags & 1 else 0
    raw_pixel_size = s.take(20) if flags & 1 else 0

    palette = []
    for idx in range(256):
        if s.take(1):
            palette.append(idx)
    palette += [0] * (256 - len(palette))

    base = s.p
    ec = Bits(data, base)
    pm = Bits(data, base + equal_cells_size)
    et = Bits(data, base + equal_cells_size + pixel_mask_size)
    rp = Bits(data, base + equal_cells_size + pixel_mask_size + encoding_type_size)
    pcd = Bits(data, base + equal_cells_size + pixel_mask_size + encoding_type_size + raw_pixel_size)

    hcells = 1 + (dx - 1) // CELL
    vcells = 1 + (dy - 1) // CELL
    widths = [dx] if hcells == 1 else [CELL] * (hcells - 1) + [dx - CELL * (hcells - 1)]
    heights = [dy] if vcells == 1 else [CELL] * (vcells - 1) + [dy - CELL * (vcells - 1)]
    cells = [Cell(widths[x], heights[y], x * CELL, y * CELL)
             for y in range(vcells) for x in range(hcells)]

    for f in frames:
        _frame_cell_counts(f, box)

    buffer = _fill_pixel_buffer(frames, cells, hcells, box, palette, ec, pm, et, rp, pcd,
                                equal_cells_size, encoding_type_size)
    _generate_frames(frames, cells, hcells, dx, dy, buffer, pcd)
    return frames, dx, dy, box


def _fill_pixel_buffer(frames, cells, hcells, box, palette, ec, pm, et, rp, pcd,
                       equal_cells_size, encoding_type_size):
    buffer = []
    cell_buffer = [None] * len(cells)

    for frame_index, frame in enumerate(frames):
        origin_x = (frame.box[0] - box[0]) // CELL
        origin_y = (frame.box[1] - box[1]) // CELL

        for cy in range(frame.vcells):
            for cx in range(frame.hcells):
                current = origin_x + cx + (cy + origin_y) * hcells
                old = cell_buffer[current]

                if old is not None:
                    same = ec.take(1) if equal_cells_size > 0 else 0
                    if same:
                        continue
                    pixel_mask = pm.take(4)
                else:
                    pixel_mask = 0x0F

                stack = [0, 0, 0, 0]
                last = 0
                count = PIXEL_MASK_LOOKUP[pixel_mask]
                encoding = et.take(1) if count and encoding_type_size > 0 else 0

                decoded = 0
                for i in range(count):
                    if encoding:
                        stack[i] = rp.take(8)
                    else:
                        stack[i] = last
                        step = pcd.take(4)
                        stack[i] += step
                        while step == 15:
                            step = pcd.take(4)
                            stack[i] += step
                    if stack[i] == last:
                        stack[i] = 0
                        break
                    last = stack[i]
                    decoded += 1

                value = [0, 0, 0, 0]
                cursor = decoded - 1
                for i in range(4):
                    if pixel_mask & (1 << i):
                        if cursor >= 0:
                            value[i] = stack[cursor]
                            cursor -= 1
                        else:
                            value[i] = 0
                    else:
                        value[i] = old.value[i]

                entry = _Entry(value, frame_index, cx + cy * frame.hcells)
                buffer.append(entry)
                cell_buffer[current] = entry

    for entry in buffer:
        entry.value = [palette[v] for v in entry.value]
    return buffer


class _Entry:
    __slots__ = ("value", "frame", "cell")

    def __init__(self, value, frame, cell):
        self.value, self.frame, self.cell = value, frame, cell


def _generate_frames(frames, cells, hcells, dx, dy, buffer, pcd):
    for cell in cells:
        cell.lw = cell.lh = -1
    scratch = bytearray(dx * dy)
    index = 0

    for frame_index, frame in enumerate(frames):
        frame.pixels = bytearray(dx * dy)
        for cell_index, cell in enumerate(frame.cells):
            buffer_cell = cells[cell.x // CELL + (cell.y // CELL) * hcells]
            entry = buffer[index] if index < len(buffer) else None

            if entry is None or entry.frame != frame_index or entry.cell != cell_index:
                if cell.w != buffer_cell.lw or cell.h != buffer_cell.lh:
                    for y in range(cell.h):
                        row = (y + cell.y) * dx + cell.x
                        for x in range(cell.w):
                            scratch[row + x] = 0
                else:
                    for y in range(cell.h):
                        dest = (y + cell.y) * dx + cell.x
                        src = (y + buffer_cell.ly) * dx + buffer_cell.lx
                        for x in range(cell.w):
                            scratch[dest + x] = scratch[src + x]
                    for y in range(cell.h):
                        row = (y + cell.y) * dx + cell.x
                        for x in range(cell.w):
                            frame.pixels[row + x] = scratch[row + x]
            else:
                if entry.value[0] == entry.value[1]:
                    for y in range(cell.h):
                        row = (y + cell.y) * dx + cell.x
                        for x in range(cell.w):
                            scratch[row + x] = entry.value[0]
                else:
                    bits = 2 if entry.value[1] != entry.value[2] else 1
                    for y in range(cell.h):
                        row = (y + cell.y) * dx + cell.x
                        for x in range(cell.w):
                            scratch[row + x] = entry.value[pcd.take(bits)]
                for y in range(cell.h):
                    row = (y + cell.y) * dx + cell.x
                    for x in range(cell.w):
                        frame.pixels[row + x] = scratch[row + x]
                index += 1

            buffer_cell.lw, buffer_cell.lh = cell.w, cell.h
            buffer_cell.lx, buffer_cell.ly = cell.x, cell.y
