#!/usr/bin/env python3
"""OHpanel's own icons, beside the Material Design Icons: in the display's
icon fonts (boards/nspanel/nextion/fonts.py), the icon table and the web interface's
pictures (tools/gen_display_assets.py), in the icon chooser under the
category "OHpanel".

Every name starts with "ohpanel-", so none can meet an MDI name. An icon is
an em square, drawn where MDI's sit (see boards/nspanel/nextion/fonts.py), from

- an SVG in boards/nspanel/nextion/assets/icons (24 x 24 like MDI's), named as the file;
- or code below: the equalizers are drawn on the pixel grid of each size
  (19 and 27 px on the display, 24 px in the web interface), so their bars
  have sharp edges -- MDI's 24 unit grid does not divide into 19 pixels; the
  washer and the dryer, round shapes, are drawn eight times larger and
  scaled down (anti-aliased).

The frames of an animation (an indicator's or shortcut's icon list, see
docs/panel-config.md) keep everything that does not move in place, so
nothing jumps between them.

The display has them at U+FF00 + n, in name order; the firmware and the web
interface look icons up by name, so the numbers may change with every build.

Usage (draws all of them, enlarged, for a look):
    own_icons.py <out.png>
"""

import io
import math
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SVG_DIR = os.path.join(HERE, "assets", "icons")
FIRST = 0xFF00
CATEGORY = "OHpanel"
FRAMES = 8   # of an equalizer


def _image(em):
    from PIL import Image
    return Image.new("L", (em, em))


def _svg(path):
    def draw(em):
        from PIL import Image
        png = subprocess.run(["rsvg-convert", "-w", str(em), "-h", str(em), path],
                             capture_output=True, check=True).stdout
        return Image.open(io.BytesIO(png)).getchannel("A")
    return draw


def _bars(frame, em):
    """Three bars where MDI's equalizer has them (x 4, 10, 16 of 24, 4 wide,
    standing on y = 20), their heights in waves of different phase."""
    from PIL import ImageDraw
    s = em / 24
    left, right, bottom = round(4 * s), round(20 * s), round(20 * s)
    low, high, gap = round(4 * s), round(16 * s), max(1, round(2 * s))
    width = (right - left - 2 * gap) // 3
    x0 = left + (right - left - (3 * width + 2 * gap)) // 2
    img = _image(em)
    d = ImageDraw.Draw(img)
    for k, phase in enumerate((0, 2.1, 4.2)):
        level = 0.5 + 0.5 * math.sin(2 * math.pi * frame / FRAMES + phase)
        h = round(low + (high - low) * level)
        x = x0 + k * (width + gap)
        d.rectangle([x, bottom - h, x + width - 1, bottom - 1], fill=255)
    return img


# a hi-fi's spectrum display: columns, column width, gap, segment height,
# gap, segments -- per em size
RETRO = {19: (4, 3, 1, 1, 1, 8), 24: (4, 4, 1, 2, 1, 6), 27: (4, 5, 1, 2, 1, 7), 40: (4, 8, 2, 3, 1, 8)}


def _retro(frame, em):
    """Columns of flat LED segments with a peak that holds for two frames,
    then falls a segment a frame; the loop runs three rounds so the peaks of
    the last are the ones a looping animation shows."""
    from PIL import ImageDraw
    cols, w, gap, seg, sgap, segs = RETRO[em]
    phases, speeds = (0, 1.9, 4.0, 5.6), (1, 2, 1, 2)
    peak, hold = [0] * cols, [0] * cols
    for step in range(2 * FRAMES + frame + 1):
        f = step % FRAMES
        lit = [max(1, round(1 + (segs - 2) * (0.5 + 0.5 * math.sin(2 * math.pi * speeds[k] * f / FRAMES + phases[k]))))
               for k in range(cols)]
        for k in range(cols):
            if lit[k] >= peak[k]:
                peak[k], hold[k] = min(segs, lit[k] + 1), 2
            elif hold[k]:
                hold[k] -= 1
            else:
                peak[k] = max(lit[k] + 1, peak[k] - 1)
    img = _image(em)
    d = ImageDraw.Draw(img)
    x0, bottom = (em - (cols * w + (cols - 1) * gap)) // 2, round(20 * em / 24)
    for k in range(cols):
        x = x0 + k * (w + gap)
        for s in range(segs):
            if s < lit[k] or (s == peak[k] - 1 and peak[k] > lit[k]):
                y = bottom - 1 - s * (seg + sgap)
                d.rectangle([x, y - seg + 1, x + w - 1, y], fill=255)
    return img


LAUNDRY_SS = 8   # the laundry icons: drawn this many times larger, then scaled down


def _laundry(kind, frame, em):
    """A washing machine or a tumble dryer (MDI's proportions: the body from 4 to 20 of 24, the door's ring in its
    lower part), what is in the drum turning with the frame: in the washer the water, its surface tilting, and a
    sock going round on the drum's wall; in the dryer three fins."""
    from PIL import Image, ImageDraw
    big = Image.new("L", (em * LAUNDRY_SS, em * LAUNDRY_SS))
    d = ImageDraw.Draw(big)
    s = em * LAUNDRY_SS / 24
    d.rounded_rectangle([4 * s, 2 * s, 20 * s, 22 * s], radius=2.2 * s, outline=255, width=round(1.7 * s))
    d.line([4 * s, 6.2 * s, 20 * s, 6.2 * s], fill=255, width=round(1.3 * s))   # the control panel's edge
    knob = lambda x: d.ellipse([(x - 0.9) * s, 3.2 * s, (x + 0.9) * s, 5.0 * s], fill=255)
    knob(6.8)
    if kind == "washer":
        knob(9.4)
    else:
        d.rounded_rectangle([12.5 * s, 3.4 * s, 17.5 * s, 4.9 * s], radius=0.6 * s, fill=255)   # a display
    cx, cy, r = 12 * s, 14.2 * s, 5.6 * s
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=round(1.5 * s))
    turn = 2 * math.pi * frame / FRAMES
    if kind == "washer":
        inner = r - 2.2 * s
        tilt = 0.35 * math.sin(turn)
        surface = [(cx - inner + 2 * inner * k / 40, 0) for k in range(41)]
        surface = [(x, cy + 0.4 * s + math.tan(tilt) * (x - cx) + 0.5 * s * math.sin(3 * (x - cx) / inner * math.pi + turn))
                   for x, _ in surface]
        water = Image.new("L", big.size)
        ImageDraw.Draw(water).polygon(surface + [(cx + inner, cy + inner + s), (cx - inner, cy + inner + s)], fill=255)
        drum = Image.new("L", big.size)
        ImageDraw.Draw(drum).ellipse([cx - inner, cy - inner, cx + inner, cy + inner], fill=255)
        big.paste(255, (0, 0), Image.composite(water, Image.new("L", big.size), drum))
        a = turn - math.pi / 2
        sx, sy = cx + (inner - 0.2 * s) * math.cos(a), cy + (inner - 0.2 * s) * math.sin(a)
        d.ellipse([sx - 1.1 * s, sy - 1.1 * s, sx + 1.1 * s, sy + 1.1 * s], fill=255)
    else:
        inner = r - 2.4 * s
        for k in range(3):
            a = turn + k * 2 * math.pi / 3
            d.line([cx + 1.2 * s * math.cos(a), cy + 1.2 * s * math.sin(a), cx + inner * math.cos(a),
                    cy + inner * math.sin(a)], fill=255, width=round(1.2 * s))
    return big.resize((em, em), Image.LANCZOS)


def icons():
    """name -> (draw(em) -> alpha picture em x em, aliases), in name order."""
    out = {}
    for n in range(1, FRAMES + 1):
        out[f"ohpanel-equalizer-{n}"] = (lambda em, f=n - 1: _bars(f, em), ["music", "playing"])
        out[f"ohpanel-equalizer-retro-{n}"] = (lambda em, f=n - 1: _retro(f, em), ["music", "playing", "spectrum"])
        out[f"ohpanel-washer-{n}"] = (lambda em, f=n - 1: _laundry("washer", f, em), ["washing machine", "laundry", "running"])
        out[f"ohpanel-dryer-{n}"] = (lambda em, f=n - 1: _laundry("dryer", f, em), ["tumble dryer", "laundry", "running"])
    for file in sorted(os.listdir(SVG_DIR)):
        if file.endswith(".svg"):
            name = file[:-4]
            aliases = (["openhab", "logo"] if name == "ohpanel-openhab" else
                       ["code", "pin", "place"] if name.startswith("ohpanel-code-") else ["sound", "playing", "volume"])
            out[name] = (_svg(os.path.join(SVG_DIR, file)), aliases)
    for name in out:
        if not name.startswith("ohpanel-"):
            raise ValueError(f"own icon {name!r}: does not start with ohpanel-")
    return dict(sorted(out.items()))


def code_points() -> dict[str, int]:
    return {name: FIRST + k for k, name in enumerate(icons())}


def files() -> list[str]:
    """What the icons are made from (for the builds' fingerprints)."""
    return [os.path.abspath(__file__)] + sorted(os.path.join(SVG_DIR, f) for f in os.listdir(SVG_DIR))


def levels(img, bits: int) -> list[int]:
    """An alpha picture as 0 .. 2^bits - 1 per pixel."""
    top = (1 << bits) - 1
    return [(a * top + 127) // 255 for a in img.tobytes()]


if __name__ == "__main__":
    import sys
    from PIL import Image
    table = icons()
    sheet = Image.new("RGB", (len(table) * 44, 3 * 44), (20, 24, 32))
    for k, (name, (draw, _)) in enumerate(table.items()):
        for row, em in enumerate((19, 24, 27)):
            sheet.paste((255, 255, 255), (k * 44 + 4, row * 44 + 4, k * 44 + 4 + em, row * 44 + 4 + em), draw(em))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(sys.argv[1] if len(sys.argv) > 1 else "own-icons.png")
