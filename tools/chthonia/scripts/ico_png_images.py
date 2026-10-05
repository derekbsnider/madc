#!/usr/bin/env python3
"""ico_png_images.py — a Windows .ico's images as PNG files in the freedesktop
hicolor layout: <outdir>/<w>x<h>/apps/<name>.png, one per image.

    python3 scripts/ico_png_images.py <file.ico> <outdir> <name>

The .ico is the one source of an application's icon artwork (the PE writer
lays it out as .rsrc for Windows: madc_pe_icon.cpp); Linux packaging takes
the same images through this script, so the two platforms never carry
diverging copies. Each image converts losslessly: a 32-bit DIB (BGRA,
bottom-up rows; the AND mask supplies transparency only when every alpha
byte is zero) or an embedded PNG (copied as is). Other bit depths are
refused. Output is deterministic (zlib level 9, no timestamps).
"""
import os
import struct
import sys
import zlib


def png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def encode_png(width, height, rgba_rows):
    raw = b"".join(b"\x00" + row for row in rgba_rows)
    return (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(raw, 9))
            + png_chunk(b"IEND", b""))


def dib_to_png(img, width, height, where):
    hdr_size, dib_w, dib_h, _planes, bpp, compression = struct.unpack("<IiiHHI", img[:20])
    if bpp != 32 or compression != 0:
        sys.exit("ico_png_images: %s is a %d-bit DIB (compression %d); only 32-bit BI_RGB images convert"
                 % (where, bpp, compression))
    if dib_w != width or abs(dib_h) != 2 * height:
        sys.exit("ico_png_images: %s's DIB header says %dx%d, its directory entry %dx%d"
                 % (where, dib_w, abs(dib_h) // 2, width, height))
    stride = width * 4
    pixels = img[hdr_size:hdr_size + stride * height]
    mask_stride = ((width + 31) // 32) * 4
    mask = img[hdr_size + stride * height:hdr_size + stride * height + mask_stride * height]
    if len(pixels) != stride * height:
        sys.exit("ico_png_images: %s's pixel data runs past the image" % where)
    use_mask = all(pixels[i] == 0 for i in range(3, len(pixels), 4))
    rows = []
    for y in range(height):
        src = height - 1 - y          # DIB rows are stored bottom-up
        row = bytearray(stride)
        for x in range(width):
            b, g, r, a = pixels[src * stride + x * 4:src * stride + x * 4 + 4]
            if use_mask:
                bit = mask[src * mask_stride + x // 8] >> (7 - x % 8) & 1
                a = 0 if bit else 255
            row[x * 4:x * 4 + 4] = bytes((r, g, b, a))
        rows.append(bytes(row))
    return encode_png(width, height, rows)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: ico_png_images.py <file.ico> <outdir> <name>")
    path, outdir, name = sys.argv[1:]
    data = open(path, "rb").read()
    reserved, kind, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or kind != 1 or count == 0:
        sys.exit("ico_png_images: %s is not an icon file with images" % path)
    for i in range(count):
        entry = data[6 + 16 * i:22 + 16 * i]
        if len(entry) != 16:
            sys.exit("ico_png_images: %s's directory runs past its end" % path)
        w, h, _colors, _res, _planes, _bpp, size, offset = struct.unpack("<BBBBHHII", entry)
        w, h = w or 256, h or 256
        img = data[offset:offset + size]
        where = "%s image %d" % (path, i + 1)
        if len(img) != size:
            sys.exit("ico_png_images: %s lies outside the icon file" % where)
        png = img if img[:8] == b"\x89PNG\r\n\x1a\n" else dib_to_png(img, w, h, where)
        target = os.path.join(outdir, "%dx%d" % (w, h), "apps")
        os.makedirs(target, exist_ok=True)
        with open(os.path.join(target, name + ".png"), "wb") as out:
            out.write(png)


if __name__ == "__main__":
    main()
