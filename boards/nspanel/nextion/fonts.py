#!/usr/bin/env python3
"""The display's fonts, made from the TrueType fonts in boards/nspanel/nextion/assets/fonts
(see boards/nspanel/nextion/tools/zifont.py for the format).

The pages refer to a font by its id -- its place in this list: 0..6 text in
Ubuntu, 7 and 8 the icon fonts (the Material Design Icons, and Noto Sans for
Latin-1 so a text in an icon field still shows).

Each font's height, em size and baseline are fixed here; the layout
(boards/nspanel/nextion/layout.py) and the firmware's fitting are made for them.

The icons sit at the code points the panel's icon table has
(boards/nspanel/nextion/icons.json): U+E000 + n on the display is U+F0001 + n in the MDI
font. Only the icons that table names are in the font.

A font converted from Ubuntu is a "Modified Version" under the Ubuntu Font
Licence (changing formats), not substantially changed, so it has to be named
"Ubuntu derivative <new work>" (UFL 1.0, 2(c)): all seven are
"Ubuntu derivative OHpanel".

OHpanel's own icons (boards/nspanel/nextion/own_icons.py) follow the MDI ones, at U+FF00 + n.

The 48 and 72 px text fonts also hold a few icons (MEDIA_ICONS, at their
icon code points), large on the media and volume pages where nothing of the
icon fonts' size would do: what plays when there is no cover, play/pause as a
disc, previous/next, minus/plus -- a handful, not all: every icon at 60 px
would not fit the display's flash.

Every character is anti-aliased (3-bit alpha).

Usage:
    fonts.py [output dir]   writes <name>.zi of every font (default build/fonts)
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
import zifont  # noqa: E402
sys.path.insert(0, HERE)
import own_icons  # noqa: E402

FONT_DIR = os.path.join(HERE, "assets", "fonts")
UBUNTU = "Ubuntu-R.ttf"
NOTO = "NotoSans-Regular.ttf"
MDI = "materialdesignicons-webfont.ttf"
UBUNTU_NAME = "Ubuntu derivative OHpanel"
ICONS = os.path.join(HERE, "icons.json")
OWN = None                     # OHpanel's own icons, drawn by boards/nspanel/nextion/own_icons.py
MDI_SHIFT = 0xF0001 - 0xE000   # display code point -> MDI font code point
# the icons the 48 and 72 px text fonts hold as well (the media page's large picture)
MEDIA_ICONS = ["music", "music-note", "radio", "headphones", "speaker", "podcast", "playlist-music",
               "play-circle", "pause-circle", "skip-previous", "skip-next", "minus", "plus"]


def text_chars(font: str) -> list[int]:
    """What a text font holds: all the TrueType font has in the Basic
    Multilingual Plane, but controls and combining marks (the display draws
    one character after the other and cannot put a mark on a letter)."""
    return sorted(c for c in zifont.cmap(os.path.join(FONT_DIR, font))
                  if 0x20 <= c <= 0xFFFF and not 0x7F <= c < 0xA0 and not 0x300 <= c < 0x370)


def icon_chars() -> list[int]:
    """The display code points of the icons boards/nspanel/nextion/icons.json names."""
    with open(ICONS) as f:
        table = json.load(f)
    return sorted({ord(bytes.fromhex(glyph).decode()) for glyph in table.values()})


# name, height, then per part: TrueType font, em size (px), baseline (px from
# the top), which characters -- for OHpanel's own icons (boards/nspanel/nextion/own_icons.py)
# the em size and the top of the em square instead, where MDI's is
FONTS = [
    (UBUNTU_NAME, 16, [(UBUNTU, 14.3, 13, "text")]),
    (UBUNTU_NAME, 18, [(UBUNTU, 16.9, 15, "text")]),
    (UBUNTU_NAME, 22, [(UBUNTU, 19.8, 18, "text")]),
    (UBUNTU_NAME, 26, [(UBUNTU, 23.2, 21, "text")]),
    (UBUNTU_NAME, 36, [(UBUNTU, 32.9, 30, "text")]),
    (UBUNTU_NAME, 48, [(UBUNTU, 43.6, 40, "text"), (MDI, 40, 40, "media")]),
    (UBUNTU_NAME, 72, [(UBUNTU, 64.9, 61, "text"), (MDI, 60, 59, "media")]),
    # the Latin-1 a px lower than in a text font: beside an icon in one field (the volume page's mute) its capitals
    # end where the icons' ink is centred (the icons draw low, ink 7..21); lower still would cut a descender
    ("icons24", 24, [(NOTO, 17.5, 20, "latin1"), (MDI, 19, 22, "icons"), (OWN, 19, 5, "own")]),
    ("icons32", 32, [(NOTO, 23.2, 25, "latin1"), (MDI, 27, 27, "icons"), (OWN, 27, 3, "own")]),
    # a button's icon in the middle of its tile (layout "center", a button without an item): icons only, no
    # Latin-1 -- 1.8 MB, what one background's pictures took
    ("icons48", 48, [(MDI, 40, 40, "icons"), (OWN, 40, 4, "own")]),
    # font 1's Latin-1 turned a quarter anticlockwise, for a text read from the bottom up (the NSPanel US on its side:
    # the relays' labels upright, Engine::relay_label): each character 18 px wide (font 1's height), as high as its
    # advance at the bottom of a 16 px cell -- the firmware puts them one over the other, its advance apart
    (UBUNTU_NAME + " turned", 16, [(UBUNTU, 16.9, 15, "latin1-turned")]),
]
TURNED_FONT = len(FONTS) - 1   # 10


def chars_of(part) -> list[tuple[int, str]]:
    """(display code point, character in the TrueType font) of a part."""
    font, _, _, which = part
    if which == "text":
        return [(c, chr(c)) for c in text_chars(font)]
    if which == "latin1":
        have = zifont.cmap(os.path.join(FONT_DIR, font))
        return [(c, chr(c)) for c in range(0x20, 0x100) if c in have and not 0x7F <= c < 0xA0]
    have = zifont.cmap(os.path.join(FONT_DIR, font))
    if which == "media":
        with open(ICONS) as f:
            table = json.load(f)
        codes = [ord(bytes.fromhex(table[n]).decode()) for n in MEDIA_ICONS]
        return [(c, chr(c + MDI_SHIFT)) for c in codes if c + MDI_SHIFT in have]
    return [(c, chr(c + MDI_SHIFT)) for c in icon_chars() if c + MDI_SHIFT in have]


def icon_ink(font_id: int, name: str) -> tuple:
    """Where an icon's ink lies as font `font_id` draws it: (advance, cell
    height, x0, y0, x1, y1) -- the ink box relative to the pen (x) and the top
    of the cell (y), x1 and y1 exclusive. What lines an icon up by what shows
    (ohpanel_hmi.py place_icon), not by its cell."""
    from PIL import ImageFont
    _, height, parts = FONTS[font_id]
    part = next(p for p in parts if p[0] == MDI)
    with open(ICONS) as f:
        code = ord(bytes.fromhex(json.load(f)[name]).decode())
    font = ImageFont.truetype(os.path.join(FONT_DIR, MDI), part[1])
    width, left, right, pixels = zifont.render(font, chr(code + MDI_SHIFT), height, part[2])
    w = left + width + right
    cols = [x for x in range(w) if any(pixels[y * w + x] for y in range(height))]
    rows = [y for y in range(height) if any(pixels[y * w + x] for x in range(w))]
    return width, height, min(cols) - left, min(rows), max(cols) - left + 1, max(rows) + 1


def icon_em(font_id: int) -> tuple:
    """The em square of a font's icons in its cell: (em px, its top) -- how the
    web preview places an icon picture (which fills its em) as the display
    draws it."""
    from PIL import ImageFont
    _, height, parts = FONTS[font_id]
    part = next(p for p in parts if p[0] == MDI)
    ascent, descent = ImageFont.truetype(os.path.join(FONT_DIR, MDI), part[1]).getmetrics()
    return part[1], part[2] - round(part[1] * ascent / (ascent + descent))


def own(em: int, top: int, height: int) -> dict:
    """OHpanel's own icons as characters: the em square at `top`, as wide as
    its advance (like MDI's)."""
    chars = {}
    for name, code in own_icons.code_points().items():
        pixels = [0] * (em * height)
        pixels[top * em:(top + em) * em] = own_icons.levels(own_icons.icons()[name][0](em), 3)
        chars[code] = (em, 0, 0, pixels)
    return chars


def turned(part, height: int) -> dict:
    """Font 1's Latin-1 (`part`: its TrueType font, size and baseline, as FONTS has them) turned a quarter
    anticlockwise: a character's column x of its 18 px cell becomes row height - 1 - x, its row y column y."""
    from PIL import ImageFont
    font, size, baseline = part[0], part[1], part[2]
    source = ImageFont.truetype(os.path.join(FONT_DIR, font), size)
    have = zifont.cmap(os.path.join(FONT_DIR, font))
    chars = {}
    for code in range(0x20, 0x100):
        if code not in have or 0x7F <= code < 0xA0:
            continue
        width, left, right, pixels = zifont.render(source, chr(code), 18, baseline)
        w = left + width + right
        if w > height:
            raise ValueError(f"turned font: U+{code:04X} is {w} px wide, the cell {height}")
        out = [0] * (18 * height)
        for y in range(18):
            for x in range(w):
                out[(height - 1 - x) * 18 + y] = pixels[y * w + x]
        chars[code] = (18, 0, 0, out)
    return chars


def make(name: str, height: int, parts) -> bytes:
    from PIL import ImageFont
    chars = {}
    for part in parts:
        if part[3] == "own":
            chars.update(own(part[1], part[2], height))
            continue
        if part[3] == "latin1-turned":
            chars.update(turned(part, height))
            continue
        font = ImageFont.truetype(os.path.join(FONT_DIR, part[0]), part[1])
        for code, text in chars_of(part):
            chars[code] = zifont.render(font, text, height, part[2])
    return zifont.write(name, height, chars)


def fingerprint() -> str:
    """What the fonts are made from: the TrueType fonts, the icon table, this
    file and the format's code."""
    h = hashlib.sha256()
    for path in sorted({os.path.join(FONT_DIR, p[0]) for _, _, parts in FONTS for p in parts if p[0]}) + [
            ICONS, os.path.abspath(__file__), zifont.__file__] + own_icons.files():
        h.update(os.path.basename(path).encode() + b"\0")
        with open(path, "rb") as f:
            h.update(f.read())
    return h.hexdigest()[:16]


def build(out_dir: str) -> list[bytes]:
    """Every font, in id order; made again only when what they are made from
    changed (they take a while)."""
    os.makedirs(out_dir, exist_ok=True)
    stamp = os.path.join(out_dir, "fingerprint")
    current = fingerprint()
    paths = [os.path.join(out_dir, f"{font}.zi") for font in range(len(FONTS))]
    fresh = os.path.exists(stamp) and open(stamp).read() == current and all(map(os.path.exists, paths))
    out = []
    for (name, height, parts), path in zip(FONTS, paths):
        if not fresh:
            data = make(name, height, parts)
            with open(path, "wb") as f:
                f.write(data)
            print(f"font {name}: {len(data)} bytes")
        with open(path, "rb") as f:
            out.append(f.read())
    if not fresh:
        with open(stamp, "w") as f:
            f.write(current)
    return out


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "build", "fonts"))
