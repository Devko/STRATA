#!/usr/bin/env python3
"""Turn the host build's screen dumps into PNGs for the README.

    tools/screenshots.py DUMPS OUT

DUMPS holds NAME.pgm (the canvas, greys 0-15) and NAME.txt ("x y grey text" a line) as written
by the ui_screenshots host test (tools/screenshots.sh runs both). The text is drawn here, in
Enaga's own 5 x 7 font, read from third_party/enaga/firmware/hal/gfx.c so nothing is copied.
Writes OUT/NAME.png for each screen and OUT/overview.png with all of them. Standard library only.
"""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GFX = os.path.join(ROOT, "third_party", "enaga", "firmware", "hal", "gfx.c")
W = H = 128
SCALE = 4          # each screen pixel becomes a 4 x 4 block, with a faint grid like the OLED
BORDER = 8         # screen pixels of bezel around each screen
ORDER = ["grid", "step", "edit", "mixer", "perform", "keys", "machine", "kit", "project"]


def load_font():
    src = open(GFX).read()
    table = re.search(r"static const u8 font\[\]\[5\] = \{(.*?)\};", src, re.S).group(1)
    table = re.sub(r"/\*.*?\*/", "", table, flags=re.S)
    glyphs = [[int(v, 0) for v in g.split(",") if v.strip()]
              for g in re.findall(r"G\(([^)]*)\)", table)]
    chars = re.search(r'font_chars\[\] = "([^"]*)"', src).group(1)
    assert len(glyphs) == len(chars), (len(glyphs), len(chars))
    return dict(zip(chars, glyphs)), glyphs[chars.index("?")]


def text(screen, x, y, s, grey, font, unknown):
    for ch in s:                                   # gfx_text: columns, bit 0 the top row, 6 apart
        g = font.get(ch, unknown)
        for cx in range(5):
            for cy in range(7):
                if g[cx] >> cy & 1 and 0 <= x + cx < W and 0 <= y + cy < H:
                    screen[(y + cy) * W + x + cx] = grey
        x += 6


def load_screen(folder, name, font, unknown):
    data = open(os.path.join(folder, name + ".pgm"), "rb").read()
    screen = list(data[-W * H:])
    for line in open(os.path.join(folder, name + ".txt")):
        line = line.rstrip("\n")
        parts = line.split(" ", 3)
        if len(parts) < 4:
            continue
        text(screen, int(parts[0]), int(parts[1]), parts[3], int(parts[2]), font, unknown)
    return screen


def colour(grey):
    if grey == 0:
        return (10, 12, 16)
    t = grey / 15.0                                # a cool white OLED
    return (int(30 + 215 * t), int(36 + 214 * t), int(44 + 211 * t))


def render(screen):
    size = (W + 2 * BORDER) * SCALE
    rows = []
    for py in range(size):
        sy = py // SCALE - BORDER
        row = bytearray()
        for px in range(size):
            sx = px // SCALE - BORDER
            if 0 <= sx < W and 0 <= sy < H:
                r, g, b = colour(screen[sy * W + sx])
                if px % SCALE == SCALE - 1 or py % SCALE == SCALE - 1:
                    r, g, b = r * 3 // 4, g * 3 // 4, b * 3 // 4
            else:
                r, g, b = (24, 26, 30)
            row += bytes((r, g, b))
        rows.append(bytes(row))
    return size, size, rows


def tile(images, columns, gap):
    w, h = images[0][0], images[0][1]
    lines = (len(images) + columns - 1) // columns
    out_w = columns * w + (columns + 1) * gap
    out_h = lines * h + (lines + 1) * gap
    blank = bytes((13, 17, 23)) * out_w
    rows = [bytearray(blank) for _ in range(out_h)]
    for i, (_, _, img) in enumerate(images):
        ox = gap + (i % columns) * (w + gap)
        oy = gap + (i // columns) * (h + gap)
        for y in range(h):
            rows[oy + y][ox * 3:(ox + w) * 3] = img[y]
    return out_w, out_h, [bytes(r) for r in rows]


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(
            ">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dumps, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    font, unknown = load_font()
    images = []
    for name in ORDER:
        image = render(load_screen(dumps, name, font, unknown))
        write_png(os.path.join(out, name + ".png"), *image)
        images.append(image)
    write_png(os.path.join(out, "overview.png"), *tile(images, 3, 24))
    print("wrote %d screens and overview.png to %s" % (len(images), out))


if __name__ == "__main__":
    main()
