#!/usr/bin/env python3
"""A line badge (an entity row's "badge", Engine::draw_entities): its pieces in the icon fonts (own_icons.py BADGE) as
the display has them (fonts.own), side by side in the row's icon field -- for every panel variant's layout
(www/layout-<variant>.json):

  - each character's piece fills the band at its right edge, the start's right edge and the end's left too: the
    pieces join where the band is whole, and two characters never touch;
  - every number from 0 to 999 fits each style's field, and so do a letter or two with digits ("S1", "U55", "RE5");
  - the badge lies within SLACK px of where an icon's ink is centred: the field centres it by its advance, which is
    the band's.

Needs Pillow and the fonts in boards/nspanel/nextion/assets/fonts.
"""

import itertools
import json
import os
import string
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NEXTION = os.path.join(ROOT, "boards", "nspanel", "nextion")
sys.path.insert(0, NEXTION)
sys.path.insert(0, os.path.join(NEXTION, "tools"))

import fonts  # noqa: E402
import own_icons  # noqa: E402

SLACK = 1.0   # px between the badge's middle and an icon's, either way
FIELDS = {"entities": "row1_icon", "entities_rows": "row1_icon", "entities_cards": "card1_icon"}
FULL = 7      # a pixel's level when fully drawn (3 bits)

failures = 0


def check(cond, message):
    global failures
    if not cond:
        print("FAIL " + message)
        failures += 1


_pieces = {}


def pieces(font_id):
    """piece -> (width, pixels by row: levels 0..7), as the display's font `font_id` has the badge's pieces."""
    if font_id not in _pieces:
        _, height, parts = fonts.FONTS[font_id]
        part = next(p for p in parts if p[0] == fonts.OWN)
        chars = fonts.own(part[1], part[2], height)
        out = {}
        for name, code in own_icons.code_points().items():
            if name.startswith(own_icons.BADGE):
                w, _, _, pixels = chars[code]
                out[name[len(own_icons.BADGE):]] = (w, [pixels[y * w:(y + 1) * w] for y in range(height)])
        _pieces[font_id] = out
    return _pieces[font_id]


def band(font_id):
    """The rows the badge's band fills: those where the digit 1's piece is drawn at its right edge (a character's
    piece has the band whole there)."""
    _, rows = pieces(font_id)["1"]
    return [y for y, r in enumerate(rows) if r[-1] == FULL]


def width(font_id, text):
    p = pieces(font_id)
    return sum(p[c][0] for c in ["start"] + list(text.lower()) + ["end"])


def icon_middle(font_id, field_w):
    advance, _, x0, y0, x1, y1 = fonts.icon_ink(font_id, "circle")
    return (field_w - advance) // 2 + (x0 + x1) / 2, (y0 + y1) / 2


def main():
    numbers = [str(n) for n in range(1000)]
    lines = ([a + b for a in string.ascii_uppercase for b in string.digits]                  # S1, N8
             + [a + b + c for a in "SUMNXEB" for b in string.digits for c in string.digits]   # U55, M10, N31
             + [a + b + c for a, b in ("RE", "RB", "IC", "IR", "EC", "SB", "NE") for c in string.digits])   # RE5
    for font_id in (7, 8):
        p, rows = pieces(font_id), band(font_id)
        check(len(rows) >= 10, f"font {font_id}: a band of {len(rows)} rows")
        for ch, (w, px) in p.items():
            edges = [0] if ch == "end" else [w - 1]
            for x in edges:
                check(all(px[y][x] == FULL for y in rows),
                      f"font {font_id}: piece {ch!r} leaves its band's column {x} open (a seam)")
            outside = [px[y][x] for y in range(len(px)) if y not in rows for x in range(w)]
            check(max(outside) <= (FULL if ch in ("start", "end") else 0),
                  f"font {font_id}: piece {ch!r} draws outside its band")
            if ch not in ("start", "end"):
                check(any(px[y][x] < FULL for y in rows for x in range(w)), f"font {font_id}: piece {ch!r} has no cut-out")
    for path in sorted(os.listdir(os.path.join(ROOT, "www"))):
        if not (path.startswith("layout-") and path.endswith(".json")):
            continue
        variant = path[len("layout-"):-len(".json")]
        with open(os.path.join(ROOT, "www", path)) as f:
            layout = json.load(f)
        for page, name in FIELDS.items():
            obj = next(o for o in layout["pages"][page]["objects"] if o["n"] == name)
            font_id, field_w = obj["f"], obj["w"]
            check(obj.get("xc") == 1 and obj.get("yc", 1) == 1, f"{variant} {page}.{name}: no longer centred")
            check(obj["h"] == fonts.FONTS[font_id][1], f"{variant} {page}.{name}: not as high as its font")
            ix, iy = icon_middle(font_id, field_w)
            rows = band(font_id)
            by = (rows[0] + rows[-1] + 1) / 2
            check(abs(by - iy) <= SLACK, f"{variant} {page}.{name}: the band's middle at {by}, an icon's at {iy}")
            worst = 0.0
            for text in numbers:
                w = width(font_id, text)
                check(w <= field_w, f"{variant} {page}.{name}: \"{text}\" is {w} px, the field {field_w}")
                bx = (field_w - w) // 2 + w / 2
                worst = max(worst, abs(bx - ix))
                check(abs(bx - ix) <= SLACK, f"{variant} {page}.{name}: \"{text}\" centred on {bx}, an icon on {ix}")
            too_wide = [t for t in lines if width(font_id, t) > field_w]
            check(not too_wide, f"{variant} {page}.{name}: too wide: {' '.join(too_wide[:12])}")
            widest = max(itertools.chain(numbers, lines), key=lambda t: width(font_id, t))
            print(f"badge: {variant} {page} (font {font_id}, field {field_w} px): 0..999 and {len(lines)} lines fit "
                  f"(widest \"{widest}\" {width(font_id, widest)} px), centred within {worst:.1f} px across, "
                  f"{abs(by - iy):.1f} px up or down")
    if failures:
        print(f"badge: {failures} check(s) failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
