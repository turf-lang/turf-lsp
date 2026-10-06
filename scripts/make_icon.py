#!/usr/bin/env python3
"""Draws editors/vscode/media/icon.png: a white "T" on a green rounded square.

Only the standard library (zlib, struct), so it runs anywhere the rest of the
build does. The PNG is committed; this is how it was made, so it can be changed
without an image editor.

    scripts/make_icon.py
"""

import pathlib
import struct
import zlib

SIZE = 128
OUT = pathlib.Path(__file__).resolve().parent.parent / "editors" / "vscode" / "media" / "icon.png"
BACKGROUND = (46, 125, 50)   # green
FOREGROUND = (255, 255, 255)


def inside_rounded_square(x, y, radius=24):
    cx = min(max(x, radius), SIZE - 1 - radius)
    cy = min(max(y, radius), SIZE - 1 - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def inside_t(x, y):
    bar = 28 <= y < 48 and 26 <= x < 102          # the top bar
    stem = 52 <= x < 76 and 48 <= y < 106          # the stem
    return bar or stem


def pixel(x, y):
    if not inside_rounded_square(x, y):
        return (0, 0, 0, 0)
    return (*(FOREGROUND if inside_t(x, y) else BACKGROUND), 255)


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(SIZE)) for y in range(SIZE))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(rows, 9))
       + chunk(b"IEND", b""))
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(png)
print("wrote %s (%d bytes)" % (OUT, len(png)))
