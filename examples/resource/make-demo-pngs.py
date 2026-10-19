"""Draws the PNGs sc-pdflite-png shows: badge.png (RGBA, a disc whose edge fades out),
badge-interlaced.png (the same, Adam7-interlaced), stamp.png (a 16-colour palette, its first
colour transparent) and gradient.png (RGB, opaque: copied into a PDF as it is). Run with python3 from this folder; only the standard library."""
import math, struct, zlib

def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

def png(name, width, height, colour_type, rows_of, extra=b"", interlace=False):
    """rows_of(xs, y) gives a row's bytes for the pixels xs on line y; filter 0 throughout."""
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)] if interlace else [(0, 0, 1, 1)]
    data = b""
    for x0, y0, dx, dy in passes:
        xs = list(range(x0, width, dx))
        if not xs: continue
        for y in range(y0, height, dy): data += b"\x00" + rows_of(xs, y)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 1 if interlace else 0))
    open(name, "wb").write(out + extra + chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b""))

S = 200
def badge(xs, y):
    row = bytearray()
    for x in xs:
        dx, dy = x - S / 2 + 0.5, y - S / 2 + 0.5
        r = math.hypot(dx, dy) / (S / 2)
        hue = (math.atan2(dy, dx) / (2 * math.pi)) % 1
        red, green, blue = (int(127 + 127 * math.cos(2 * math.pi * (hue + k / 3))) for k in range(3))
        alpha = 255 if r < 0.7 else max(0, int(255 * (1 - (r - 0.7) / 0.3)))  # the edge fades out
        row += bytes([red, green, blue, alpha])
    return bytes(row)
png("badge.png", S, S, 6, badge)
png("badge-interlaced.png", S, S, 6, badge, interlace=True)

palette = bytes([255, 255, 255]) + b"".join(bytes([int(255 * (0.5 + 0.5 * math.cos(i / 15 * 6.283 + k * 2.094))) for k in range(3)]) for i in range(15))
def stamp(xs, y):
    row = bytearray()
    for x in xs:
        dx, dy = x - 80, y - 80
        angle, r = math.atan2(dy, dx), math.hypot(dx, dy)
        inside = r < 40 + 30 * (0.5 + 0.5 * math.cos(5 * angle))  # a five-pointed flower
        row.append(1 + int(r / 6) % 15 if inside else 0)            # outside: index 0, see-through
    return bytes(row)
png("stamp.png", 160, 160, 3, stamp, extra=chunk(b"PLTE", palette) + chunk(b"tRNS", b"\x00"))

def gradient(xs, y):
    return b"".join(bytes([int(255 * x / 239), int(255 * y / 79), 160]) for x in xs)
png("gradient.png", 240, 80, 2, gradient)
