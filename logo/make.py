#!/usr/bin/env python3
"""Writes OHpanel's logo files from code, so they can be rebuilt and reviewed:

    ohpanel-mark.svg         the mark: the NSPanel's front -- a 3:2 screen above two
                             buttons with their LED strips -- on openHAB's orange
    ohpanel-logo.svg         mark and wordmark, for light backgrounds
    ohpanel-logo-dark.svg    the same for dark backgrounds
    ohpanel-logo-nspanel.svg the NSPanel's edition: "for NSPanel" small under the
                             wordmark, right-aligned (the firmware's boot page has it)
    ohpanel-logo-nspanel-dark.svg   the same for dark backgrounds

The wordmark is set in Sora (boards/nspanel/nextion/assets/fonts, SIL Open Font License) and
converted to outlines, so the files look the same without the font installed:
"OH" bold in orange, "panel" regular.

    python3 logo/make.py      (needs fonttools: pip install fonttools)

The colour is openHAB's brand orange; the shapes are our own -- the openHAB
logo itself (circle and roof) is not used, see its trademark policy.
"""

import os

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "boards", "nspanel", "nextion", "assets", "fonts", "Sora[wght].ttf")

ORANGE = "#E64A19"      # openHAB brand orange
ORANGE_LIGHT = "#F3A486"
INK = "#2B2826"         # wordmark on light backgrounds
INK_DARK = "#F2EDE9"    # wordmark on dark backgrounds
SUB = "#8A7F79"         # the edition line's "for", on light backgrounds
SUB_DARK = "#B3A9A3"    # ... on dark backgrounds

# The mark, 64 x 64. The same drawing is in boards/nspanel/nextion/assets/generate.py
# (the boot page's logo) and www/index.html (header, favicon).
MARK = f"""<rect x="2" y="2" width="60" height="60" rx="14" fill="{ORANGE}"/>
<rect x="9" y="9" width="46" height="30.7" rx="3.5" fill="#FFFFFF"/>
<rect x="14" y="15" width="14" height="4" rx="2" fill="{ORANGE}"/>
<rect x="14" y="22" width="22" height="3" rx="1.5" fill="{ORANGE_LIGHT}"/>
<rect x="14" y="28" width="17" height="3" rx="1.5" fill="{ORANGE_LIGHT}"/>
<rect x="9" y="44" width="22" height="11" rx="3.5" fill="#FFFFFF"/>
<rect x="33" y="44" width="22" height="11" rx="3.5" fill="#FFFFFF"/>
<rect x="15" y="51.5" width="10" height="1.8" rx="0.9" fill="{ORANGE}"/>
<rect x="39" y="51.5" width="10" height="1.8" rx="0.9" fill="{ORANGE_LIGHT}"/>"""

FONT_SIZE = 46   # in mark units: the wordmark's caps a little shorter than the mark
GAP = 16

# The edition lockup ("for NSPanel"), in mark units: the wordmark's caps from
# EDITION_CAP_TOP, the line (EDITION_SIZE) right-aligned with the wordmark's
# end, its baseline at EDITION_BASELINE -- the same in boards/nspanel/nextion/assets/generate.py.
EDITION_CAP_TOP = 4
EDITION_SIZE = 13
EDITION_BASELINE = 61


def outline(text, weight, x, baseline, size=FONT_SIZE):
    """SVG path data for `text` in Sora at `weight` and `size`, starting at x on the baseline."""
    font = instantiateVariableFont(TTFont(FONT), {"wght": weight})
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    scale = size / font["head"].unitsPerEm
    pen = SVGPathPen(glyphs)
    for ch in text:
        name = cmap[ord(ch)]
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, x, baseline)))
        x += glyphs[name].width * scale
    return pen.getCommands(), x


def lockup(ink):
    font = TTFont(FONT)
    cap = font["OS/2"].sCapHeight * FONT_SIZE / font["head"].unitsPerEm
    baseline = 32 + cap / 2   # caps centred on the mark
    oh, x = outline("OH", 700, 64 + GAP, baseline)
    panel, end = outline("panel", 400, x, baseline)
    width = round(end + 2)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 64" width="{width * 2}" height="128">\n'
            f'<title>OHpanel</title>\n{MARK}\n'
            f'<path fill="{ORANGE}" d="{oh}"/>\n<path fill="{ink}" d="{panel}"/>\n</svg>\n')


def width(text, weight, size):
    font = instantiateVariableFont(TTFont(FONT), {"wght": weight})
    glyphs, cmap = font.getGlyphSet(), font.getBestCmap()
    return sum(glyphs[cmap[ord(ch)]].width for ch in text) * size / font["head"].unitsPerEm


def edition_lockup(ink, sub, device):
    """The wordmark a little higher, "for <device>" small under it, right-aligned:
    "for" regular in a muted colour, the device semibold in the wordmark's."""
    font = TTFont(FONT)
    cap = font["OS/2"].sCapHeight * FONT_SIZE / font["head"].unitsPerEm
    baseline = EDITION_CAP_TOP + cap
    oh, x = outline("OH", 700, 64 + GAP, baseline)
    panel, end = outline("panel", 400, x, baseline)
    name_w = width(device, 600, EDITION_SIZE)
    for_w = width("for ", 400, EDITION_SIZE)
    for_d, _ = outline("for ", 400, end - name_w - for_w, EDITION_BASELINE, EDITION_SIZE)
    name_d, _ = outline(device, 600, end - name_w, EDITION_BASELINE, EDITION_SIZE)
    w = round(end + 2)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} 64" width="{w * 2}" height="128">\n'
            f'<title>OHpanel for {device}</title>\n{MARK}\n'
            f'<path fill="{ORANGE}" d="{oh}"/>\n<path fill="{ink}" d="{panel}"/>\n'
            f'<path fill="{sub}" d="{for_d}"/>\n<path fill="{ink}" d="{name_d}"/>\n</svg>\n')


def main():
    with open(os.path.join(HERE, "ohpanel-mark.svg"), "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="256" height="256">\n'
                f'<title>OHpanel</title>\n{MARK}\n</svg>\n')
    for name, ink in (("ohpanel-logo.svg", INK), ("ohpanel-logo-dark.svg", INK_DARK)):
        with open(os.path.join(HERE, name), "w") as f:
            f.write(lockup(ink))
    for name, ink, sub in (("ohpanel-logo-nspanel.svg", INK, SUB), ("ohpanel-logo-nspanel-dark.svg", INK_DARK, SUB_DARK)):
        with open(os.path.join(HERE, name), "w") as f:
            f.write(edition_lockup(ink, sub, "NSPanel"))
    print("logo/: ohpanel-mark.svg, ohpanel-logo.svg, ohpanel-logo-dark.svg, ohpanel-logo-nspanel(-dark).svg")


if __name__ == "__main__":
    main()
