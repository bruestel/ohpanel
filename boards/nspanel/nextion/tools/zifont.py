#!/usr/bin/env python3
"""Nextion .zi fonts: read them and make them from TrueType fonts.

The format (version 6, as the Nextion Editor's font generator writes it for
utf-8; reverse engineered in github.com/hagronnestad/nextion-font-editor,
Docs/Nextion Font Format Specification ZI version 5/6):

    0x00  04 FF 00 0A      signature, no skipped bytes, orientation
    0x04  u8  encoding     0x18 utf-8
    0x05  u8  multi-byte   2: a subset of the code points
    0x06  u8  width        0: variable
    0x07  u8  height       px, the same for every character
    0x08  FF FF 00 FF      code page range (none for a subset)
    0x0C  u32 characters
    0x10  u8  version      6
    0x11  u8  length of the description (name + "utf-8")
    0x14  u32 length of everything after the header
    0x18  u32 44           where the description starts
    0x1C  FF 00            no skipped high bytes
    0x1E  u8  anti-aliased
    0x1F  u8  variable width
    0x20  u8  length of the name
    0x24  u32 characters
    0x2C  the description, then the character map: 10 bytes a character,
          sorted by code -- code (u16), advance width, overhang left and
          right (u8 each; the bitmap is left + width + right px wide and
          drawn that far left of the pen), offset of the data from the map's
          start (u24), length of the data (u16) -- then the data.

A character's data is its bitmap, row by row, run-length coded. The first
byte is 0x03 (3-bit alpha) or 0x01 (no alpha), then one byte per run, the top
bits the kind:

    000nnnnn  n transparent        001nnnnn  n opaque
    010nnnnn  n transparent, 1 opaque
    011nnnnn  n transparent, 2 opaque
    with 0x03:  10nnnaaa  n transparent, 1 pixel of alpha a (0..7)
                11aaabbb  2 pixels of alpha a and b
    with 0x01:  100nnnnn  n transparent, 3 opaque
                101nnnnn  n transparent, 4 opaque
                11nnnmmm  n transparent, m opaque

Usage (looks at the fonts of a project):
    zifont.py <project.HMI> [font id] [characters]
"""

import struct
import sys

HEADER = 44
ENCODING = b"utf-8"


# --- reading ---------------------------------------------------------------

def read(data: bytes) -> dict:
    """A .zi font: name, height and code -> (width, left, right, data)."""
    count = struct.unpack_from("<I", data, 0x0C)[0]
    name = data[HEADER:HEADER + data[0x20]].decode("latin1")
    table = HEADER + data[0x11]
    chars = {}
    for i in range(count):
        at = table + 10 * i
        code, width, left, right = struct.unpack_from("<HBBB", data, at)
        offset = int.from_bytes(data[at + 5:at + 8], "little")
        length = struct.unpack_from("<H", data, at + 8)[0]
        chars[code] = (width, left, right, data[table + offset:table + offset + length])
    return {"name": name, "height": data[7], "chars": chars}


def metrics(data: bytes, first: int, last: int) -> dict:
    """What fitting a text needs of the characters first..last: the advance
    widths, and the blank columns inside them before and after the ink
    ("lead", "trail"), so a text whose first and last characters have room to
    spare there still counts as fitting. A character the font lacks counts
    as "?" -- none at all in a font without text (icons only, no "?" either): an empty list."""
    font = read(data)
    out = {"first": first, "widths": [], "lead": [], "trail": []}
    if ord("?") not in font["chars"]:
        return out
    height = font["height"]
    for c in range(first, last + 1):
        width, left, right, glyph = font["chars"].get(c, font["chars"][ord("?")])
        bw = left + width + right
        pixels = decode(glyph)
        cols = [x for x in range(bw) if any(pixels[y * bw + x] for y in range(height))]
        lead = max(0, cols[0] - left) if cols else 0
        trail = max(0, left + width - cols[-1] - 1) if cols else 0
        out["widths"].append(width)
        out["lead"].append(min(lead, width))
        out["trail"].append(min(trail, width))
    return out


# The characters whose widths the firmware and the web preview keep to fit a
# text: the letters of the display languages (components/app_display/texts.json)
# and of names -- Latin-1 and Latin Extended-A, Romanian's s and t with comma,
# Greek, Cyrillic. Others count as "?".
FIT_RUNS = ((0x20, 0x17F), (0x218, 0x21B), (0x384, 0x3CE), (0x400, 0x45F))


def fit_metrics(data: bytes) -> dict:
    """metrics() of FIT_RUNS, the runs one after the other in the lists and
    named in "ranges" ([first, count]; Engine::FontWidths)."""
    out = {"first": FIT_RUNS[0][0], "widths": [], "lead": [], "trail": [], "ranges": []}
    for first, last in FIT_RUNS:
        part = metrics(data, first, last)
        if not part["widths"]:   # a font without text
            return part
        for k in ("widths", "lead", "trail"):
            out[k] += part[k]
        out["ranges"].append([first, last - first + 1])
    return out


def cap_rows(data: bytes) -> list[int]:
    """The first and last row of ink of "H" in its cell: where capitals and
    digits stand. Empty for a font without it (icons only)."""
    font = read(data)
    if ord("H") not in font["chars"]:
        return []
    width, left, right, glyph = font["chars"][ord("H")]
    bw, pixels = left + width + right, decode(glyph)
    rows = [y for y in range(font["height"]) if any(pixels[y * bw + x] for x in range(bw))]
    return [rows[0], rows[-1]] if rows else []


def decode(glyph: bytes) -> list[int]:
    """A character's pixels, alpha 0..7, row by row."""
    out, alpha = [], glyph[0] == 3
    for b in glyph[1:]:
        kind, n = b >> 5, b & 31
        if kind == 0:
            out += [0] * n
        elif kind == 1:
            out += [7] * n
        elif kind in (2, 3):
            out += [0] * n + [7] * (kind - 1)
        elif alpha and kind in (4, 5):
            out += [0] * ((b >> 3) & 7) + [b & 7]
        elif alpha:
            out += [(b >> 3) & 7, b & 7]
        elif kind in (4, 5):
            out += [0] * n + [7] * (kind - 1)
        else:
            out += [0] * ((b >> 3) & 7) + [7] * (b & 7)
    return out


# --- writing ---------------------------------------------------------------

def encode(pixels: list[int]) -> bytes:
    """Run-length codes a character's pixels (alpha 0..7) as short as it goes
    (the fewest runs, found by trying every way from each reachable pixel);
    0x01 when they are all either transparent or opaque."""
    alpha = any(0 < p < 7 for p in pixels)
    n = len(pixels)
    zeros, solid = [0] * (n + 1), [0] * (n + 1)   # run of 0 / of 7 starting at each pixel
    for i in range(n - 1, -1, -1):
        zeros[i] = zeros[i + 1] + 1 if pixels[i] == 0 else 0
        solid[i] = solid[i + 1] + 1 if pixels[i] == 7 else 0
    best = [None] * (n + 1)                        # i -> (bytes so far, previous i, code)
    best[0] = (0, -1, 0)
    for i in range(n):
        if best[i] is None:
            continue
        cost = best[i][0] + 1

        def step(j, code):
            if best[j] is None or cost < best[j][0]:
                best[j] = (cost, i, code)

        z, o = zeros[i], solid[i]
        for k in {min(z, 31), z - 31, z - 7}:
            if 0 < k <= 31:
                step(i + k, k)
        for k in {min(o, 31), o - 1, o - 2}:
            if 0 < k <= 31:
                step(i + k, 0x20 + k)
        if z <= 31:
            after = solid[i + z]
            if after >= 1:
                step(i + z + 1, 0x40 + z)
            if after >= 2:
                step(i + z + 2, 0x60 + z)
            if not alpha:
                if after >= 3:
                    step(i + z + 3, 0x80 + z)
                if after >= 4:
                    step(i + z + 4, 0xA0 + z)
        if alpha:
            if z <= 7 and i + z < n:
                step(i + z + 1, 0x80 + (z << 3) + pixels[i + z])
            if i + 1 < n:
                step(i + 2, 0xC0 + (pixels[i] << 3) + pixels[i + 1])
        elif z <= 7:
            after = min(solid[i + z], 7)
            if after:
                step(i + z + after, 0xC0 + (z << 3) + after)
    codes, i = [], n
    while i > 0:
        codes.append(best[i][2])
        i = best[i][1]
    return bytes([3 if alpha else 1] + codes[::-1])


def write(name: str, height: int, chars: dict) -> bytes:
    """A .zi font from code -> (width, left, right, pixels)."""
    desc = name.encode("latin1") + ENCODING
    codes = sorted(chars)
    glyphs = [encode(chars[c][3]) for c in codes]
    table, data, offset = bytearray(), bytearray(), 10 * len(codes)
    for code, glyph in zip(codes, glyphs):
        width, left, right, _ = chars[code]
        table += struct.pack("<HBBB", code, width, left, right) + offset.to_bytes(3, "little") \
            + struct.pack("<H", len(glyph))
        data += glyph
        offset += len(glyph)
    body = desc + table + data
    head = bytearray(HEADER)
    head[0:4] = b"\x04\xff\x00\x0a"
    head[4:8] = bytes([0x18, 2, 0, height])
    head[8:12] = b"\xff\xff\x00\xff"
    struct.pack_into("<I", head, 0x0C, len(codes))
    head[0x10], head[0x11] = 6, len(desc)
    struct.pack_into("<II", head, 0x14, len(body), HEADER)
    head[0x1C:0x21] = bytes([0xFF, 0, 1, 1, len(name)])
    struct.pack_into("<I", head, 0x24, len(codes))
    return bytes(head) + body


# --- TrueType --------------------------------------------------------------

def cmap(path: str) -> set[int]:
    """The code points a TrueType font has a glyph for (its Unicode cmap,
    format 4 or 12)."""
    data = open(path, "rb").read()
    tables = {data[12 + 16 * i:16 + 16 * i]: struct.unpack_from(">I", data, 20 + 16 * i)[0]
              for i in range(struct.unpack_from(">H", data, 4)[0])}
    base = tables[b"cmap"]
    subtables = []
    for i in range(struct.unpack_from(">H", data, base + 2)[0]):
        platform, encoding, offset = struct.unpack_from(">HHI", data, base + 4 + 8 * i)
        fmt = struct.unpack_from(">H", data, base + offset)[0]
        if (platform, encoding) in ((3, 10), (0, 4), (0, 6)) and fmt == 12:
            subtables.insert(0, (12, base + offset))
        elif (platform, encoding) in ((3, 1), (0, 3)) and fmt == 4:
            subtables.append((4, base + offset))
    if not subtables:
        raise ValueError(f"{path}: no Unicode cmap")
    fmt, at = subtables[0]
    codes = set()
    if fmt == 12:
        for g in range(struct.unpack_from(">I", data, at + 12)[0]):
            start, end, glyph = struct.unpack_from(">III", data, at + 16 + 12 * g)
            codes.update(c for c in range(start, end + 1) if glyph + c - start)
        return codes
    segs = struct.unpack_from(">H", data, at + 6)[0] // 2
    ends = struct.unpack_from(f">{segs}H", data, at + 14)
    starts = struct.unpack_from(f">{segs}H", data, at + 16 + 2 * segs)
    deltas = struct.unpack_from(f">{segs}h", data, at + 16 + 4 * segs)
    ranges_at = at + 16 + 6 * segs
    ranges = struct.unpack_from(f">{segs}H", data, ranges_at)
    for s in range(segs):
        for c in range(starts[s], ends[s] + 1):
            if c == 0xFFFF:
                continue
            if ranges[s] == 0:
                glyph = (c + deltas[s]) & 0xFFFF
            else:
                at_glyph = ranges_at + 2 * s + ranges[s] + 2 * (c - starts[s])
                glyph = struct.unpack_from(">H", data, at_glyph)[0]
                glyph = (glyph + deltas[s]) & 0xFFFF if glyph else 0
            if glyph:
                codes.add(c)
    return codes


def render(font, text: str, height: int, baseline: int) -> tuple:
    """One character as the display draws it: (width, left, right, pixels).
    `font` a PIL FreeType font; the pen at x = 0 on the baseline (px from the
    top); ink beyond the advance becomes the overhang, ink above or below the
    cell is cut off."""
    from PIL import Image, ImageDraw
    width = round(font.getlength(text))
    ink = font.getbbox(text, anchor="ls")
    left = max(0, min(255, -ink[0]))
    right = max(0, min(255, ink[2] - width))
    img = Image.new("L", (left + width + right, height))
    ImageDraw.Draw(img).text((left, baseline), text, font=font, fill=255, anchor="ls")
    pixels = [(a * 7 + 127) // 255 for a in img.tobytes()]
    return width, left, right, pixels


def show(chars: dict, height: int, text: str) -> None:
    for c in text:
        width, left, right, glyph = chars[ord(c)]
        pixels = decode(glyph) if isinstance(glyph, bytes) else glyph
        w = left + width + right
        print(f"U+{ord(c):04X} width {width} overhang {left}/{right}")
        for row in range(height):
            print("  " + "".join(" .:-=+*#"[p] for p in pixels[row * w:(row + 1) * w]) + "|")


def main():
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    from hmi2txt import read_container
    import re
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    entries = read_container(open(sys.argv[1], "rb").read())
    order = re.findall(rb"(\d+)\.zi\0", entries["main.HMI"])
    fonts = [read(entries[f"{n.decode()}.zi"]) for n in order]
    if len(sys.argv) == 2:
        for i, f in enumerate(fonts):
            print(f"{i}: {f['name']}  {f['height']} px  {len(f['chars'])} characters")
        return
    f = fonts[int(sys.argv[2])]
    show(f["chars"], f["height"], sys.argv[3] if len(sys.argv) > 3 else "Ag")


if __name__ == "__main__":
    main()
