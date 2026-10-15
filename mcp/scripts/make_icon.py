#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draw mcpb/icon.png (512x512, the size Claude Desktop recommends) with the standard library
only: a blueprint-blue tile with a drawing grid, a circle with center lines and a snap marker.
Anti-aliased with signed distances.

    python scripts/make_icon.py            # writes mcpb/icon.png next to this script's folder
"""
from __future__ import annotations

import math
import struct
import sys
import zlib
from pathlib import Path

SIZE = 512
K = SIZE / 256  # the design below is in 256-px units
BLUE = (31, 95, 168)
WHITE = (255, 255, 255)
ORANGE = (255, 153, 0)


def rounded_box(x, y, cx, cy, half, r):
    qx, qy = abs(x - cx) - (half - r), abs(y - cy) - (half - r)
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def segment(x, y, ax, ay, bx, by, width):
    px, py, ux, uy = x - ax, y - ay, bx - ax, by - ay
    t = max(0.0, min(1.0, (px * ux + py * uy) / (ux * ux + uy * uy)))
    return math.hypot(px - t * ux, py - t * uy) - width / 2


def ring(x, y, cx, cy, radius, width):
    return abs(math.hypot(x - cx, y - cy) - radius) - width / 2


def pixel(px: float, py: float) -> tuple[int, int, int, int]:
    # Distances are computed in 256-px design units and scaled back to device pixels.
    x, y = px / K, py / K

    def coverage(d: float) -> float:
        """Pixel coverage from a signed distance (negative inside), one device pixel of anti-aliasing."""
        return max(0.0, min(1.0, 0.5 - d * K))

    tile = coverage(rounded_box(x, y, 128, 128, 120, 44))
    if tile <= 0:
        return (0, 0, 0, 0)
    color = list(BLUE)

    def paint(rgb, alpha):
        for i in range(3):
            color[i] = color[i] * (1 - alpha) + rgb[i] * alpha

    # drawing grid every 32 design px
    for g in range(32, 256, 32):
        paint(WHITE, 0.14 * coverage(abs(x - g) - 0.6 / K))
        paint(WHITE, 0.14 * coverage(abs(y - g) - 0.6 / K))
    # center lines through the circle, a circle, and a snap marker on it
    paint(WHITE, 0.9 * coverage(segment(x, y, 46, 128, 210, 128, 5)))
    paint(WHITE, 0.9 * coverage(segment(x, y, 128, 46, 128, 210, 5)))
    paint(WHITE, coverage(ring(x, y, 128, 128, 62, 13)))
    sx, sy = 128 + 62 * math.cos(math.radians(45)), 128 - 62 * math.sin(math.radians(45))
    paint(ORANGE, coverage(rounded_box(x, y, sx, sy, 11, 2)))
    paint(BLUE, coverage(rounded_box(x, y, sx, sy, 5, 1)))
    return (round(color[0]), round(color[1]), round(color[2]), round(255 * tile))


def png(width: int, height: int, rows: list[bytes]) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main() -> int:
    rows = [b"".join(bytes(pixel(x + 0.5, y + 0.5)) for x in range(SIZE)) for y in range(SIZE)]
    out = Path(__file__).resolve().parent.parent / "mcpb" / "icon.png"
    out.write_bytes(png(SIZE, SIZE, rows))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
