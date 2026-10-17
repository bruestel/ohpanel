#!/usr/bin/env python3
"""A station logo as the panel draws it: a picture turned into a short text
code for the station catalogue's `ohpanel` metadata (images="name=code,...",
docs/panel-config.md). The panel only draws what the code holds -- filled
rectangles, in a palette whose colours may be translucent (laid on the
tile's colour by the panel) -- so everything else happens here: the
background taken away, the logo cut out and scaled down, its colours reduced.

  tools/station_logo.py logo.png                   # a logo on a plain background
  tools/station_logo.py logo.jpg --circle          # a round logo: the circle cut out
  tools/station_logo.py logo.png --disc "#1d3fbb"  # a one-colour logo, white on a disc
  tools/station_logo.py logo.png --keep            # the picture as it is
  ... --preview out.png                            # how it looks on the tiles

The code (base64url, no padding) of the bytes:
  1, width, height, colours,
  per colour: RGB565 (little-endian), alpha (0..255),
  per rectangle: x, y, width, height, colour index.
Rectangles not drawn (transparent) are left out; at most 64 x 64 pixels.
"""

import argparse
import base64
import sys
from collections import deque

from PIL import Image, ImageDraw

SIZE = 56          # the logo's box on the panel (boards/nspanel/nextion/ohpanel_hmi.py LOGO_BOX)
SUPER = 4          # supersampling for drawn shapes (circle, disc)


def corner_colour(im: Image.Image) -> tuple:
    w, h = im.size
    px = im.load()
    corners = [px[0, 0], px[w - 1, 0], px[0, h - 1], px[w - 1, h - 1]]
    return tuple(sorted(c[k] for c in corners)[1] for k in range(3))


def without_background(im: Image.Image, tolerance: int) -> Image.Image:
    """The background (the corners' colour, reached from the edges) taken
    away; the pixels along it unmixed from it, so edges stay smooth."""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    bg = corner_colour(im)
    distance = lambda p: max(abs(p[k] - bg[k]) for k in range(3))
    gone = bytearray(w * h)
    todo = deque([(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)])
    while todo:
        x, y = todo.popleft()
        if not (0 <= x < w and 0 <= y < h) or gone[y * w + x] or distance(px[x, y]) > tolerance:
            continue
        gone[y * w + x] = 1
        todo.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    out = im.copy()
    op = out.load()
    for y in range(h):
        for x in range(w):
            if gone[y * w + x]:
                op[x, y] = (0, 0, 0, 0)
                continue
            near = any(0 <= x + dx < w and 0 <= y + dy < h and gone[(y + dy) * w + x + dx]
                       for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            if not near:
                continue
            # an edge pixel: how much of it is the background (as far as one can tell)
            p = px[x, y]
            room = max(max(bg[k], 255 - bg[k]) for k in range(3))
            a = min(1.0, max(abs(p[k] - bg[k]) for k in range(3)) / max(1, room) * 1.6)
            if a <= 0.02:
                op[x, y] = (0, 0, 0, 0)
                continue
            colour = tuple(max(0, min(255, round(bg[k] + (p[k] - bg[k]) / a))) for k in range(3))
            op[x, y] = colour + (round(a * p[3]),)
    return out


def circle_mask(size: tuple) -> Image.Image:
    w, h = size
    big = Image.new("L", (w * SUPER, h * SUPER), 0)
    ImageDraw.Draw(big).ellipse((0, 0, w * SUPER - 1, h * SUPER - 1), fill=255)
    return big.resize(size, Image.LANCZOS)


def cut_circle(im: Image.Image, tolerance: int) -> Image.Image:
    """A round logo: the circle the logo fills, everything outside it gone."""
    im = im.convert("RGBA")
    bg = corner_colour(im)
    px = im.load()
    w, h = im.size
    inside = [(x, y) for y in range(0, h, 2) for x in range(0, w, 2)
              if max(abs(px[x, y][k] - bg[k]) for k in range(3)) > tolerance]
    x0, x1 = min(p[0] for p in inside), max(p[0] for p in inside)
    y0, y1 = min(p[1] for p in inside), max(p[1] for p in inside)
    side = max(x1 - x0, y1 - y0) + 1
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    box = (round(cx - side / 2), round(cy - side / 2), round(cx - side / 2) + side, round(cy - side / 2) + side)
    out = im.crop(box)
    out.putalpha(circle_mask(out.size))
    return out


def on_disc(im: Image.Image, disc: tuple, tolerance: int) -> Image.Image:
    """A one-colour logo (dark on light) in white on a disc of `disc`."""
    logo = without_background(im, tolerance)
    logo = logo.crop(logo.getbbox())
    grey = logo.convert("LA")
    mask = Image.new("L", logo.size, 0)
    mp, gp = mask.load(), grey.load()
    for y in range(logo.height):
        for x in range(logo.width):
            lum, a = gp[x, y]
            mp[x, y] = round((255 - lum) * a / 255)
    side = round(max(logo.size) / 0.62)
    out = Image.new("RGBA", (side, side), disc + (0,))
    out.putalpha(circle_mask((side, side)))
    white = Image.new("RGBA", logo.size, (255, 255, 255, 255))
    white.putalpha(mask)
    out.alpha_composite(white, ((side - logo.width) // 2, (side - logo.height) // 2))
    return out


def rgb565(c: tuple) -> int:
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def encode(im: Image.Image, size: int, colours: int) -> tuple:
    """The code, and the logo as the panel will draw it (for the preview)."""
    im = im.crop(im.getbbox())
    im.thumbnail((size, size), Image.LANCZOS)
    w, h = im.size
    # alpha in few steps: fewer colours, fewer rectangles; nearly opaque is opaque
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            a = 0 if a < 40 else 255 if a > 215 else round(a / 64) * 64
            px[x, y] = (r, g, b, a) if a else (0, 0, 0, 0)
    q = im.quantize(colours, method=Image.FASTOCTREE, dither=Image.NONE)
    shown = q.convert("RGBA")
    idx, sp = q.load(), shown.load()
    palette, index = [], {}
    rects, open_rects = [], {}
    for y in range(h):
        x = 0
        while x < w:
            i, end = idx[x, y], x
            while end < w and idx[end, y] == i:
                end += 1
            colour = sp[x, y]
            if colour[3] >= 40:
                if i not in index:
                    index[i] = len(palette)
                    palette.append(colour)
                key = (x, end - x, index[i])
                r = open_rects.get(key)
                if r and r[1] + r[3] == y:
                    r[3] += 1
                else:
                    r = [x, y, end - x, 1, index[i]]
                    open_rects[key] = r
                    rects.append(r)
            x = end
    data = bytearray([1, w, h, len(palette)])
    for c in palette:
        v = rgb565(c)
        data += bytes([v & 0xFF, v >> 8, c[3]])
    for r in rects:
        data += bytes(r)
    code = base64.urlsafe_b64encode(bytes(data)).decode().rstrip("=")
    return code, palette, rects, (w, h)


def preview(path: str, palette: list, rects: list, size: tuple, logos: int) -> None:
    """The logo on an inactive and an active tile and in the art box, twice as large."""
    tiles = [(44, 44, 44), (255, 255, 255), (44, 44, 44)]
    sheet = Image.new("RGB", (len(tiles) * 100 + 20, 120), (20, 20, 20))
    for n, bg in enumerate(tiles):
        tile = Image.new("RGB", (96, 96), bg)
        ox, oy = (96 - size[0]) // 2, (96 - size[1]) // 2
        for x, y, w, h, i in rects:
            c = palette[i]
            a = c[3] / 255
            mix = tuple(round(c[k] * a + bg[k] * (1 - a)) for k in range(3))
            ImageDraw.Draw(tile).rectangle((ox + x, oy + y, ox + x + w - 1, oy + y + h - 1), fill=mix)
        sheet.paste(tile, (10 + n * 100, 12))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    shape = ap.add_mutually_exclusive_group()
    shape.add_argument("--circle", action="store_true", help="a round logo: cut out its circle")
    shape.add_argument("--disc", metavar="COLOUR", help="a one-colour logo: white on a disc of this colour (#rrggbb)")
    shape.add_argument("--keep", action="store_true", help="keep the background")
    ap.add_argument("--tolerance", type=int, default=40, help="how far from the background colour still counts as it")
    ap.add_argument("--size", type=int, default=SIZE, help=f"the longer side in pixels (default {SIZE}, at most 64)")
    ap.add_argument("--colours", type=int, default=16, help="at most this many colours (default 16)")
    ap.add_argument("--preview", metavar="PNG", help="write how it looks on the panel")
    ap.add_argument("--save", metavar="PNG", help="write the prepared logo (before scaling)")
    a = ap.parse_args()

    im = Image.open(a.image).convert("RGBA")
    if a.circle:
        im = cut_circle(im, a.tolerance)
    elif a.disc:
        h = a.disc.lstrip("#")
        im = on_disc(im, tuple(int(h[k:k + 2], 16) for k in (0, 2, 4)), a.tolerance)
    elif not a.keep:
        im = without_background(im, a.tolerance)
    if a.save:
        im.save(a.save)
    code, palette, rects, size = encode(im, min(64, a.size), max(2, min(64, a.colours)))
    if a.preview:
        preview(a.preview, palette, rects, size, 1)
    print(code)
    print(f"{size[0]}x{size[1]} px, {len(palette)} colours, {len(rects)} rectangles, {len(code)} characters",
          file=sys.stderr)


if __name__ == "__main__":
    main()
