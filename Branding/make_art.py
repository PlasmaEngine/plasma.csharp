"""Renders the icon and banner for plasma.csharp.

There is no rasteriser on this machine and no Pillow, so this writes PNG bytes directly - the
format is just zlib-compressed scanlines with a filter byte each. Everything is drawn at 3x and
box-downsampled, which is where the antialiasing comes from.

The mark and palette come from EditorPluginCSharp/QtResources/CSharp.svg so the package artwork and
the in-editor iconography agree: a purple rounded square carrying a white C and a sharp. The glyphs
are drawn as geometry rather than traced from the SVG path data - a C is an annulus with a gap, a
sharp is four slanted bars - which is close enough at these sizes and needs no path rasteriser.
"""

import math
import os
import struct
import sys
import zlib

SS = 3  # supersample factor

# ---- palette, from CSharp.svg ----------------------------------------------
PURPLE = (0x6F, 0x43, 0xC0)
PURPLE_DEEP = (0x3C, 0x21, 0x70)
PURPLE_LIFT = (0x8B, 0x5F, 0xD8)
WHITE = (0xFF, 0xFF, 0xFF)


def write_png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c0, c1, t):
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return (lerp(c0[0], c1[0], t), lerp(c0[1], c1[1], t), lerp(c0[2], c1[2], t))


def rounded_rect(x, y, w, h, radius):
    cx = min(max(x, radius), w - radius)
    cy = min(max(y, radius), h - radius)
    dx, dy = x - cx, y - cy
    if dx == 0 and dy == 0:
        return True
    return (dx * dx + dy * dy) <= radius * radius


def in_c_glyph(x, y, cx, cy, outer, thickness):
    """The C: an annulus with a wedge removed from the right, like the letterform's opening."""
    dx, dy = x - cx, y - cy
    d = math.hypot(dx, dy)
    if d > outer or d < outer - thickness:
        return False

    # Opening faces right. atan2 is 0 along +x, so the gap is a band around that.
    angle = math.degrees(math.atan2(dy, dx))
    return abs(angle) > 38.0


def in_sharp_glyph(x, y, cx, cy, size, bar):
    """The sharp: two leaning verticals crossed by two rising horizontals."""
    half = size * 0.5
    if abs(x - cx) > half * 1.25 or abs(y - cy) > half * 1.25:
        return False

    ry = (y - cy) / half  # -1 .. 1 within the glyph

    # Verticals lean right as they rise, matching the mark's italic feel.
    for offset in (-size * 0.22, size * 0.22):
        if abs((x - cx) - offset + ry * size * 0.10) <= bar * 0.5:
            return True

    # Horizontals rise slightly to the right.
    rx = (x - cx) / half
    for offset in (-size * 0.20, size * 0.20):
        if abs((y - cy) - offset + rx * size * 0.07) <= bar * 0.5:
            return True

    return False


def downsample(buf, w, h):
    out = bytearray(w * h * 4)
    n = SS * SS
    bw = w * SS
    for y in range(h):
        for x in range(w):
            r = g = b = a = 0
            for sy in range(SS):
                row = (y * SS + sy) * bw
                for sx in range(SS):
                    i = (row + x * SS + sx) * 4
                    r += buf[i]; g += buf[i + 1]; b += buf[i + 2]; a += buf[i + 3]
            o = (y * w + x) * 4
            out[o] = r // n; out[o + 1] = g // n; out[o + 2] = b // n; out[o + 3] = a // n
    return out


def render_icon(size):
    W = H = size * SS
    buf = bytearray(W * H * 4)
    radius = W * 0.20

    c_cx, c_cy = W * 0.36, H * 0.52
    c_outer, c_thick = H * 0.20, H * 0.075

    s_cx, s_cy = W * 0.68, H * 0.52
    s_size, s_bar = H * 0.30, H * 0.045

    for y in range(H):
        yt = y / float(H - 1)
        bg = mix(PURPLE_LIFT, PURPLE_DEEP, yt)
        for x in range(W):
            if not rounded_rect(x + 0.5, y + 0.5, W, H, radius):
                continue

            col = bg
            if in_c_glyph(x + 0.5, y + 0.5, c_cx, c_cy, c_outer, c_thick):
                col = WHITE
            elif in_sharp_glyph(x + 0.5, y + 0.5, s_cx, s_cy, s_size, s_bar):
                col = WHITE

            i = (y * W + x) * 4
            buf[i] = int(col[0]); buf[i + 1] = int(col[1]); buf[i + 2] = int(col[2]); buf[i + 3] = 255

    return downsample(buf, size, size)


def render_banner(width, height):
    W, H = width * SS, height * SS
    buf = bytearray(W * H * 4)

    c_cx, c_cy = W * 0.42, H * 0.50
    c_outer, c_thick = H * 0.26, H * 0.095

    s_cx, s_cy = W * 0.58, H * 0.50
    s_size, s_bar = H * 0.38, H * 0.058

    # Diagonal wash so the flat purple does not read as a solid block at banner size.
    for y in range(H):
        for x in range(W):
            t = (x / float(W - 1)) * 0.45 + (y / float(H - 1)) * 0.55
            col = mix(PURPLE_LIFT, PURPLE_DEEP, t)

            if in_c_glyph(x + 0.5, y + 0.5, c_cx, c_cy, c_outer, c_thick):
                col = WHITE
            elif in_sharp_glyph(x + 0.5, y + 0.5, s_cx, s_cy, s_size, s_bar):
                col = WHITE

            i = (y * W + x) * 4
            buf[i] = int(col[0]); buf[i + 1] = int(col[1]); buf[i + 2] = int(col[2]); buf[i + 3] = 255

    return downsample(buf, width, height)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    write_png(os.path.join(out, "CSharpIcon.png"), 512, 512, render_icon(512))
    print("CSharpIcon.png   512x512")

    write_png(os.path.join(out, "CSharpBanner.png"), 1100, 512, render_banner(1100, 512))
    print("CSharpBanner.png 1100x512")


if __name__ == "__main__":
    main()
