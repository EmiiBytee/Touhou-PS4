#!/usr/bin/env python3
"""Dumps the texture sheets of a TH08 ANM file as PNG, with their sprite rectangles.

The ANM must already be decompressed and decrypted, as the game receives it: extract it from
th08.dat with thtk (`thdat -x 8 th08.dat title01.anm`), or run the reconstruction's Linux
reference build with TH08_DUMP_DIR set to get every archive entry in that form.

usage: th08_anm_dump.py <file.anm> <out dir>
"""
import os
import struct
import sys
import zlib

ENTRY_SIZE = 0x40


def write_png(path, width, height, rgba):
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def decode(fmt, width, height, data):
    """THTX pixels to RGBA bytes. Formats follow g_TextureFormatD3D8Mapping."""
    out = bytearray(width * height * 4)
    if fmt == 1:  # A8R8G8B8, stored B G R A
        for i in range(width * height):
            b, g, r, a = data[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    elif fmt == 5:  # A4R4G4B4
        for i in range(width * height):
            (p,) = struct.unpack_from("<H", data, i * 2)
            out[i * 4:i * 4 + 4] = bytes((((p >> 8) & 15) * 17, ((p >> 4) & 15) * 17,
                                          (p & 15) * 17, ((p >> 12) & 15) * 17))
    elif fmt == 3:  # R5G6B5
        for i in range(width * height):
            (p,) = struct.unpack_from("<H", data, i * 2)
            out[i * 4:i * 4 + 4] = bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                                          (p & 31) * 255 // 31, 255))
    elif fmt == 2:  # A1R5G5B5
        for i in range(width * height):
            (p,) = struct.unpack_from("<H", data, i * 2)
            out[i * 4:i * 4 + 4] = bytes((((p >> 10) & 31) * 255 // 31, ((p >> 5) & 31) * 255 // 31,
                                          (p & 31) * 255 // 31, 255 if p & 0x8000 else 0))
    else:
        raise ValueError("unsupported THTX format %d" % fmt)
    return bytes(out)


def main():
    path, out_dir = sys.argv[1:3]
    os.makedirs(out_dir, exist_ok=True)
    data = open(path, "rb").read()
    offset = 0
    index = 0
    while True:
        (num_sprites, num_scripts, texture_idx, width, height, fmt, color_key, name_off,
         _sprite_idx_off, _mip_off, version, _priority, texture_off, has_data, next_off) = \
            struct.unpack_from("<iiIiiIIIIIIIIB3xI", data, offset)
        name = data[offset + name_off:data.index(b"\x00", offset + name_off)].decode("ascii", "replace")
        print("entry %d: %s  %dx%d fmt=%d sprites=%d scripts=%d%s" %
              (index, name, width, height, fmt, num_sprites, num_scripts, "" if has_data else " (external)"))
        for s in range(num_sprites):
            (sprite_off,) = struct.unpack_from("<I", data, offset + ENTRY_SIZE + s * 4)
            sid, x, y, w, h = struct.unpack_from("<Iffff", data, offset + sprite_off)
            print("  sprite %3d: x=%g y=%g w=%g h=%g" % (sid, x, y, w, h))
        if has_data:
            tex = offset + texture_off
            magic = data[tex:tex + 4]
            _r, tfmt, tw, th = struct.unpack_from("<Hhhh", data, tex + 4)
            if magic == b"THTX":
                bpp = {1: 4, 2: 2, 3: 2, 4: 3, 5: 2}.get(tfmt, 4)
                pixels = data[tex + 16:tex + 16 + tw * th * bpp]
                png = os.path.join(out_dir, "%02d_%s.png" % (index, os.path.basename(name)))
                write_png(png, tw, th, decode(tfmt, tw, th, pixels))
                print("  -> %s (%dx%d fmt=%d)" % (png, tw, th, tfmt))
        if next_off == 0:
            break
        offset += next_off
        index += 1


if __name__ == "__main__":
    main()
