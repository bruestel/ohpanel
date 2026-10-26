#!/usr/bin/env python3
"""Put the icons and tiles onto every background, for a display that knows no
transparency.

The Nextion Discovery display draws a picture as an opaque rectangle; a
transparent pixel of a PNG becomes a colour. So every picture that has to look
transparent -- a weather icon, the rounded tile behind a button -- is stored
once per background, with exactly the part of the background it covers baked
in. The firmware picks the variant of the background in use (the pictures of
boards/nspanel/nextion/display-manifest.json).

    python3 boards/nspanel/nextion/assets/compose.py

Slots (picture objects that show these pictures), their positions from
boards/nspanel/nextion/layout.py, which the recipe (boards/nspanel/nextion/ohpanel_hmi.py) places them by:

    weather_home    home.weather_pic          the weather icons of weather-icons.json (layout.WEATHER_HOME)
    weather_page    weather.weather_pic       the same (layout.WEATHER_PAGE)
    button1..8      buttons.tileN_pic         the tiles off/on (assets/<variant>/tiles)
    close           <page>.close              the close icon "x" (drawn here, layout.CLOSE_ICON)
    badge1, badge2  home.infoN_badge(ico)     the info lines' left field as a badge, per palette colour
    restart         settings.restart          the settings page's restart button off/on (layout.SETTINGS_BUTTON)
    card1..8        system.cardN_pic          the panel info's cards (layout.SYSTEM_CARDS), the entity cards too
    rowcard1..7     entities_rows.rowN_pic    the entity page's row cards (layout.ENTITY_ROWS_*)
    wchip1..6       weather.chipN_pic         the weather page's cards of an extra (layout.WEATHER_CHIPS)
    logo            boot.logo_pic             the boot page's logo (assets/<variant>/logo, layout.BOOT_LOGO)
    splash          splash.splash_pic         the display's own logo when it starts, on black only (layout.SPLASH_LOGO)
    art             media.art_pic             the box behind what plays (layout.MEDIA_ART)
    vol             media.volume_pic          the volume button, a disc (layout.MEDIA_VOLUME)
    fav             media.fav_pic             the favourites button, a disc (layout.MEDIA_FAV)
    side1..3        light/cover.sideN_pic     their column of tiles off/on (layout.SIDE_TILES)
    power           light.power_pic           the light's on / off tile (layout.POWER_TILE)
    wheel           light.wheel_pic           the colour wheel (assets/<variant>/controls, layout.COLOUR_WHEEL)
    ctemp           light.temp_slider         the colour temperature slider's track (opaque: the same on all)

Writes assets/composites/<background>.<slot>.<picture>.png, rounded to the
display's 16-bit colours, for boards/nspanel/nextion/tools/import-pictures.sh.
"""

import glob
import json
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
NEXTION = os.path.dirname(HERE)
sys.path.insert(0, NEXTION)
import layout  # noqa: E402

OUT = os.path.join(HERE, "composites")

# The close icon: a 25 px disc with a 12 px cross of 2 px strokes.
CLOSE_DISC = (41, 44, 41, 255)
CLOSE_CROSS = (173, 174, 173, 255)

def fit_icon(img, w, h, margin=2, bottom=False):
    """An icon cut to what it draws and scaled to fill a w x h place (its
    proportions kept, centred -- or standing on the bottom, `bottom` -- with
    `margin` px free around): the icons are drawn with room around them in
    their grid, which wastes their place."""
    box = img.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    if box:
        img = img.crop(box)
    k = min((w - 2 * margin) / img.width, (h - 2 * margin) / img.height)
    icon = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.alpha_composite(icon, ((w - icon.width) // 2, h - margin - icon.height if bottom else (h - icon.height) // 2))
    return out


# The display's palette (components/app_display/src/Format.cpp kPalette), RGB565.
PALETTE = {"white": 65535, "black": 0, "red": 63488, "green": 21920, "blue": 1055, "yellow": 65472,
           "brown": 33280, "grey": 33808, "light-grey": 52857, "dark-grey": 10597}


def badge(color565):
    """The left field of an info line as a badge: a rounded box in the colour,
    a pixel clear of the field's top and bottom."""
    w, h, k = layout.INFO_LEFT[1], layout.INFO_H, 8
    rgb = ((color565 >> 11) * 255 // 31, ((color565 >> 5) & 63) * 255 // 63, (color565 & 31) * 255 // 31)
    img = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle((0, k, w * k - 1, (h - 1) * k - 1), radius=layout.BADGE_RADIUS * k,
                                          fill=rgb + (255,))
    return img.resize((w, h), Image.LANCZOS)


def weather_icons():
    """The icon names the icon sets use: only those are composed."""
    names = set()
    for icon_set in json.load(open(os.path.join(NEXTION, "weather-icons.json"))).values():
        names |= set(icon_set["codes"].values()) | {icon_set["default"]}
    return sorted(names)


def close_icon():
    """The disc with the cross, drawn 8x larger and scaled down (anti-aliased)."""
    n, k = layout.CLOSE_DISC, 8
    img = Image.new("RGBA", (n * k, n * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((0, 0, n * k - 1, n * k - 1), fill=CLOSE_DISC)
    c, arm = n * k / 2, 5.5 * k
    for dx in (-arm, arm):
        d.line((c - dx, c - arm, c + dx, c + arm), fill=CLOSE_CROSS, width=round(1.9 * k))
    return img.resize((n, n), Image.LANCZOS)


def rounded_box(w, h, r, colour):
    """A rounded box, drawn 8x larger and scaled down (anti-aliased)."""
    k = 8
    img = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle((0, 0, w * k - 1, h * k - 1), radius=r * k, fill=colour)
    return img.resize((w, h), Image.LANCZOS)


def slots():
    """Slot name -> (x, y, w, h, {picture name: RGBA image})."""
    weather = {n: Image.open(os.path.join(HERE, "weather", n + ".png")).convert("RGBA") for n in weather_icons()}
    tiles = {n: Image.open(os.path.join(layout.ASSETS, "tiles", n + ".png")).convert("RGBA") for n in ("off", "on")}

    out = {"weather_home": (*layout.WEATHER_HOME, weather), "weather_page": (*layout.WEATHER_PAGE, weather)}
    for i, (x, y) in enumerate(layout.button_tiles(), start=1):
        out[f"button{i}"] = (x, y, layout.TILE_W, layout.TILE_H, tiles)
    out["close"] = (*layout.CLOSE_ICON, {"x": close_icon()})
    wide = {n: Image.open(os.path.join(layout.ASSETS, "tiles", "wide_" + n + ".png")).convert("RGBA") for n in ("off", "on")}
    out["restart"] = (*layout.SETTINGS_BUTTON, wide)
    card = {"box": rounded_box(layout.SYSTEM_CARD_W, layout.SYSTEM_CARD_H, layout.SYSTEM_CARD_RADIUS, (44, 44, 44, 255))}
    for i, (x, y) in enumerate(layout.SYSTEM_CARDS, start=1):
        out[f"card{i}"] = (x, y, layout.SYSTEM_CARD_W, layout.SYSTEM_CARD_H, card)
    chip = {"box": rounded_box(layout.WEATHER_CHIP_W, layout.WEATHER_CHIP_H, layout.WEATHER_CHIP_RADIUS, (44, 44, 44, 255))}
    for i, (x, y) in enumerate(layout.WEATHER_CHIPS, start=1):
        out[f"wchip{i}"] = (x, y, layout.WEATHER_CHIP_W, layout.WEATHER_CHIP_H, chip)
    row_w = layout.VISIBLE_W - 2 * layout.MARGIN
    rowcard = {"box": rounded_box(row_w, layout.ENTITY_ROWS_H, layout.ENTITY_ROWS_RADIUS, (44, 44, 44, 255))}
    for i in range(layout.ENTITY_ROWS_N):
        out[f"rowcard{i + 1}"] = (layout.MARGIN, layout.ENTITY_ROWS_Y + i * layout.ENTITY_ROWS_STEP, row_w,
                                  layout.ENTITY_ROWS_H, rowcard)
    tile = (44, 44, 44, 255)   # the inactive tile's flat colour (generate.py TILES["off"]): an icon on it takes it as its own
    box = layout.MEDIA_ART
    out["art"] = (*box, {"box": rounded_box(box[2], box[3], layout.ART_RADIUS, tile)})
    for slot, box in (("vol", layout.MEDIA_VOLUME), ("fav", layout.MEDIA_FAV)):
        out[slot] = (*box, {"box": rounded_box(box[2], box[3], box[3] // 2, layout.PILL_COLOUR + (255,))})
    side = {n: Image.open(os.path.join(layout.ASSETS, "tiles", "side_" + n + ".png")).convert("RGBA") for n in ("off", "on")}
    for i, (x, y) in enumerate(layout.SIDE_TILES, start=1):
        out[f"side{i}"] = (x, y, layout.SIDE_TILE, layout.SIDE_TILE, side)
    out["power"] = (*layout.POWER_TILE, layout.SIDE_TILE, layout.SIDE_TILE, side)
    out["wheel"] = (*layout.COLOUR_WHEEL,
                    {"disc": Image.open(os.path.join(layout.ASSETS, "controls", "wheel.png")).convert("RGBA")})
    out["ctemp"] = (*layout.CONTROL_SLIDER,
                    {"track": Image.open(os.path.join(layout.ASSETS, "controls", "ctemp.png")).convert("RGBA")})
    out["logo"] = (*layout.BOOT_LOGO, {"boot": Image.open(os.path.join(layout.ASSETS, "logo", "boot.png")).convert("RGBA")})
    out["splash"] = (*layout.SPLASH_LOGO,
                     {"logo": Image.open(os.path.join(layout.ASSETS, "logo", "splash.png")).convert("RGBA")})
    badges = {name: badge(c) for name, c in PALETTE.items()}
    for line, y in enumerate(layout.INFO_Y, start=1):
        out[f"badge{line}"] = (layout.INFO_LEFT[0], y, layout.INFO_LEFT[1], layout.INFO_H, badges)
    return out


def rgb565(img):
    """Rounded (not dithered) to the display's colours: the background part is
    already exactly in them, and a flat tile must stay one colour."""
    a = np.asarray(img.convert("RGB"), dtype=np.float64) / 255.0
    for c, levels in enumerate((31, 63, 31)):
        a[:, :, c] = np.round(a[:, :, c] * levels) / levels
    return Image.fromarray((a * 255 + 0.5).astype(np.uint8), "RGB")


# The display's own start (the splash page, before the firmware has found it) is black whatever the background
# configured: its logo only on that.
SPLASH_ONLY, SPLASH_BACKGROUND = ("splash",), "black"


def main():
    if len(sys.argv) != 1:
        sys.exit(__doc__)
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    count = 0
    for bg_path in sorted(glob.glob(os.path.join(layout.ASSETS, "backgrounds", "*.png"))):
        bg_name = os.path.splitext(os.path.basename(bg_path))[0]
        bg = Image.open(bg_path).convert("RGBA")
        for slot, (x, y, w, h, pictures) in slots().items():
            if slot in SPLASH_ONLY and bg_name != SPLASH_BACKGROUND:
                continue
            under = bg.crop((x, y, x + w, y + h))
            for name, picture in pictures.items():
                if picture.size != (w, h):
                    if not slot.startswith("weather"):
                        sys.exit(f"{name}: {picture.size}, slot {slot} is {w}x{h}")
                if slot.startswith("weather"):
                    # drawn large with room around: filling its place; on home standing on its
                    # bottom, where the weather block ends (layout.WEATHER_BOTTOM)
                    picture = fit_icon(picture, w, h, bottom=slot == "weather_home")
                rgb565(Image.alpha_composite(under, picture)).save(
                    os.path.join(OUT, f"{bg_name}.{slot}.{name}.png"), optimize=True)
                count += 1
    print(f"composites: {count} pictures")


if __name__ == "__main__":
    main()
