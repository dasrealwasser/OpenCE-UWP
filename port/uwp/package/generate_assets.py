#!/usr/bin/env python3
"""Generates the UWP visual assets (Square44x44Logo.png, Square150x150Logo.png,
Wide310x150Logo.png, SplashScreen.png, StoreLogo.png) into an output directory
using only Python's standard library (zlib + struct), compositing the project's
icon (port/assets/icon/opence-icon-256.png) centered on a dark #0e121a canvas.
"""

import math
import struct
import sys
import zlib
from pathlib import Path
from typing import Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent.parent.parent
ICON_PATH = ROOT / "port/assets/icon/opence-icon-256.png"

ASSETS = (
    ("Square44x44Logo.png", 44, 44, 38),
    ("StoreLogo.png", 50, 50, 42),
    ("Square150x150Logo.png", 150, 150, 112),
    ("Wide310x150Logo.png", 310, 150, 112),
    ("SplashScreen.png", 620, 300, 192),
)

BG_COLOR = (14, 18, 26, 255)


def decode_rgba_png(path: Path) -> Optional[Tuple[int, int, bytearray]]:
    """Decodes an 8-bit RGBA or RGB non-interlaced PNG using stdlib zlib."""
    if not path.is_file():
        return None
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos = 8
    width = height = bit_depth = color_type = interlace = 0
    idat = bytearray()
    while pos + 8 <= len(data):
        length, chunk_type = struct.unpack(">I4s", data[pos:pos + 8])
        chunk_data = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk_data[:13])
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0 or width <= 0 or height <= 0:
        return None
    bpp = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * bpp
    rgba = bytearray(width * height * 4)
    prev_row = bytearray(stride)
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        row = bytearray(raw[offset:offset + stride])
        offset += stride
        if filter_type == 1:  # Sub
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                row[i] = (row[i] + prev_row[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up = prev_row[i]
                row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = prev_row[i]
                c = prev_row[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        prev_row = row
        dst_row = y * width * 4
        if bpp == 4:
            rgba[dst_row:dst_row + width * 4] = row
        else:
            for x in range(width):
                rgba[dst_row + x * 4:dst_row + x * 4 + 4] = bytes((row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255))
    return width, height, rgba


def encode_rgba_png(width: int, height: int, rgba: bytes) -> bytes:
    """Encodes an 8-bit RGBA image buffer into a valid PNG byte string."""
    def make_chunk(chunk_type: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(chunk_type + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + chunk_type + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    compressed = zlib.compress(bytes(raw), 9)
    return b"\x89PNG\r\n\x1a\n" + make_chunk(b"IHDR", ihdr) + make_chunk(b"IDAT", compressed) + make_chunk(b"IEND", b"")


def sample_bilinear(src: Tuple[int, int, bytearray], u: float, v: float) -> Tuple[int, int, int, int]:
    sw, sh, sdata = src
    fx = max(0.0, min(sw - 1.0, u * (sw - 1)))
    fy = max(0.0, min(sh - 1.0, v * (sh - 1)))
    x0, y0 = int(fx), int(fy)
    x1, y1 = min(sw - 1, x0 + 1), min(sh - 1, y0 + 1)
    tx, ty = fx - x0, fy - y0
    out = [0, 0, 0, 0]
    for c in range(4):
        c00 = sdata[(y0 * sw + x0) * 4 + c]
        c10 = sdata[(y0 * sw + x1) * 4 + c]
        c01 = sdata[(y1 * sw + x0) * 4 + c]
        c11 = sdata[(y1 * sw + x1) * 4 + c]
        top = c00 * (1.0 - tx) + c10 * tx
        bot = c01 * (1.0 - tx) + c11 * tx
        out[c] = int(round(top * (1.0 - ty) + bot * ty))
    return out[0], out[1], out[2], out[3]


def render_tile(width: int, height: int, icon_size: int, src: Optional[Tuple[int, int, bytearray]]) -> bytes:
    buf = bytearray(width * height * 4)
    for i in range(width * height):
        buf[i * 4:i * 4 + 4] = bytes(BG_COLOR)

    ox = (width - icon_size) // 2
    oy = (height - icon_size) // 2
    for iy in range(icon_size):
        dy = oy + iy
        if dy < 0 or dy >= height:
            continue
        for ix in range(icon_size):
            dx = ox + ix
            if dx < 0 or dx >= width:
                continue
            if src is not None:
                r, g, b, a = sample_bilinear(src, (ix + 0.5) / icon_size, (iy + 0.5) / icon_size)
            else:
                nx = (ix + 0.5) / icon_size * 2.0 - 1.0
                ny = (iy + 0.5) / icon_size * 2.0 - 1.0
                dist = math.hypot(nx, ny)
                if 0.58 <= dist <= 0.82:
                    r, g, b, a = 110, 200, 125, 255
                else:
                    r, g, b, a = 0, 0, 0, 0
            if a > 0:
                idx = (dy * width + dx) * 4
                alpha = a / 255.0
                buf[idx + 0] = int(round(r * alpha + BG_COLOR[0] * (1.0 - alpha)))
                buf[idx + 1] = int(round(g * alpha + BG_COLOR[1] * (1.0 - alpha)))
                buf[idx + 2] = int(round(b * alpha + BG_COLOR[2] * (1.0 - alpha)))
                buf[idx + 3] = 255
    return encode_rgba_png(width, height, bytes(buf))


def generate_uwp_assets(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    src = decode_rgba_png(ICON_PATH)
    for filename, width, height, icon_size in ASSETS:
        (output_dir / filename).write_bytes(render_tile(width, height, icon_size, src))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} OUTPUT_ASSETS_DIR")
    generate_uwp_assets(Path(sys.argv[1]))
