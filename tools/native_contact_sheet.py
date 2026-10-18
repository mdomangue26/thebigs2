#!/usr/bin/env python3
"""Contact sheet of a native capture's texture previews (phase 2).

  native_contact_sheet.py <native_capture_NNN_textures> <out.png> [cell px]

The capture writes one uncompressed PNG per converted texture when
--native_capture_textures=<edge> is set (src/native_capture.cpp). This tiles them
(scaled to fit a cell, on a checkerboard so alpha shows) into one compressed PNG,
and writes <out>.txt mapping grid positions to file names.
No dependencies beyond the standard library.
"""
import os, struct, sys, zlib


def read_png(path):
    """RGBA8 PNGs as written by the capture (or any non-interlaced RGBA8)."""
    data = open(path, 'rb').read()
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', body[:10])
            if depth != 8 or color != 6:
                raise ValueError('not RGBA8')
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = row[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1: row[x] = (row[x] + a) & 255
            elif f == 2: row[x] = (row[x] + b) & 255
            elif f == 3: row[x] = (row[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(row)
        prev = row
    return w, h, rows


def write_png(path, w, h, rgb):
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    cell = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    names = sorted(n for n in os.listdir(src) if n.endswith('.png'))
    if not names:
        sys.exit('no previews in ' + src)
    cols = max(1, int(len(names) ** 0.5 + 0.999))
    rows_n = (len(names) + cols - 1) // cols
    pad = 4
    W, H = cols * (cell + pad), rows_n * (cell + pad)
    sheet = bytearray(b'\x30' * (W * H * 3))
    index = []
    for i, name in enumerate(names):
        w, h, rows = read_png(os.path.join(src, name))
        scale = max(w / cell, h / cell, 1)
        tw, th = max(1, int(w / scale)), max(1, int(h / scale))
        ox, oy = (i % cols) * (cell + pad), (i // cols) * (cell + pad)
        for y in range(th):
            row = rows[min(h - 1, int(y * scale))]
            for x in range(tw):
                sx = min(w - 1, int(x * scale)) * 4
                r, g, b, a = row[sx:sx + 4]
                bg = 200 if ((x // 8) + (y // 8)) & 1 else 140
                o = ((oy + y) * W + ox + x) * 3
                sheet[o:o + 3] = bytes((r * a + bg * (255 - a)) // 255 for r in (r, g, b))
        index.append(f'{i // cols:3} {i % cols:3}  {name}')
    write_png(out, W, H, sheet)
    with open(out + '.txt', 'w') as f:
        f.write(f'# row col file ({cols} columns, {cell}px cells)\n' + '\n'.join(index) + '\n')
    print(f'{out}: {len(names)} textures, {cols}x{rows_n}')


if __name__ == '__main__':
    main()
