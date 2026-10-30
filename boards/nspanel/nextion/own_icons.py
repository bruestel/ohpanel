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
  appliances (washer, dryer, dishwasher, oven, kitchen machine), the fire and the
  sprinklers are drawn eight times larger and scaled down (anti-aliased).

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


def _big(em):
    """A picture LAUNDRY_SS times larger to draw on, its drawing context, and px per unit of MDI's 24 grid."""
    from PIL import Image, ImageDraw
    big = Image.new("L", (em * LAUNDRY_SS, em * LAUNDRY_SS))
    return big, ImageDraw.Draw(big), em * LAUNDRY_SS / 24


def _drop(d, x, y, r):
    d.ellipse([x - r, y - r, x + r, y + r], fill=255)


def _dishwasher(frame, em):
    """A dishwasher (the washer's body, a handle under the control panel): inside, the spray arm turning at the
    bottom (as wide as it looks from the front) and drops rising from it."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([4 * s, 2 * s, 20 * s, 22 * s], radius=2.2 * s, outline=255, width=round(1.7 * s))
    d.line([4 * s, 6.2 * s, 20 * s, 6.2 * s], fill=255, width=round(1.3 * s))
    _drop(d, 6.8 * s, 4.1 * s, 0.9 * s)
    d.rounded_rectangle([10 * s, 7.9 * s, 14 * s, 9.1 * s], radius=0.6 * s, fill=255)   # the handle
    turn = 2 * math.pi * frame / FRAMES
    half = 4.6 * abs(math.cos(turn)) + 0.6
    d.line([(12 - half) * s, 18.6 * s, (12 + half) * s, 18.6 * s], fill=255, width=round(1.3 * s))
    _drop(d, 12 * s, 18.6 * s, 1.1 * s)
    for k, x in enumerate((8.5, 12, 15.5)):   # drops rising, each its own phase, from the arm to the top
        t = (frame / FRAMES + k / 3) % 1
        _drop(d, x * s, (17 - 6.5 * t) * s, (0.95 - 0.35 * t) * s)
    return big.resize((em, em), Image.LANCZOS)


def _oven(frame, em):
    """An oven: two knobs and a display in the panel, the window under it, heat rising in it as three waves."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([3 * s, 3 * s, 21 * s, 21 * s], radius=2 * s, outline=255, width=round(1.7 * s))
    d.line([3 * s, 7.4 * s, 21 * s, 7.4 * s], fill=255, width=round(1.3 * s))
    for x in (6.4, 9.2):
        _drop(d, x * s, 5.2 * s, 0.9 * s)
    d.rounded_rectangle([13 * s, 4.5 * s, 18.2 * s, 5.9 * s], radius=0.5 * s, fill=255)
    d.rounded_rectangle([6 * s, 9.6 * s, 18 * s, 18.4 * s], radius=1.2 * s, outline=255, width=round(1.3 * s))
    phase = 2 * math.pi * frame / FRAMES
    for x0 in (9, 12, 15):   # each wave half a sine period high, moving up: heat rising in the window
        pts = [((x0 + 0.8 * math.sin(2 * math.pi * y / 7 + phase)) * s, y * s)
               for y in [16.4 - 0.25 * k for k in range(17)]]
        d.line(pts, fill=255, width=round(1.2 * s), joint="curve")
    return big.resize((em, em), Image.LANCZOS)


def _kitchen_machine(frame, em):
    """A kitchen machine (a mixing jug on a base with a dial, its handle on the right): its blade turning in the
    jug, steam rising over the lid."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([3.5 * s, 17.4 * s, 20.5 * s, 22 * s], radius=1.4 * s, outline=255, width=round(1.4 * s))
    _drop(d, 12 * s, 19.7 * s, 1.1 * s)                                        # the dial
    d.polygon([(5.5 * s, 7.4 * s), (16.5 * s, 7.4 * s), (14.8 * s, 17.4 * s), (7.2 * s, 17.4 * s)], outline=255,
              width=round(1.5 * s))                                             # the jug
    d.rounded_rectangle([5 * s, 6 * s, 17 * s, 7.6 * s], radius=0.6 * s, fill=255)   # the lid
    d.rounded_rectangle([10 * s, 4.6 * s, 12 * s, 6.2 * s], radius=0.4 * s, fill=255)  # its cap
    d.arc([14.2 * s, 9 * s, 20 * s, 15.6 * s], -90, 90, fill=255, width=round(1.5 * s))   # the handle
    turn = 2 * math.pi * frame / FRAMES
    half = 3.4 * abs(math.cos(turn)) + 0.5   # the blade turning, as wide as it looks from the front
    d.line([(11 - half) * s, 13.6 * s, (11 + half) * s, 13.6 * s], fill=255, width=round(1.5 * s))
    d.line([11 * s, 13.6 * s, 11 * s, 16.4 * s], fill=255, width=round(1.3 * s))
    for k, x0 in enumerate((8, 14)):   # steam: a wave rising from the lid and fading
        t = (frame / FRAMES + k / 2) % 1
        top = 4.4 - 3.4 * t
        pts = [((x0 + 0.8 * math.sin(2 * math.pi * y / 3.2 + 2 * math.pi * t)) * s, y * s)
               for y in [top + 2.6 - 0.2 * j for j in range(14)]]
        d.line(pts, fill=round(255 * (1 - 0.55 * t)), width=round(1.3 * s), joint="curve")
    return big.resize((em, em), Image.LANCZOS)


def _fire(frame, em):
    """A flame (MDI's fire: a drop rising to a tip, the inner flame cut out), its tip swaying and the flames
    flickering."""
    from PIL import Image
    big, d, s = _big(em)
    turn = 2 * math.pi * frame / FRAMES

    def flame(cx, bottom, width, height, sway, fill):
        pts = []
        for k in range(61):   # from the tip round the left, the bottom and the right back to it
            a = 2 * math.pi * k / 60
            y = math.cos(a)   # 1 at the tip, -1 at the bottom
            reach = math.sin(a) * width * (0.62 + 0.38 * (1 - y) / 2) ** 0.9 * math.sqrt(max(0.0, 1 - ((y + 1) / 2) ** 3))
            lift = (y + 1) / 2
            pts.append(((cx + reach + sway * lift ** 2) * s, (bottom - height * lift) * s))
        d.polygon(pts, fill=fill)

    flame(12, 21.5, 7.2, 18.5 + 0.8 * math.sin(turn), 1.8 * math.sin(turn), 255)
    flame(12.2, 21.5, 3.6, 9 + 1.2 * math.sin(turn + 1.7), -1.2 * math.sin(turn + 0.8), 0)
    return big.resize((em, em), Image.LANCZOS)


def _sprinkler(frame, em):
    """An oscillating lawn sprinkler: its tube on two feet, a fan of five jets over it -- drops along a parabola
    each -- swinging from side to side."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([5 * s, 18 * s, 19 * s, 20 * s], radius=1 * s, fill=255)   # the tube
    for x, out in ((6, -1), (18, 1)):                                             # its feet
        d.line([x * s, 19 * s, (x + out) * s, 22 * s], fill=255, width=round(1.4 * s))
    tilt = 0.55 * math.sin(2 * math.pi * frame / FRAMES)
    for k in range(5):
        a = tilt + (k - 2) * 0.32
        for j in range(1, 8):
            t = j / 8
            _drop(d, (7 + 2.5 * k + 10 * t * math.sin(a)) * s, (17.6 - 30 * t * math.cos(a) + 22 * t * t) * s,
                  0.75 * s)
    return big.resize((em, em), Image.LANCZOS)


def _sprinkler_rotating(frame, em):
    """A rotating lawn sprinkler: its head on the ground, one jet arching from it and turning round -- as long as it
    looks from the front, fainter while behind."""
    from PIL import Image
    big, d, s = _big(em)
    d.line([3 * s, 21.6 * s, 21 * s, 21.6 * s], fill=255, width=round(1.4 * s))   # the ground
    d.pieslice([9 * s, 18 * s, 15 * s, 24 * s], 180, 360, fill=255)               # the head
    d.line([12 * s, 15 * s, 12 * s, 19 * s], fill=255, width=round(1.5 * s))      # its nozzle
    turn = 2 * math.pi * frame / FRAMES
    reach, near = 10 * math.cos(turn), math.sin(turn)
    shade = 255 if near >= 0 else round(255 * (0.55 + 0.45 * (1 + near)))
    for j in range(1, 8):
        t = j / 8
        x, y, r = (12 + reach * t) * s, (15 - 20 * t + 16 * t * t) * s, (0.8 + 0.3 * near) * s
        d.ellipse([x - r, y - r, x + r, y + r], fill=shade)
    return big.resize((em, em), Image.LANCZOS)


def _turned(points, cx, cy, a):
    c, n = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * n, cy + (x - cx) * n + (y - cy) * c) for x, y in points]


def _blades(d, s, cx, cy, r, n, a, width):
    """n blades round (cx, cy), r long, each a leaf (narrow at the hub, wide at its tip), turned by a."""
    for k in range(n):
        b = a + 2 * math.pi * k / n
        leaf = [(cx + r * t * math.cos(b) - width * math.sin(math.pi * t) * math.sin(b) * (0.4 + t),
                 cy + r * t * math.sin(b) + width * math.sin(math.pi * t) * math.cos(b) * (0.4 + t))
                for t in [j / 12 for j in range(13)]]
        back = [(cx + r * t * math.cos(b) + 0.3 * width * math.sin(math.pi * t) * math.sin(b),
                 cy + r * t * math.sin(b) - 0.3 * width * math.sin(math.pi * t) * math.cos(b))
                for t in [j / 12 for j in range(12, -1, -1)]]
        d.polygon([(x * s, y * s) for x, y in leaf + back], fill=255)


def _fan(frame, em):
    """A fan: three blades turning round the hub (a third of a turn over the frames: they look alike)."""
    from PIL import Image
    big, d, s = _big(em)
    _blades(d, s, 12, 12, 9.5, 3, 2 * math.pi / 3 * frame / FRAMES - math.pi / 2, 3.2)
    _drop(d, 12 * s, 12 * s, 2.2 * s)
    d.ellipse([10.9 * s, 10.9 * s, 13.1 * s, 13.1 * s], fill=0)
    return big.resize((em, em), Image.LANCZOS)


def _heat_pump(frame, em):
    """A heat pump's outdoor unit: its box, the fan behind the round grille turning, the pipes' ports beside it."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([2 * s, 5 * s, 22 * s, 19 * s], radius=1.4 * s, outline=255, width=round(1.5 * s))
    d.ellipse([4 * s, 6.5 * s, 15 * s, 17.5 * s], outline=255, width=round(1.3 * s))
    big2, d2, _ = _big(em)
    _blades(d2, s, 9.5, 12, 4.3, 3, 2 * math.pi / 3 * frame / FRAMES, 1.6)
    big.paste(255, (0, 0), big2)
    for y in (9, 12, 15):
        d.line([17 * s, y * s, 20 * s, y * s], fill=255, width=round(1.1 * s))
    for x in (5, 19):   # its feet
        d.line([x * s, 19 * s, x * s, 21 * s], fill=255, width=round(1.5 * s))
    return big.resize((em, em), Image.LANCZOS)


def _battery_charging(frame, em):
    """A battery upright, filling from the bottom over the frames, a bolt on it cut out."""
    from PIL import Image, ImageDraw
    big, d, s = _big(em)
    d.rounded_rectangle([9.5 * s, 2 * s, 14.5 * s, 4 * s], radius=0.5 * s, fill=255)
    d.rounded_rectangle([7 * s, 3.6 * s, 17 * s, 22 * s], radius=1.4 * s, outline=255, width=round(1.5 * s))
    level = (frame + 1) / FRAMES
    top = 20.3 - 14.8 * level
    if level > 0:
        d.rectangle([8.8 * s, top * s, 15.2 * s, 20.3 * s], fill=255)
    bolt = [(12.8, 7.5), (9.8, 13.6), (12, 13.6), (11.2, 18.5), (14.2, 12), (12, 12)]
    mask = Image.new("L", big.size)
    ImageDraw.Draw(mask).polygon([(x * s, y * s) for x, y in bolt], fill=255)
    inside = Image.new("L", big.size)
    ImageDraw.Draw(inside).rectangle([8.8 * s, top * s, 15.2 * s, 20.3 * s], fill=255)
    big.paste(0, (0, 0), Image.composite(mask, Image.new("L", big.size), inside))   # cut out where filled
    outside = Image.composite(Image.new("L", big.size), mask, inside)
    big.paste(255, (0, 0), outside)                                                  # drawn where empty
    return big.resize((em, em), Image.LANCZOS)


def _robot_vacuum(frame, em):
    """A robot vacuum from above: its round body, the bumper at the front, the button; its side brush turning at
    the front left."""
    from PIL import Image
    big, d, s = _big(em)
    d.ellipse([4 * s, 4 * s, 22 * s, 22 * s], outline=255, width=round(1.6 * s))
    d.line([8 * s, 9 * s, 18 * s, 9 * s], fill=255, width=round(1.4 * s))          # the bumper's edge
    _drop(d, 13 * s, 15 * s, 2 * s)                                                 # the button
    a = 2 * math.pi * frame / FRAMES / 3   # three arms: a third of a turn looks like a whole
    bx, by = 5.6, 5.6   # the side brush at the front left, its arms reaching out over the edge
    for k in range(3):
        b = a + 2 * math.pi * k / 3
        d.line([bx * s, by * s, (bx + 4.4 * math.cos(b)) * s, (by + 4.4 * math.sin(b)) * s], fill=255,
               width=round(1.4 * s))
    _drop(d, bx * s, by * s, 1.3 * s)
    return big.resize((em, em), Image.LANCZOS)


def _solar(frame, em):
    """A sun over a solar panel, its rays growing and shrinking in turn."""
    from PIL import Image
    big, d, s = _big(em)
    panel = [(4, 21.5), (20, 21.5), (17.5, 14), (6.5, 14)]
    d.polygon([(x * s, y * s) for x, y in panel], outline=255, width=round(1.3 * s))
    d.line([5.3 * s, 17.75 * s, 18.7 * s, 17.75 * s], fill=255, width=round(1 * s))
    for t in (1 / 3, 2 / 3):
        d.line([(6.5 + 11 * t) * s, 14 * s, (4 + 16 * t) * s, 21.5 * s], fill=255, width=round(1 * s))
    cx, cy = 12, 6.5
    _drop(d, cx * s, cy * s, 2.6 * s)
    for k in range(8):
        a = math.pi * k / 4
        grow = 0.5 + 0.5 * math.sin(2 * math.pi * frame / FRAMES + (k % 2) * math.pi)
        r0, r1 = 3.9, 4.9 + 1.4 * grow
        d.line([(cx + r0 * math.cos(a)) * s, (cy + r0 * math.sin(a)) * s, (cx + r1 * math.cos(a)) * s,
                (cy + r1 * math.sin(a)) * s], fill=255, width=round(1.2 * s))
    return big.resize((em, em), Image.LANCZOS)


def _grid(frame, em, to_grid):
    """A house on the left, a pylon on the right, between them dots moving: to the grid (feeding in) or from it."""
    from PIL import Image
    big, d, s = _big(em)
    d.polygon([(1.5 * s, 13 * s), (6.5 * s, 8 * s), (11.5 * s, 13 * s)], fill=255)   # the house: its roof
    d.rectangle([3 * s, 13 * s, 10 * s, 20 * s], outline=255, width=round(1.4 * s))
    d.rectangle([5.6 * s, 16 * s, 7.4 * s, 20 * s], fill=255)
    px = 19   # the pylon
    d.line([(px - 3) * s, 21 * s, px * s, 4 * s, (px + 3) * s, 21 * s], fill=255, width=round(1.4 * s))
    d.line([(px - 3) * s, 8 * s, (px + 3) * s, 8 * s], fill=255, width=round(1.3 * s))
    d.line([(px - 2) * s, 13 * s, (px + 2) * s, 13 * s], fill=255, width=round(1.1 * s))
    for k in range(2):   # along the line from the house's roof to the pylon's arm
        t = (frame / FRAMES + k / 2) % 1
        x = 9 + 7.5 * (t if to_grid else 1 - t)
        _drop(d, x * s, (9.5 - 1.5 * t if to_grid else 8 + 1.5 * t) * s, 1.25 * s)
    return big.resize((em, em), Image.LANCZOS)


def _waves(d, s, xs, bottom, height, frame, width=1.2, amp=0.8, period=7):
    phase = 2 * math.pi * frame / FRAMES
    for x0 in xs:
        pts = [((x0 + amp * math.sin(2 * math.pi * y / period + phase)) * s, y * s)
               for y in [bottom - height * j / 16 for j in range(17)]]
        d.line(pts, fill=255, width=round(width * s), joint="curve")


def _radiator(frame, em):
    """A radiator: its fins standing on two feet, heat rising from it as three waves."""
    from PIL import Image
    big, d, s = _big(em)
    for k in range(5):
        x = 4 + 4 * k
        d.rounded_rectangle([x * s, 12 * s, (x + 2.6) * s, 21 * s], radius=1.1 * s, fill=255)
    d.rectangle([4 * s, 14 * s, 20 * s, 15 * s], fill=255)
    d.rectangle([4 * s, 18.4 * s, 20 * s, 19.4 * s], fill=255)
    _waves(d, s, (7.5, 12, 16.5), 10.4, 8.2, frame)
    return big.resize((em, em), Image.LANCZOS)


def _kettle(frame, em):
    """A kettle: its body on the base, the handle, the spout, steam rising from the spout."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([4 * s, 20 * s, 18 * s, 22 * s], radius=0.8 * s, fill=255)   # the base
    d.polygon([(6 * s, 9 * s), (15 * s, 9 * s), (16.5 * s, 19.5 * s), (4.5 * s, 19.5 * s)], outline=255,
              width=round(1.5 * s))
    d.rounded_rectangle([7.5 * s, 7.4 * s, 13.5 * s, 9 * s], radius=0.6 * s, fill=255)   # the lid
    d.arc([12.6 * s, 9.6 * s, 20 * s, 17.4 * s], -90, 90, fill=255, width=round(1.5 * s))   # the handle
    d.line([6 * s, 12 * s, 2.6 * s, 9.4 * s], fill=255, width=round(1.6 * s))       # the spout
    for k, x0 in enumerate((3.2, 6.4)):
        t = (frame / FRAMES + k / 2) % 1
        top = 6.4 - 4.6 * t
        pts = [((x0 + 0.7 * math.sin(2 * math.pi * y / 3.2 + 2 * math.pi * t)) * s, y * s)
               for y in [top + 2.6 - 0.2 * j for j in range(14)]]
        d.line(pts, fill=round(255 * (1 - 0.55 * t)), width=round(1.3 * s), joint="curve")
    return big.resize((em, em), Image.LANCZOS)


def _coffee(frame, em):
    """A cup of coffee on its saucer, steam rising from it."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([3 * s, 20 * s, 19 * s, 21.6 * s], radius=0.8 * s, fill=255)   # the saucer
    d.chord([4.5 * s, 7 * s, 16.5 * s, 21 * s], 0, 180, fill=255)                     # the cup
    d.rectangle([4.5 * s, 11 * s, 16.5 * s, 14 * s], fill=255)
    d.arc([14 * s, 11 * s, 20.5 * s, 17 * s], -90, 90, fill=255, width=round(1.5 * s))   # its handle
    for k, x0 in enumerate((8, 10.5, 13)):
        t = (frame / FRAMES + k / 3) % 1
        top = 8.6 - 6.6 * t
        pts = [((x0 + 0.6 * math.sin(2 * math.pi * y / 3 + 2 * math.pi * t)) * s, y * s)
               for y in [top + 2.4 - 0.2 * j for j in range(13)]]
        d.line(pts, fill=round(255 * (1 - 0.6 * t)), width=round(1.2 * s), joint="curve")
    return big.resize((em, em), Image.LANCZOS)


def _bell(frame, em):
    """A bell swinging, sound arcs beside it on the side it swings to."""
    from PIL import Image
    big, d, s = _big(em)
    swing = 0.35 * math.sin(2 * math.pi * frame / FRAMES)
    cx, cy = 12, 3.5   # it hangs here
    shape = [(12, 3.5), (15, 5.5), (16, 9), (16.5, 14), (19, 17), (5, 17), (7.5, 14), (8, 9), (9, 5.5)]
    d.polygon([(x * s, y * s) for x, y in _turned(shape, cx, cy, swing)], fill=255)
    kx, ky = _turned([(12, 19.2)], cx, cy, swing)[0]
    _drop(d, kx * s, ky * s, 1.6 * s)   # the clapper
    side = 1 if swing > 0.05 else -1 if swing < -0.05 else 0   # the sound on the side it swings to
    for sgn in ((1, -1) if side == 0 else (side,)):
        for r in (8.6, 10.8):
            d.arc([(12 - r) * s, (11 - r) * s, (12 + r) * s, (11 + r) * s], -28 if sgn > 0 else 152,
                  28 if sgn > 0 else 208, fill=255, width=round(1.2 * s))
    return big.resize((em, em), Image.LANCZOS)


def _siren(frame, em):
    """An alarm light: its dome on the base, rays round it flashing (on, then off)."""
    from PIL import Image
    big, d, s = _big(em)
    d.rounded_rectangle([5 * s, 19 * s, 19 * s, 22 * s], radius=0.8 * s, fill=255)   # the base
    d.chord([7 * s, 9 * s, 17 * s, 29 * s], 180, 360, fill=255)                     # the dome
    d.rectangle([7 * s, 17 * s, 17 * s, 19 * s], fill=255)
    d.ellipse([9.4 * s, 11.6 * s, 11.4 * s, 13.6 * s], fill=0)                      # its shine
    on = math.sin(2 * math.pi * frame / FRAMES)
    if on > -0.4:
        reach = 1.3 + 1.4 * max(0.0, on)
        for a in (-150, -115, -90, -65, -30):
            r = math.radians(a)
            d.line([(12 + 7 * math.cos(r)) * s, (17 + 7 * math.sin(r)) * s, (12 + (7 + reach) * math.cos(r)) * s,
                    (17 + (7 + reach) * math.sin(r)) * s], fill=255, width=round(1.4 * s))
    return big.resize((em, em), Image.LANCZOS)


def _garage(frame, em):
    """A garage: its roof, the door going up over the first half of the frames and down again over the second."""
    from PIL import Image
    big, d, s = _big(em)
    d.line([2 * s, 9 * s, 12 * s, 3 * s, 22 * s, 9 * s], fill=255, width=round(1.6 * s), joint="curve")
    d.line([4 * s, 8.5 * s, 4 * s, 21.5 * s], fill=255, width=round(1.5 * s))
    d.line([20 * s, 8.5 * s, 20 * s, 21.5 * s], fill=255, width=round(1.5 * s))
    open_ = (frame / (FRAMES / 2)) if frame <= FRAMES / 2 else (FRAMES - frame) / (FRAMES / 2)
    bottom = 21.5 - 10.5 * open_
    top = 10.5
    y = top
    while y < bottom - 0.3:   # the door's panels, from the top down to its bottom edge
        d.rectangle([6 * s, y * s, 18 * s, min(y + 2.2, bottom) * s], fill=255)
        y += 3
    return big.resize((em, em), Image.LANCZOS)


def icons():
    """name -> (draw(em) -> alpha picture em x em, aliases), in name order."""
    out = {}
    for n in range(1, FRAMES + 1):
        out[f"ohpanel-equalizer-{n}"] = (lambda em, f=n - 1: _bars(f, em), ["music", "playing"])
        out[f"ohpanel-equalizer-retro-{n}"] = (lambda em, f=n - 1: _retro(f, em), ["music", "playing", "spectrum"])
        out[f"ohpanel-washer-{n}"] = (lambda em, f=n - 1: _laundry("washer", f, em), ["washing machine", "laundry", "running"])
        out[f"ohpanel-dryer-{n}"] = (lambda em, f=n - 1: _laundry("dryer", f, em), ["tumble dryer", "laundry", "running"])
        out[f"ohpanel-dishwasher-{n}"] = (lambda em, f=n - 1: _dishwasher(f, em), ["dishwasher", "kitchen", "running"])
        out[f"ohpanel-oven-{n}"] = (lambda em, f=n - 1: _oven(f, em), ["oven", "baking", "kitchen", "running"])
        out[f"ohpanel-kitchen-machine-{n}"] = (lambda em, f=n - 1: _kitchen_machine(f, em),
                                               ["kitchen machine", "food processor", "cooking", "running"])
        out[f"ohpanel-fire-{n}"] = (lambda em, f=n - 1: _fire(f, em), ["fire", "flame", "burning", "heating"])
        out[f"ohpanel-sprinkler-{n}"] = (lambda em, f=n - 1: _sprinkler(f, em), ["sprinkler", "watering", "garden", "running"])
        out[f"ohpanel-sprinkler-rotating-{n}"] = (lambda em, f=n - 1: _sprinkler_rotating(f, em),
                                                  ["sprinkler", "watering", "garden", "lawn", "running"])
        out[f"ohpanel-fan-{n}"] = (lambda em, f=n - 1: _fan(f, em), ["fan", "ventilation", "air", "running"])
        out[f"ohpanel-heat-pump-{n}"] = (lambda em, f=n - 1: _heat_pump(f, em), ["heat pump", "air conditioning", "running"])
        out[f"ohpanel-battery-charging-{n}"] = (lambda em, f=n - 1: _battery_charging(f, em),
                                                ["battery", "charging", "car", "storage"])
        out[f"ohpanel-robot-vacuum-{n}"] = (lambda em, f=n - 1: _robot_vacuum(f, em), ["robot vacuum", "cleaning", "running"])
        out[f"ohpanel-solar-{n}"] = (lambda em, f=n - 1: _solar(f, em), ["solar", "photovoltaics", "pv", "sun", "energy"])
        out[f"ohpanel-grid-feed-in-{n}"] = (lambda em, f=n - 1: _grid(f, em, True), ["grid", "feed in", "export", "energy"])
        out[f"ohpanel-grid-supply-{n}"] = (lambda em, f=n - 1: _grid(f, em, False), ["grid", "supply", "import", "energy"])
        out[f"ohpanel-radiator-{n}"] = (lambda em, f=n - 1: _radiator(f, em), ["radiator", "heating", "warm"])
        out[f"ohpanel-kettle-{n}"] = (lambda em, f=n - 1: _kettle(f, em), ["kettle", "boiling", "kitchen", "running"])
        out[f"ohpanel-coffee-{n}"] = (lambda em, f=n - 1: _coffee(f, em), ["coffee", "cup", "hot", "kitchen"])
        out[f"ohpanel-bell-{n}"] = (lambda em, f=n - 1: _bell(f, em), ["bell", "doorbell", "ringing"])
        out[f"ohpanel-siren-{n}"] = (lambda em, f=n - 1: _siren(f, em), ["siren", "alarm", "alert", "flashing"])
        out[f"ohpanel-garage-{n}"] = (lambda em, f=n - 1: _garage(f, em), ["garage", "door", "gate", "moving"])
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
