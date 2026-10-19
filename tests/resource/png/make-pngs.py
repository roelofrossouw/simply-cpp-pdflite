"""Writes the PNGs sc-pdflite's tests read: every colour type, several bit depths, each row
filter, Adam7 interlacing, and transparency as an alpha channel, a palette's alphas and a key
colour. Each pixel follows a pattern the test (tests/pdflite.cpp) computes too, so it can check
the decoded pixels exactly. Run with python3 from this folder; only the standard library."""
import struct, zlib

W, H = 7, 5  # odd sizes, so every Adam7 pass has an edge

def rgb(x, y): return (x * 30 % 256, y * 50 % 256, (x + y) * 10 % 256)
def alpha(x, y): return (x * y * 13 + 40) % 256
def grey(x, y, depth): return (x + 2 * y) % (1 << depth)

def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c

def filtered(rows, bpp):
    """Each row behind its filter byte, the filter cycling through all five."""
    out, previous = b"", bytes(len(rows[0])) if rows else b""
    for index, row in enumerate(rows):
        kind = index % 5
        line = bytearray()
        for i, value in enumerate(row):
            left = row[i - bpp] if i >= bpp else 0
            up = previous[i]; corner = previous[i - bpp] if i >= bpp else 0
            predicted = [0, left, up, (left + up) // 2, paeth(left, up, corner)][kind]
            line.append((value - predicted) % 256)
        out += bytes([kind]) + bytes(line); previous = row
    return out

def pack(samples, depth):
    """Samples of a row at depth: big-endian 16 bits, bytes, or bits from the left."""
    if depth == 16: return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8: return bytes(samples)
    out, bits, count = bytearray(), 0, 0
    for s in samples:
        bits = bits << depth | s; count += depth
        if count == 8: out.append(bits); bits = count = 0
    if count: out.append(bits << (8 - count))
    return bytes(out)

def write(name, colour_type, depth, pixel, interlace=False, extra=b""):
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour_type]
    bpp = max(1, channels * depth // 8)
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)] if interlace else [(0, 0, 1, 1)]
    data = b""
    for x0, y0, dx, dy in passes:
        xs, ys = range(x0, W, dx), range(y0, H, dy)
        if not xs or not ys: continue
        rows = [pack([s for x in xs for s in pixel(x, y)], depth) for y in ys]
        data += filtered(rows, bpp)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, depth, colour_type, 0, 0, 1 if interlace else 0))
    png += extra + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")
    open(name, "wb").write(png)

palette = b"".join(bytes([i * 36 % 256, i * 18 % 256, 255 - i * 30]) for i in range(8))
write("rgb8.png", 2, 8, lambda x, y: rgb(x, y))
write("rgb16.png", 2, 16, lambda x, y: [v * 257 for v in rgb(x, y)])
write("grey1.png", 0, 1, lambda x, y: [grey(x, y, 1)])
write("grey4.png", 0, 4, lambda x, y: [grey(x, y, 4)])
write("palette4.png", 3, 4, lambda x, y: [(x + y) % 8], extra=chunk(b"PLTE", palette))
write("rgba8.png", 6, 8, lambda x, y: list(rgb(x, y)) + [alpha(x, y)])
write("grey-alpha16.png", 4, 16, lambda x, y: [grey(x, y, 8) * 257, alpha(x, y) * 257])
write("palette-alpha.png", 3, 8, lambda x, y: [(x + y) % 8], extra=chunk(b"PLTE", palette) + chunk(b"tRNS", bytes([0, 64, 128, 255])))
write("grey-key.png", 0, 4, lambda x, y: [grey(x, y, 4)], extra=chunk(b"tRNS", struct.pack(">H", 3)))
write("rgb-interlaced.png", 2, 8, lambda x, y: rgb(x, y), interlace=True)
write("rgba-interlaced.png", 6, 8, lambda x, y: list(rgb(x, y)) + [alpha(x, y)], interlace=True)
