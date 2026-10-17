#!/usr/bin/env python3
"""Generate OHpanel's own display pictures: home backgrounds, weather icons,
button tiles and the boot page's logo. (compose.py
puts icons and tiles onto every background.)

Everything is drawn procedurally here, so the pictures carry no third-party
rights and can be regenerated or varied at will.

  boards/nspanel/nextion/assets/generate.py            writes backgrounds/*.png and weather/*.png
  boards/nspanel/nextion/assets/generate.py --sheet    also writes contact sheets for review

The display shows 16-bit colour (RGB565). Dark, smooth gradients band visibly
at that depth, so backgrounds are rendered in full precision and reduced to
RGB565 with ordered dithering here -- the PNGs are exactly what the panel can
show, and the Nextion Editor has nothing left to quantise.
"""

import glob
import io
import math
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import layout  # noqa: E402
W, H = 480, 320

# ------------------------------------------------------------------ helpers --

BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]], dtype=np.float64) / 64.0 - 0.5


def to_rgb565(img):
    """float RGB array in 0..1 -> uint8 RGB image restricted to RGB565 levels, dithered."""
    h, w, _ = img.shape
    threshold = np.tile(BAYER8, (h // 8 + 1, w // 8 + 1))[:h, :w]
    out = np.empty_like(img)
    for c, levels in enumerate((31, 63, 31)):
        v = img[:, :, c] * levels + threshold
        out[:, :, c] = np.clip(np.round(v), 0, levels) / levels
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGB")


def grid():
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    return x / W, y / H


def mix(a, b, t):
    t = t[..., None]
    return np.asarray(a) * (1 - t) + np.asarray(b) * t


def rgb(hexcode):
    return np.array([int(hexcode[i:i + 2], 16) / 255.0 for i in (1, 3, 5)])


def glow(img, cx, cy, radius, color, strength):
    x, y = grid()
    d = np.sqrt(((x - cx) * W / H) ** 2 + (y - cy) ** 2)
    img += np.exp(-(d / radius) ** 2)[..., None] * rgb(color) * strength


def ribbon(img, phase, freq, amp, centre, width, color, strength, tilt=0.0):
    """A soft band along a sine curve, brightest at its centre line."""
    x, y = grid()
    curve = centre + amp * np.sin(2 * math.pi * freq * x + phase) + tilt * (x - 0.5)
    d = np.abs(y - curve)
    img += (np.exp(-(d / width) ** 2) * (0.35 + 0.65 * x))[..., None] * rgb(color) * strength


def silhouette(img, base, rough, height, color, seed, fade=None):
    """A layered ridge line (hills, dunes, tree line) filled to the bottom."""
    rng = np.random.default_rng(seed)
    x, y = grid()
    xs = np.linspace(0, 1, W)
    ridge = np.full(W, base)
    for octave in range(1, 6):
        ridge += rng.uniform(-1, 1) * height / octave * np.sin(2 * math.pi * (octave * rough) * xs + rng.uniform(0, 6))
    mask = y >= ridge[None, :]
    col = rgb(color)
    if fade is not None:  # lighter at the ridge, like mist settling between layers
        t = np.clip((y - ridge[None, :]) / fade, 0, 1)
        layer = mix(col * 1.6, col, t)
        img[mask] = layer[mask]
    else:
        img[mask] = col


# -------------------------------------------------------------- backgrounds --

def bg_aurora():
    x, y = grid()
    img = mix(rgb("#04060b"), rgb("#020305"), y)
    ribbon(img, 0.6, 0.55, 0.10, 0.70, 0.07, "#1fb3a6", 0.45, tilt=-0.25)
    ribbon(img, 2.2, 0.75, 0.08, 0.78, 0.05, "#7a3cff", 0.40, tilt=-0.30)
    ribbon(img, 4.0, 0.40, 0.12, 0.62, 0.10, "#1c5cff", 0.22, tilt=-0.15)
    return img


def bg_candy():
    """Coloured lights out of focus (bokeh) -- pink, orange, cyan, lime,
    violet -- on a deep violet."""
    x, y = grid()
    img = mix(rgb("#1a0a2a"), rgb("#07040e"), y)
    rng = np.random.default_rng(5)
    cols = ["#ff4f9a", "#ffa040", "#3fd6ff", "#a6ff4f", "#b56bff"]
    for i in range(16):
        glow(img, rng.uniform(0, 1), rng.uniform(0.05, 0.95), rng.uniform(0.04, 0.12), cols[i % len(cols)],
             rng.uniform(0.25, 0.5))
    return img


def bg_dunes():
    x, y = grid()
    img = mix(rgb("#1a0d24"), rgb("#05030a"), np.clip(y * 1.4, 0, 1))
    glow(img, 0.80, 0.55, 0.30, "#c2542b", 0.30)
    silhouette(img, 0.70, 0.8, 0.05, "#160b0a", 21, fade=0.18)
    silhouette(img, 0.80, 1.1, 0.04, "#0f0707", 22, fade=0.14)
    silhouette(img, 0.90, 1.6, 0.03, "#080404", 23)
    return img


def bg_forest():
    x, y = grid()
    img = mix(rgb("#0d1a1c"), rgb("#030606"), y)
    glow(img, 0.70, 0.45, 0.35, "#3d6b5c", 0.25)
    silhouette(img, 0.62, 1.4, 0.06, "#0b1714", 31, fade=0.20)
    silhouette(img, 0.74, 2.5, 0.05, "#08110f", 32, fade=0.16)
    silhouette(img, 0.86, 4.0, 0.04, "#040807", 33)
    return img


def bg_black():
    return np.zeros((H, W, 3))


BACKGROUNDS = {
    "aurora": ("Aurora", bg_aurora),
    "candy": ("Candy", bg_candy),
    "dunes": ("Dunes", bg_dunes),
    "forest": ("Forest", bg_forest),
    "black": ("Black", bg_black),
}

# The colours that go with each background (the firmware's defaults, which the
# configuration may override, display.theme): accent -- an active button's
# icon --, text -- icons and values --, dim -- inactive icons.
THEMES = {
    "aurora": {"accent": "#4fc3f7", "text": "#ffffff", "dim": "#c8ccc8"},
    "candy": {"accent": "#ff5fa8", "text": "#ffffff", "dim": "#d0c4e0"},
    "dunes": {"accent": "#ffb74d", "text": "#fff8ee", "dim": "#d8c8b0"},
    "forest": {"accent": "#8bd17c", "text": "#ffffff", "dim": "#c0d0c0"},
    "black": {"accent": "#0080f8", "text": "#ffffff", "dim": "#c8ccc8"},
}

# ------------------------------------------------------------ weather icons --
#
# Meteocons by Bas Milius (MIT, boards/nspanel/nextion/assets/meteocons/LICENSE), the "fill"
# style of release 2.0.0, its static exports (design/fill/export, without the
# "wi_" prefix): the SVGs rendered at
# ICON_OUT px; compose.py scales them to each place.

ICON_OUT = 200
METEOCONS = os.path.join(HERE, "meteocons")


def weather_icons():
    """Icon name -> RGBA picture, one per SVG in meteocons/."""
    import subprocess
    out = {}
    for path in sorted(glob.glob(os.path.join(METEOCONS, "*.svg"))):
        png = subprocess.run(["rsvg-convert", "-w", str(ICON_OUT), "-h", str(ICON_OUT), path],
                             check=True, capture_output=True).stdout
        out[os.path.splitext(os.path.basename(path))[0]] = Image.open(io.BytesIO(png)).convert("RGBA")
    return out


# --------------------------------------------------------------------- logo --
#
# The logo on the boot page (layout.BOOT_LOGO): the mark and the wordmark --
# "OH" bold in orange, "panel" regular -- with transparent surroundings;
# compose.py puts it onto every background.

# The logo's typeface (SIL Open Font License, see boards/nspanel/nextion/assets/fonts/OFL.txt).
FONT = os.path.join(HERE, "fonts", "Sora[wght].ttf")

ORANGE = (230, 74, 25)        # openHAB brand orange #E64A19
ORANGE_LIGHT = (243, 164, 134)
WHITE = (242, 237, 233)

# The mark, in units of a 64 x 64 square: the NSPanel's front -- a 3:2 screen
# above two buttons with their LED strips -- on the orange tile. The same
# drawing as logo/make.py: (x, y, w, h, corner radius, colour).
MARK = [
    (2, 2, 60, 60, 14, ORANGE),
    (9, 9, 46, 30.7, 3.5, (255, 255, 255)),
    (14, 15, 14, 4, 2, ORANGE),
    (14, 22, 22, 3, 1.5, ORANGE_LIGHT),
    (14, 28, 17, 3, 1.5, ORANGE_LIGHT),
    (9, 44, 22, 11, 3.5, (255, 255, 255)),
    (33, 44, 22, 11, 3.5, (255, 255, 255)),
    (15, 51.5, 10, 1.8, 0.9, ORANGE),
    (39, 51.5, 10, 1.8, 0.9, ORANGE_LIGHT),
]


def font(size, weight):
    f = ImageFont.truetype(FONT, size)
    f.set_variation_by_axes([weight])
    return f


def draw_mark(d, x, y, size):
    s = size / 64
    for mx, my, mw, mh, r, color in MARK:
        d.rounded_rectangle((x + mx * s, y + my * s, x + (mx + mw) * s, y + (my + mh) * s), radius=r * s, fill=color)


# The edition lockup, in mark units (64 = the mark's height), as logo/make.py
# draws it (ohpanel-logo-nspanel.svg): the wordmark FONT_SIZE with its caps from
# EDITION_CAP_TOP, "for NSPanel" (EDITION_SIZE) right-aligned under it, its
# baseline at EDITION_BASELINE.
FONT_SIZE, GAP = 46, 16
EDITION_CAP_TOP, EDITION_SIZE, EDITION_BASELINE = 4, 13, 61
EDITION = "NSPanel"
SUB = (179, 169, 163)     # "for", muted (logo/make.py SUB_DARK)


def logo():
    """The mark (as high as the picture), the wordmark beside it and the
    edition line under that ("for NSPanel"), in logo/make.py's proportions,
    centred in the picture. Drawn at 4x, then scaled down."""
    _, _, w, h = layout.BOOT_LOGO
    k = 4
    img = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = h * k / 64                      # one mark unit
    mark, gap = h * k, GAP * u
    bold, regular = font(round(FONT_SIZE * u), 700), font(round(FONT_SIZE * u), 400)
    w_oh, w_panel = d.textlength("OH", font=bold), d.textlength("panel", font=regular)
    left = (w * k - (mark + gap + w_oh + w_panel)) / 2
    draw_mark(d, left, 0, mark)
    # the wordmark: its caps from EDITION_CAP_TOP (anchored on the baseline, as the caps' height says)
    cap = bold.getbbox("H", anchor="ls")
    baseline = EDITION_CAP_TOP * u - cap[1]
    x = left + mark + gap
    d.text((x, baseline), "OH", font=bold, fill=ORANGE, anchor="ls")
    d.text((x + w_oh, baseline), "panel", font=regular, fill=WHITE, anchor="ls")
    # the edition line, right-aligned with the wordmark's end
    end = x + w_oh + w_panel
    small, small_bold = font(round(EDITION_SIZE * u), 400), font(round(EDITION_SIZE * u), 600)
    w_name = d.textlength(EDITION, font=small_bold)
    d.text((end, EDITION_BASELINE * u), EDITION, font=small_bold, fill=WHITE, anchor="rs")
    d.text((end - w_name, EDITION_BASELINE * u), "for ", font=small, fill=SUB, anchor="rs")
    return img.resize((w, h), Image.LANCZOS)


# ------------------------------------------------------------- button tiles --
#
# The tiles behind the buttons of a button page, inactive and active: a
# rounded square inset by 2 px, corner radius 20, flat #2c2c2c or white, with
# transparent corners, so the background shows there
# (boards/nspanel/nextion/assets/compose.py). The inside stays one flat colour:
# the button's texts sit on it with that colour as their own background.

TILES = {"off": (44, 44, 44), "on": (255, 255, 255)}


def wide_tile(color, w, h, radius):
    """A tile like button_tile() of another size: the settings page's
    restart button (boards/nspanel/nextion/layout.py SETTINGS_BUTTON)."""
    k = 4
    img = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle((2 * k, 2 * k, (w - 2) * k - 1, (h - 2) * k - 1), radius=radius * k,
                                          fill=color + (255,))
    return img.resize((w, h), Image.LANCZOS)


def colour_wheel(size):
    """The light page's colour wheel (layout.COLOUR_WHEEL): the hue around --
    0 deg to the right, counter-clockwise, as Engine::light_wheel reads a touch
    --, the saturation from the middle outwards, full brightness; transparent
    outside the disc (compose.py lays it on every background)."""
    k = 4
    n = size * k
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    dx, dy = (x + 0.5) / k - size / 2, (y + 0.5) / k - size / 2
    r = np.sqrt(dx * dx + dy * dy) / (size / 2)
    h = (np.degrees(np.arctan2(-dy, dx)) % 360) / 60.0
    s = np.clip(r, 0, 1)
    c = s
    xx = c * (1 - np.abs(h % 2 - 1))
    zero = np.zeros_like(h)
    sector = np.floor(h).astype(int) % 6
    table = [(c, xx, zero), (xx, c, zero), (zero, c, xx), (zero, xx, c), (xx, zero, c), (c, zero, xx)]
    rgb = np.zeros((n, n, 3))
    for i, (rr, gg, bb) in enumerate(table):
        m = sector == i
        rgb[m, 0], rgb[m, 1], rgb[m, 2] = rr[m], gg[m], bb[m]
    rgb += (1 - s)[..., None]   # value 1: what is not hue is white
    rgba = np.dstack([np.clip(rgb, 0, 1), (r <= 1).astype(np.float64)])
    img = Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA")
    return img.resize((size, size), Image.LANCZOS)


def colour_temperature(w, h):
    """The colour temperature slider's track (layout.CONTROL_SLIDER): warm
    (2200 K, at the bottom: the slider's low end) through white to cold (6500 K)."""
    warm, mid, cold = np.array([255, 147, 41]), np.array([255, 236, 210]), np.array([200, 220, 255])
    t = np.linspace(1, 0, h)[:, None] / 1.0   # 1 at the top
    col = np.where(t < 0.5, warm + (mid - warm) * (t / 0.5), mid + (cold - mid) * ((t - 0.5) / 0.5))
    rows = np.repeat(col[:, None, :], w, axis=1) / 255.0
    return to_rgb565(rows)


def button_tile(color):
    k = 4  # drawn at 4x for smooth corners
    img = Image.new("RGBA", (100 * k, 100 * k), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle((2 * k, 2 * k, 98 * k - 1, 98 * k - 1), radius=20 * k, fill=color + (255,))
    return img.resize((100, 100), Image.LANCZOS)


# --------------------------------------------------------------------- main --


def main():
    sheet = "--sheet" in sys.argv
    os.makedirs(os.path.join(HERE, "backgrounds"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "weather"), exist_ok=True)

    backgrounds = {}
    for key, (title, fn) in BACKGROUNDS.items():
        img = to_rgb565(np.clip(fn(), 0, 1))
        img.save(os.path.join(HERE, "backgrounds", key + ".png"), optimize=True)
        backgrounds[key] = img
        print(f"background {key}: {title}")

    shutil.rmtree(os.path.join(HERE, "weather"), ignore_errors=True)
    os.makedirs(os.path.join(HERE, "weather"))
    icons = weather_icons()
    for key, img in icons.items():
        img.save(os.path.join(HERE, "weather", key + ".png"), optimize=True)
    print(f"weather: {len(os.listdir(os.path.join(HERE, 'weather')))} icons (Meteocons)")

    os.makedirs(os.path.join(HERE, "tiles"), exist_ok=True)
    for key, color in TILES.items():
        button_tile(color).save(os.path.join(HERE, "tiles", key + ".png"), optimize=True)
        _, _, w, h = layout.SETTINGS_BUTTON
        wide_tile(color, w, h, layout.SETTINGS_BUTTON_RADIUS).save(
            os.path.join(HERE, "tiles", "wide_" + key + ".png"), optimize=True)
        # the light and cover pages' column of tiles: the button tiles smaller, their corners in proportion
        side = layout.SIDE_TILE
        wide_tile(color, side, side, round(20 * side / 100)).save(
            os.path.join(HERE, "tiles", "side_" + key + ".png"), optimize=True)
    print("button tiles")

    os.makedirs(os.path.join(HERE, "controls"), exist_ok=True)
    colour_wheel(layout.COLOUR_WHEEL[2]).save(os.path.join(HERE, "controls", "wheel.png"), optimize=True)
    _, _, w, h = layout.CONTROL_SLIDER
    colour_temperature(w, h).save(os.path.join(HERE, "controls", "ctemp.png"), optimize=True)
    print("colour wheel, colour temperature track")

    os.makedirs(os.path.join(HERE, "logo"), exist_ok=True)
    logo().save(os.path.join(HERE, "logo", "boot.png"), optimize=True)
    print("logo")

    if sheet:
        out = Image.new("RGB", (3 * (W + 8) + 8, 2 * (H + 8) + 8), (60, 60, 60))
        for i, img in enumerate(backgrounds.values()):
            out.paste(img, (8 + (i % 3) * (W + 8), 8 + (i // 3) * (H + 8)))
        out.save(os.path.join(HERE, "sheet-backgrounds.png"))

        # The icons as the panel shows them: on a background.
        bg = backgrounds["aurora"].convert("RGBA")
        cols = 6
        rows = math.ceil(len(icons) / cols)
        out = Image.new("RGBA", (cols * 120 + 10, rows * 120 + 10), (0, 0, 0, 255))
        for i, icon in enumerate(icons.values()):
            x, y = 10 + (i % cols) * 120, 10 + (i // cols) * 120
            out.paste(Image.alpha_composite(bg.crop((40, 60, 140, 160)), icon.resize((100, 100))), (x, y))
        out.convert("RGB").save(os.path.join(HERE, "sheet-weather.png"))
        print("contact sheets written")


if __name__ == "__main__":
    main()
