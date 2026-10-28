#!/usr/bin/env python3
"""Build OHpanel's display design, as code, from an empty project.

    boards/nspanel/nextion/build.sh            the whole pipeline (import pictures, this, compile)
    boards/nspanel/nextion/ohpanel_hmi.py IN.HMI OUT.HMI

IN is the seed (boards/nspanel/nextion/seed/seed.HMI.gz, see seed/README.md:
an empty project of the Nextion Editor for the NX4832F035, turned to 270
degrees as the NSPanel mounts it, with one page holding one object of each
type) with OHpanel's pictures imported (boards/nspanel/nextion/tools/import-pictures.sh);
this script finds their ids itself. Everything else is made here: every page
and object (copies of the seed's, which supply the attributes the Editor
expects), every line of code, the fonts (boards/nspanel/nextion/fonts.py) and
Program.s. Positions come from boards/nspanel/nextion/layout.py, which the
picture composer (boards/nspanel/nextion/assets/compose.py) shares.

It also writes boards/nspanel/nextion/display-manifest-<variant>.json: what the firmware and the web
interface need to know about the built design -- picture ids of the
backgrounds and of the pictures composed onto them, page ids, and per page
the objects that crop from the background and therefore follow a background
change (and whether it has a close icon, whose picture does too).

Object names (the firmware, Engine*.cpp, and the web preview use them): lower
case, "_" between the parts, at most 14 characters; numbered from 1 without a
leading zero (row1, tile8); a group and its part -- "_text" a label, "_value" a
value, "_icon" a field on an icon font, "_pic" a picture, "_hs" a touch area,
"_tm" a timer, "_slider" -- and a single object without a part (clock, date,
dots). On every page alike: title, close and close_hs (the close cross),
swipe and swipe_tm, busy1..3 and busy_tm. An event names what was touched as
its touch area without "_hs" (tile3, shortcut2, close).

The design in short:

- Navigation belongs to the firmware: swipes and close buttons report to the
  ESP, which knows the configured page order (docs/panel-config.md, "swipe")
  and where to go back to; no page opens another.
- Weather icons, button tiles, the close icon and the like once per
  background (boards/nspanel/nextion/assets/compose.py), since the display has no
  transparency; the manifest lists them and the firmware picks the variant of
  the background in use. Text fields crop from the background.
- The background and the theme's colours are global variables of Program.s
  that every page takes when it loads (background_globals(), theme_colours()).
- The firmware dims and blanks the screen itself; "blank" is a black page a
  touch on which only says so.
- A design version (global design_ver in Program.s, also in the manifest), so
  the firmware can tell when the display runs an older design than it was
  built for.
"""

import glob
import hashlib
import math
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
from hmi2layout import picture_previews  # noqa: E402
from hmi2txt import read_container  # noqa: E402
from hmipatch import Project  # noqa: E402

sys.path.insert(0, HERE)
import layout  # noqa: E402
import fonts  # noqa: E402
import zifont  # noqa: E402

BACKGROUNDS = os.path.join(layout.ASSETS, "backgrounds")   # the panel variant's (generate.py)
MANIFEST = os.path.join(HERE, f"display-manifest-{layout.VARIANT}.json")   # the panel variant's (layout.py)

# The background the design ships with; the firmware switches pages to the
# configured one at runtime (page pic and the picc of every cropped object).
DEFAULT_BACKGROUND = "aurora"

# The NSPanel EU's frame hides the rightmost 30 px of the 480x320 display:
# everything that has to be seen ends at x=450 and centres on x=225 (touch
# still works behind the frame).
VISIBLE_W = layout.VISIBLE_W

# The close icon: a picture per background (boards/nspanel/nextion/assets/compose.py, slot
# "close") on the title's margins (boards/nspanel/nextion/layout.py). Its touch area (the
# hotspot) is larger and reaches under the frame.
CLOSE_ICON = layout.CLOSE_ICON
CLOSE_TOUCH = layout.CLOSE_TOUCH         # to the display's edge
WIDE_ROW_MAXL = layout.WIDE_ROW_MAXL     # what fits in the entity rows' wide field

# Colours (RGB565) the design starts with; the firmware sets most of them by
# the theme and the state of what they show.
WHITE = 65535
LIGHT_GREY = 52857                       # rgb(200, 204, 200)
TITLE_GREY = 50712                       # rgb(192, 192, 192)
WIFI_GREY = 33808                        # rgb(128, 128, 128)
TILE_OFF_565, PILL_565 = 10597, 14855    # the art box's and the pill's flat colours (rgb 44,44,44 / 60,66,60)
SLIDER_TRACK = "#3a3f3a"                 # the slider's empty part
TILE_OFF = "#2c2c2c"                     # generate.py TILES: the button's text sits on it

TEXT_FONT, ICON_FONT, ICON_FONT_LARGE = 3, 7, 8   # the icon font at 24 px; font 8: the same glyphs at 32 px
ICON_FONT_XL = 9   # and at 48 px: a button's icon in the middle of its tile

# The seed's objects by type (main(): from the seed's page, which goes once
# every page is built).
TEXT, NUMBER, BUTTON, PICTURE, HOTSPOT, TOUCH, SLIDER, TIMER, VARIABLE = 116, 54, 98, 112, 109, 5, 1, 51, 52
PROGRESS = 106                            # a progress bar: val 0..100, filled in pco on bco
SCROLL = 55                               # scrolling text: the display moves it on by itself
SCROLL_STEP, SCROLL_MS = 3, 80            # 3 px every 80 ms (the least the display takes): 37 px a second
SEED_PAGE = "page0"
DIRECTION = layout.DIRECTION   # the display turned as the panel mounts it (the seed's 270; set again, to be sure)
BAUD = 921600     # the serial link's speed (the firmware's nextion::Config::baud); the TFT upload proved the line at it
TEMPLATES: dict = {}
BG = 0          # the default background's picture id (main())
PICTURES = {}   # the default background's composed pictures: slot -> picture -> id (main())
ALL_PICTURES = {}   # every background's: background -> slot -> picture -> id


def event(page: str, component: str, value: str) -> list[str]:
    """Nextion code that sends one event to the ESP: 0x92, "nspanelevent",
    0x00, a JSON object, 0x00, 0xFF 0xFF 0xFF -- the framing the firmware's
    display driver parses."""
    payload = f'{{\\"page\\": \\"{page}\\", \\"component\\": \\"{component}\\", \\"value\\": \\"{value}\\"}}'
    return ["printh 92", 'prints "nspanelevent",0', "printh 00", f'prints "{payload}",0', "printh 00",
            "printh FF FF FF"]


def report(tag: str, obj: str) -> list[str]:
    """Nextion code that sends a number to the ESP: 0x91, a name of four
    characters, 0x00, the value (4 bytes), 0xFF 0xFF 0xFF."""
    return ["printh 91", f'prints "{tag}",0', "printh 00", f"prints {obj}.val,0", "printh FF FF FF"]


# ------------------------------------------------------------------ objects --

def new(page, kind: int, name: str, box=None, **attrs):
    """A copy of the seed's object of `kind` on `page`, at `box` (x, y, w, h),
    with `attrs` set."""
    o = page.add_copy(TEMPLATES[kind], name)
    if box is not None:
        for attr, v in zip(("x", "y", "w", "h"), box):
            o.set(attr, v)
    for attr, v in attrs.items():
        o.set(attr, v)
    return o


# A text field's room in bytes (txt_maxl) -- what the display keeps of a text; it cuts the rest silently, in the
# middle of a letter's bytes. UTF-8 takes 2 bytes for a letter beyond ASCII (ü, ł, ř, Cyrillic, Greek): a field in
# a text font holds at least BYTES_PER_CHAR times the characters its width takes (an average letter of the font,
# AVERAGE_EM of its em) -- fields meant for words (10 bytes and more; the shorter ones hold numbers, units, times).
# What that costs, the display's memory per page (3584 bytes), was checked for every page.
BYTES_PER_CHAR = 1.6
AVERAGE_EM = 0.45


def room_bytes(box, font: int, maxl: int) -> int:
    """`maxl`, or more where a text the field's width takes would not fit in it."""
    if font in (ICON_FONT, ICON_FONT_LARGE, ICON_FONT_XL) or maxl < 10:   # icons; numbers, units, times
        return maxl
    em = fonts.FONTS[font][2][0][1]
    return max(maxl, math.ceil(BYTES_PER_CHAR * box[2] / (AVERAGE_EM * em)))


def text(page, name: str, box, font: int, *, xcen=0, ycen=1, maxl=20, pco=WHITE, bco=None, pic=None,
         txt: str | bytes = ""):
    """A text field: cropped from the background, or solid in `bco`, or on the
    picture `pic`."""
    o = new(page, TEXT, name, box, font=font, xcen=xcen, ycen=ycen, txt_maxl=room_bytes(box, font, maxl), pco=pco,
            txt=txt)
    if pic is not None:
        o.set("sta", 2).set("pic", pic)
    elif bco is not None:
        o.set("sta", 1).set("bco", bco)
    else:
        o.set("sta", 0).set("picc", BG)
    return o


def scroller(page, name: str, box, font: int, *, pco=WHITE, bco=None, maxl=80):
    """A scrolling text over a text field's box, standing still (en=0) until
    the firmware shows it for a text too long for that field: cropped from the
    background or solid in `bco`, as the field."""
    o = new(page, SCROLL, name, box, font=font, xcen=0, ycen=1, txt_maxl=maxl, pco=pco, txt="", en=0,
            dis=SCROLL_STEP, tim=SCROLL_MS)
    if bco is not None:
        o.set("sta", 1).set("bco", bco)
    else:
        o.set("sta", 0).set("picc", BG)
    return o


def picture(page, name: str, box, pic: int):
    return new(page, PICTURE, name, box, pic=pic)


def hotspot(page, name: str, box, release: list[str] | None = None, press: list[str] | None = None):
    o = new(page, HOTSPOT, name, box)
    if press:
        o.event("codesdown", press)
    if release:
        o.event("codesup", release)
    return o


def progress(page, name: str, box, *, pco: int, bco: int):
    """A progress bar in solid colours (sta 0), filled from the left; the firmware sets its value and colours."""
    return new(page, PROGRESS, name, box, sta=0, dez=0, val=0, pco=pco, bco=bco)


def timer(page, name: str, ms: int, code: list[str], enabled=False):
    return new(page, TIMER, name, tim=ms, en=int(enabled)).event("codestimer", code)


def slider(page, name: str, box, tag: str, *, upright=False, handle=layout.SLIDER_HANDLE):
    """A slider in the theme's colours (set when the page loads): a narrow bar,
    the empty part dark, the filled part the accent, a slim light handle across
    it. It reports as it moves (tag + "s") and when let go (tag + "u") -- or,
    for a tag of four characters, as that both times."""
    x, y, w, h = box
    o = new(page, SLIDER, name, box, mode=int(upright), sta=1, psta=0, bco=rgb565(SLIDER_TRACK))
    o.set("wid", w if upright else handle).set("hig", handle if upright else h)
    moving, up = (tag, tag) if len(tag) == 4 else (tag + "s", tag + "u")
    o.event("codesslide", report(moving, name)).event("codesup", report(up, name))
    return o


# Icons lined up by what shows of them: the display centres a text field's
# characters by their cell (advance and font height), and where an icon's ink
# lies in its cell differs from icon to icon and font to font. place_icon()
# puts a field so that the ink of `glyph` in that font has its middle on a
# point; check_icons() measures every one of them again once all steps have
# run and stops the build if one is more than ICON_SLACK px off.
CENTRED: list = []   # (page, object, x, y, font, glyph)
ICON_SLACK = 1


def place_icon(obj, cx: float, cy: float, font: int, glyph: str):
    """`obj` (a text field) on `font`, centred, so that `glyph`'s ink is
    centred on (cx, cy) -- its box 4 px wider than the glyph's advance and as
    high as the font, so the display's centring halves an even number."""
    advance, height, x0, y0, x1, y1 = fonts.icon_ink(font, glyph)
    w, h = advance + 4, height
    x = round(cx - (w - advance) / 2 - (x0 + x1) / 2)
    y = round(cy - (h - height) / 2 - (y0 + y1) / 2)
    obj.set("x", x).set("y", y).set("w", w).set("h", h).set("font", font).set("xcen", 1).set("ycen", 1)
    CENTRED.append((obj._page.name, obj.name, cx, cy, font, glyph))
    return obj


def icon(page, name: str, cx: float, cy: float, font: int, glyph="circle", **kw):
    """A text field for an icon the firmware writes, centred on (cx, cy) by
    the ink of `glyph` (a circle stands for any icon)."""
    return place_icon(text(page, name, (0, 0, 1, 1), font, **kw), cx, cy, font, glyph)


def check_icons(p: Project) -> None:
    for page, name, cx, cy, font, glyph in CENTRED:
        o = p.page(page).obj(name)
        advance, height, x0, y0, x1, y1 = fonts.icon_ink(font, glyph)
        if o.get("font") != font or o.get("xcen") != 1 or o.get("ycen") != 1:
            sys.exit(f"{page}.{name}: no longer centred on font {font}")
        ink_x = o.get("x") + (o.get("w") - advance) // 2 + (x0 + x1) / 2
        ink_y = o.get("y") + (o.get("h") - height) // 2 + (y0 + y1) / 2
        if abs(ink_x - cx) > ICON_SLACK or abs(ink_y - cy) > ICON_SLACK:
            sys.exit(f"{page}.{name}: {glyph} centred on ({ink_x}, {ink_y}), wanted ({cx}, {cy})")
    print(f"icons: {len(CENTRED)} centred within {ICON_SLACK} px")


# -------------------------------------------------------------------- pages --

def new_page(p: Project, name: str, *, background=True, swipe=True, touch=False):
    """A page on the configurable background (or black), with the swipe
    detection (swipe_detection()) and, with `touch`, a touch on the page
    itself reported ("touch": the panel is in use)."""
    page = p.add_page(name, p.page(SEED_PAGE))
    root = page.objects()[0]
    if background:
        root.set("sta", 2).set("pic", BG)
    else:
        root.set("sta", 1).set("bco", 0)
    if touch:
        root.event("codesdown", event(name, "touch", "press")).event("codesup", event(name, "touch", "release"))
    if swipe:
        swipe_detection(page)
    return page


SWIPE_MIN = 100   # px the finger moves for a swipe
SWIPE_TICK_MS = 50


def swipe_detection(page) -> None:
    """A swipe anywhere on the page, reported with the direction the finger
    moved ("left", "right", "up", "down"): a touch area over the whole page
    ("swipe") and a timer ("swipe_tm") that notes where the finger is while it touches
    (swipex, swipey in Program.s) -- when it lifts, the display still knows
    where it came down (tch2, tch3)."""
    name = page.name

    code = ["swipe_tm.en=0", "sys0=swipex-tch2", "sys1=swipey-tch3"]
    for k, (test, value) in enumerate(((f"sys0>{SWIPE_MIN}", "right"), (f"sys0<-{SWIPE_MIN}", "left"),
                                       (f"sys1>{SWIPE_MIN}", "down"), (f"sys1<-{SWIPE_MIN}", "up"))):
        code += [f"if({test})" if k == 0 else f"}}else if({test})", "{"]
        code += ["  " + line for line in event(name, "swipe", value)]
    new(page, TOUCH, "swipe").event("codesdown", ["swipex=tch0", "swipey=tch1", "swipe_tm.en=1"]).event(
        "codesup", code + ["}"])
    timer(page, "swipe_tm", SWIPE_TICK_MS, ["swipex=tch0", "swipey=tch1"])


def title(page, *, pco=TITLE_GREY):
    """The page's title, top left."""
    return text(page, "title", layout.TITLE, TEXT_FONT, ycen=2, maxl=30, pco=pco)


def close(page) -> None:
    """The close cross, top right: its touch area (to the display's edge) and
    the picture (the variant of the background in use, Program.s bg_close) on
    top of it -- which, being on top, takes the touch and carries the event
    too."""
    release = event(page.name, "close", "release")
    hotspot(page, "close_hs", CLOSE_TOUCH, release)
    picture(page, "close", CLOSE_ICON, PICTURES["close"]["x"]).event("codesup", release)


def on_load(page, lines: list[str]) -> None:
    """Appends code to the page's preinitialize event."""
    root = page.objects()[0]
    root.event("codesload", root.code("codesload") + lines)


def loaded(page) -> list[str]:
    """The event that tells the firmware the page has loaded."""
    return event(page.name, "currentpage", "pagechange")


# The unit fields after a number (home_page): prefix -> the number's x, a
# digit's width, how many places, the number's font; in the manifest ("units").
UNITS = {}
# Today's low and high (home_page): the places' step, how many, the font; in
# the manifest ("range").
RANGE = {}


def home_page(p: Project) -> None:
    """Home. The left half from the left margin (boards/nspanel/nextion/layout.py):
    the status icons, the weather icon (and a hotspot over it) with the
    outdoor temperature beside it and today's low and high under that, two
    value rows (an icon and a value each), the info lines. The right half:
    a value and the WiFi icon at the end of the status row, the clock, the date
    and the shortcuts right-aligned at the right margin. At the bottom the
    relays' labels with a bar each.

    The left field of an info line also comes as an icon (suffix "_ico", the
    24 px icon font) and both as a badge (suffix "b": on a picture of a
    rounded box in a colour, compose.py slots badge1, badge2); the firmware
    shows one."""
    home = new_page(p, "home")
    x, y, w, h = layout.CLOCK
    text(home, "clock", layout.CLOCK, 6, xcen=2, maxl=5)
    text(home, "date", layout.DATE, TEXT_FONT, xcen=2, ycen=2, maxl=25)
    # AM/PM small above the clock's minutes
    my, mw, mh = layout.MERIDIEM
    text(home, "ampm", (layout.RIGHT - mw, my, mw, mh), 2, xcen=2, ycen=2, maxl=10)

    for k, (sx, sy) in enumerate(layout.STATUS_ICONS, start=1):
        text(home, f"top{k}", (sx, sy, 25, 25), ICON_FONT, xcen=1, maxl=10, pco=LIGHT_GREY)
    text(home, "top_value", layout.STATUS_VALUE, TEXT_FONT, xcen=2, maxl=10, pco=LIGHT_GREY)
    text(home, "top_icon", layout.STATUS_VALUE_ICON, ICON_FONT_LARGE, xcen=1, maxl=10, pco=LIGHT_GREY)
    text(home, "wifi", layout.WIFI_ICON, ICON_FONT, xcen=1, maxl=5, pco=WIFI_GREY)

    weather_pics = PICTURES["weather_home"]
    picture(home, "weather_pic", layout.WEATHER_HOME, weather_pics[sorted(weather_pics)[0]])

    def units(number, prefix, steps, digit, y, h, w, font):
        """The unit's places after `number`, `digit` apart: prefix1 .. prefixN
        ("row1_u1" ...), and for each a copy of `number` that ends where the
        unit begins ("row1_n1" ...): a field repaints its whole box when its
        text changes, and the wide one would paint over the unit (the firmware
        shows a pair, or the wide field for a text without a unit)."""
        x0, numbers = number.get("x"), prefix[:-1] + "n"
        for k in range(1, steps + 1):
            home.clone(number.name, f"{numbers}{k}").set("w", k * digit + 2)
            u = home.clone(number.name, f"{prefix}{k}")
            u.set("x", x0 + k * digit + 2).set("y", y).set("w", w).set("h", h)
            u.set("font", font).set("xcen", 0).set("ycen", 0)
        UNITS[prefix] = {"x": x0, "step": digit, "n": steps, "font": number.get("font"),
                         "field": number.name, "numbers": numbers}

    # the outdoor temperature: the number large beside the icon, its unit small at its top
    outdoor = text(home, "outdoor", layout.OUTDOOR_TEMP, layout.FONT_OUTDOOR, maxl=8)
    units(outdoor, "outdoor_u", layout.OUTDOOR_UNIT_STEPS, layout.OUTDOOR_UNIT_STEP, layout.OUTDOOR_UNIT_Y,
          layout.OUTDOOR_UNIT_H, 34, layout.FONT_OUTDOOR_UNIT)

    # today's low and high under it, on the date's baseline: "↓9° ↑22°"
    def arrow(name, x, glyph):
        # a glyph of the icon font (boards/nspanel/nextion/icons.json), UTF-8
        return text(home, name, (x, layout.RANGE_ARROW_Y, layout.RANGE_ARROW_W, 24), ICON_FONT, xcen=1, maxl=10,
                    txt=bytes.fromhex(glyph))

    def value(name, x, w):
        return text(home, name, (x, layout.RANGE_Y, w, layout.RANGE_H), layout.FONT_RANGE, ycen=2, maxl=25)

    # Material Design Icons arrow-down-thin / arrow-up-thin: as high as the small digits
    made = [arrow("low_arrow", layout.RANGE_DOWN_X, "EFA6B2")]
    for k in range(0, layout.RANGE_STEPS + 1):
        if k:
            made.append(value(f"low{k}", layout.RANGE_LOW_X, k * layout.RANGE_STEP))
        x = layout.RANGE_DOWN_X if k == 0 else layout.RANGE_LOW_X + k * layout.RANGE_STEP + layout.RANGE_GAP
        made.append(arrow(f"high_arrow{k}", x, "EFA6B1"))
        made.append(value(f"high{k}", x + layout.RANGE_ARROW_W, layout.RANGE_HIGH_W))
    assert made[-1].get("x") + made[-1].get("w") <= layout.RANGE_END, "the high reaches into the date"
    RANGE.update({"step": layout.RANGE_STEP, "n": layout.RANGE_STEPS, "font": layout.FONT_RANGE})

    # value rows: an icon and the number beside it, its unit small at the number's top
    for (icon_name, number, row), cy in zip((("row1_icon", "row1", "row1"), ("row2_icon", "row2", "row2")),
                                            layout.VALUE_ROWS):
        text(home, icon_name, (layout.VALUE_ICON_X, cy - 12, 25, 25), ICON_FONT, xcen=1, maxl=10)
        num = text(home, number, (layout.VALUE_NUMBER[0], cy - layout.VALUE_NUMBER_H // 2, layout.VALUE_NUMBER[1],
                                  layout.VALUE_NUMBER_H), layout.FONT_VALUE, maxl=8)
        units(num, row + "_u", layout.VALUE_UNIT_STEPS, layout.VALUE_UNIT_STEP, cy - 13, 20, layout.VALUE_UNIT_W,
              layout.FONT_VALUE_UNIT)

    # the info lines: a left field (a line number, mostly; centred, as its badge), the main text, a right one
    for line, y in enumerate(layout.INFO_Y, start=1):
        lx, lw = layout.INFO_LEFT
        rx, rw = layout.INFO_RIGHT
        text(home, f"info{line}_text", (layout.INFO_MAIN_X, y, rx - layout.INFO_GAP - layout.INFO_MAIN_X,
                                          layout.INFO_H), 1, maxl=20, pco=LIGHT_GREY)
        text(home, f"info{line}_tag", (lx, y, lw, layout.INFO_H), 1, xcen=1, maxl=5)
        text(home, f"info{line}_right", (rx, y, rw, layout.INFO_H), 1, xcen=1, maxl=5, pco=LIGHT_GREY)
        text(home, f"info{line}_tagicon", (lx, y, lw, layout.INFO_H), ICON_FONT, xcen=1, maxl=5)
        # the badges: the default background's red box to start with; the firmware sets the colour and background
        badge = PICTURES[f"badge{line}"]["red"]
        for name, font in ((f"info{line}_badge", 1), (f"info{line}_badgeico", ICON_FONT)):
            text(home, name, (lx, y, lw, layout.INFO_H), font, xcen=1, maxl=5, pic=badge)

    # the relays: a label, and under it a bar the firmware colours (the theme's accent while the relay is on,
    # dark grey while off: Engine::draw_relays)
    for k, (x, y, w, h) in enumerate(layout.RELAYS, start=1):
        text(home, f"relay{k}_text", (x, y, w, h), 1, xcen=1, maxl=20, pco=WHITE if k == 1 else LIGHT_GREY)
        text(home, f"relay{k}_bar", (x, y + layout.RELAY_BAR_DY, w, layout.RELAY_BAR_H), 1, maxl=1,
             bco=TILE_OFF_565)

    # the shortcuts, and the touch areas over them and the weather icon
    for n, (x, y) in enumerate(layout.SHORTCUTS, start=1):
        text(home, f"shortcut{n}_icon", (x, y, layout.SHORTCUT, layout.SHORTCUT), ICON_FONT_LARGE, xcen=1, maxl=10,
             pco=LIGHT_GREY)
    hotspot(home, "weather_hs", layout.WEATHER_HOME, event("home", "weather", "release"),
            event("home", "weather", "press"))
    for n, (x, y) in enumerate(layout.SHORTCUTS, start=1):
        hotspot(home, f"shortcut{n}_hs", (x, y, layout.SHORTCUT, layout.SHORTCUT),
                event("home", f"shortcut{n}", "release"))
    # everything hidden while the page loads: the firmware shows what is configured
    on_load(home, loaded(home) + ["vis 255,0"])


def weather_page(p: Project) -> None:
    """The days of the forecast (page type "weather", boards/nspanel/nextion/layout.py
    WEATHER_*), a sheet each (swiped left and right, dots at the bottom): the
    day and its date as the title, the icon (a picture per background), the
    high large beside it and the low with its arrow under it -- or, with the
    condition in words, the high smaller with the low right after it and the
    condition large under them --, six cards of an
    extra -- an icon, the value, a small label -- on their pictures."""
    page = new_page(p, "weather", touch=True)
    title(page)
    pics = PICTURES["weather_page"]
    picture(page, "weather_pic", layout.WEATHER_PAGE, pics[sorted(pics)[0]])
    text(page, "high", layout.WEATHER_HIGH, 5, ycen=1, maxl=12)
    # Material Design Icons arrow-down-thin, as on home
    text(page, "low_arrow", layout.WEATHER_LOW_ARROW, ICON_FONT, xcen=1, maxl=10, pco=LIGHT_GREY, txt=bytes.fromhex("EFA6B2"))
    text(page, "low", layout.WEATHER_LOW, TEXT_FONT, ycen=1, maxl=12, pco=LIGHT_GREY)
    # with the condition: the high smaller, the low after it (a place per width), the condition under them
    text(page, "high_line", layout.WEATHER_LINE_HIGH, 4, ycen=2, maxl=12)
    for k in range(1, layout.WEATHER_LINE_STEPS + 1):
        x = layout.WEATHER_LINE_LOW_X0 + (k - 1) * layout.WEATHER_LINE_STEP
        text(page, f"low_arrow{k}", (x, layout.WEATHER_LINE_LOW_Y + 4, 26, layout.WEATHER_LINE_LOW_H - 4), ICON_FONT,
             xcen=1, ycen=2, maxl=10, pco=LIGHT_GREY, txt=bytes.fromhex("EFA6B2"))
        text(page, f"low{k}", (x + 26, layout.WEATHER_LINE_LOW_Y, layout.WEATHER_LINE_LOW_W, layout.WEATHER_LINE_LOW_H),
             TEXT_FONT, ycen=2,
             maxl=12, pco=LIGHT_GREY)
    text(page, "condition", layout.WEATHER_CONDITION, TEXT_FONT, ycen=1, maxl=48)
    w, h = layout.WEATHER_CHIP_W, layout.WEATHER_CHIP_H
    for k, (x, y) in enumerate(layout.WEATHER_CHIPS, start=1):
        c = f"chip{k}"
        picture(page, f"{c}_pic", (x, y, w, h), PICTURES[f"wchip{k}"]["box"])
        icon(page, f"{c}_icon", x + 20, y + h / 2, ICON_FONT, maxl=4, bco=TILE_OFF_565)
        text(page, f"{c}_value", (x + 38, y + 6, w - 44, 26), 2, maxl=16, bco=TILE_OFF_565)
        text(page, f"{c}_text", (x + 38, y + 32, w - 44, 18), 0, maxl=16, pco=LIGHT_GREY, bco=TILE_OFF_565)
    text(page, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    close(page)
    # everything hidden while the page loads: the firmware shows what the day has; the title and the cross at once
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


def entities_pages(p: Project) -> None:
    """The entity page (page type "entities", boards/nspanel/nextion/layout.py ENTITY_*)
    in three styles, a page each -- the firmware opens the one the page's
    "style" names (Engine::template_of):

    entities        the list: per row an icon, the label and the value
                    right-aligned (or one wide field, rowN_wide), a thin line
                    under it (rowN_line); for a narrow value a longer label
                    (rowN_longtext) beside a narrow value field (rowN_shortval)
                    -- the firmware shows the pair the value fits;
    entities_cards  the panel info's cards: per card an icon, a small label and
                    the value;
    entities_rows   a card per row (its picture per background): an icon, the
                    label and the value right-aligned -- or the longer label
                    and the narrow value, as the list --, or one wide field.

    Each has dots at the bottom for the sheets, where there are more rows than
    one sheet holds (swiped left and right).

    The display cannot move objects, so a page holds each layout it shows."""
    tx, vw, font = layout.ENTITY_TEXT_X, layout.ENTITY_VALUE_W, layout.ENTITY_FONT
    nw = layout.ENTITY_VALUE_NARROW
    line = rgb565("#%02x%02x%02x" % layout.ENTITY_LINE)

    page = new_page(p, "entities", touch=True)
    title(page)
    text(page, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    h = layout.ENTITY_LIST_H
    for r in range(1, layout.ENTITY_LIST_N + 1):
        y = layout.ENTITY_LIST_Y + (r - 1) * layout.ENTITY_LIST_STEP
        icon(page, f"row{r}_icon", layout.ENTITY_ICON_X, y + h / 2, ICON_FONT_LARGE, maxl=4)
        text(page, f"row{r}_text", (tx, y, layout.RIGHT - vw - tx, h), font, maxl=24, pco=LIGHT_GREY)
        text(page, f"row{r}_value", (layout.RIGHT - vw, y, vw, h), font, xcen=2, maxl=24)
        text(page, f"row{r}_longtext", (tx, y, layout.RIGHT - nw - tx, h), font, maxl=40, pco=LIGHT_GREY)
        text(page, f"row{r}_shortval", (layout.RIGHT - nw, y, nw, h), font, xcen=2, maxl=24)
        text(page, f"row{r}_wide", (tx, y, layout.RIGHT - tx, h), font, maxl=WIDE_ROW_MAXL)
        scrolling_row(page, r, tx, y, h, layout.RIGHT, vw, font)
        text(page, f"row{r}_line", (tx, y + h - 1, layout.RIGHT - tx, 2), font, maxl=1, bco=line)   # 2 px: the least the editor takes
    close(page)
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])

    page = new_page(p, "entities_cards", touch=True)
    title(page)
    text(page, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    w, h, ctx = layout.SYSTEM_CARD_W, layout.SYSTEM_CARD_H, layout.SYSTEM_TEXT_X
    for k, (x, y) in enumerate(layout.SYSTEM_CARDS, start=1):
        c = f"card{k}"
        picture(page, f"{c}_pic", (x, y, w, h), PICTURES[f"card{k}"]["box"])
        part = card_parts(x, y, w)
        icon(page, f"{c}_icon", x + 24, y + h / 2, ICON_FONT_LARGE, maxl=4, bco=TILE_OFF_565)
        text(page, f"{c}_text", part("text"), layout.SYSTEM_TEXT_FONT, maxl=24, pco=LIGHT_GREY, bco=TILE_OFF_565)
        text(page, f"{c}_value", part("value"), layout.SYSTEM_VALUE_FONT, maxl=24, bco=TILE_OFF_565)
    close(page)
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])

    page = new_page(p, "entities_rows", touch=True)
    title(page)
    text(page, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    h, row_w = layout.ENTITY_ROWS_H, VISIBLE_W - 2 * layout.MARGIN
    right = layout.RIGHT - 10   # the value inside the card
    for r in range(1, layout.ENTITY_ROWS_N + 1):
        y = layout.ENTITY_ROWS_Y + (r - 1) * layout.ENTITY_ROWS_STEP
        picture(page, f"row{r}_pic", (layout.MARGIN, y, row_w, h), PICTURES[f"rowcard{r}"]["box"])
        icon(page, f"row{r}_icon", layout.ENTITY_ICON_X + 2, y + h / 2, ICON_FONT, maxl=4, bco=TILE_OFF_565)
        text(page, f"row{r}_text", (tx, y + 1, right - (vw - 10) - tx, h - 2), font, maxl=24, pco=LIGHT_GREY,
             bco=TILE_OFF_565)
        text(page, f"row{r}_value", (right - (vw - 10), y + 1, vw - 10, h - 2), font, xcen=2, maxl=24, bco=TILE_OFF_565)
        text(page, f"row{r}_longtext", (tx, y + 1, right - nw - tx, h - 2), font, maxl=40, pco=LIGHT_GREY,
             bco=TILE_OFF_565)
        text(page, f"row{r}_shortval", (right - nw, y + 1, nw, h - 2), font, xcen=2, maxl=24, bco=TILE_OFF_565)
        text(page, f"row{r}_wide", (tx, y + 1, right - tx, h - 2), font, maxl=WIDE_ROW_MAXL, bco=TILE_OFF_565)
        scrolling_row(page, r, tx, y + 1, h - 2, right, vw - 10, font, bco=TILE_OFF_565)
    close(page)
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


def scrolling_row(page, r: int, tx: int, y: int, h: int, right: int, vw: int, font: int, bco=None):
    """An entity row's scrolling label ("scroll"), over the label field beside
    the wide value column, for a label too long for its field -- beside the
    narrow value column as well. One a row: a scrolling text takes one of the
    page's 12 timers (two of them the page's own, swipe_tm and busy_tm), so a
    page holds 10 at most; values and wide rows end in "..." as before."""
    scroller(page, f"row{r}_textscr", (tx, y, right - vw - tx, h), font, pco=LIGHT_GREY, bco=bco, maxl=48)


def system_page(p: Project) -> None:
    """The panel's own values (page type "system", Engine::draw_system;
    boards/nspanel/nextion/layout.py SYSTEM_*): eight cards, each a picture (per
    background, compose.py "card1".."card8") with an icon, a small label and the
    value on it -- or, for a value too long for a line, the label higher
    (cardN_top) and the value in two smaller lines (cardN_line1, cardN_line2) --
    and a small icon top right (cardN_badge: the WiFi's signal). The texts sit
    on the card's flat colour."""
    page = new_page(p, "system", touch=True)
    title(page)
    w, h, tx = layout.SYSTEM_CARD_W, layout.SYSTEM_CARD_H, layout.SYSTEM_TEXT_X
    small, big = layout.SYSTEM_TEXT_FONT, layout.SYSTEM_VALUE_FONT
    for k, (x, y) in enumerate(layout.SYSTEM_CARDS, start=1):
        c = f"card{k}"
        picture(page, f"{c}_pic", (x, y, w, h), PICTURES[f"card{k}"]["box"])
        part = card_parts(x, y, w)
        icon(page, f"{c}_icon", x + 24, y + h / 2, ICON_FONT_LARGE, maxl=4, pco=WHITE, bco=TILE_OFF_565)
        text(page, f"{c}_text", part("text"), small, maxl=32, pco=LIGHT_GREY, bco=TILE_OFF_565)
        text(page, f"{c}_value", part("value"), big, maxl=40, pco=WHITE, bco=TILE_OFF_565)
        text(page, f"{c}_top", part("top", 30), small, maxl=24, pco=LIGHT_GREY, bco=TILE_OFF_565)
        text(page, f"{c}_line1", part("line1", 34), small, maxl=32, pco=WHITE, bco=TILE_OFF_565)   # clear of the badge
        text(page, f"{c}_line2", part("line2"), small, maxl=32, pco=WHITE, bco=TILE_OFF_565)
        text(page, f"{c}_badge", (x + w - 28, y + 3, 25, 25), ICON_FONT, xcen=1, maxl=4, pco=LIGHT_GREY, bco=TILE_OFF_565)
    # the IP card opens the network page (Engine::draw_network_page)
    x, y = layout.SYSTEM_CARDS[6]
    hotspot(page, "card7_hs", (x, y, w, h), event("system", "card7", "release"))
    # the page dots, in the swipe row (Engine::draw_dots shows them)
    text(page, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    close(page)
    # everything hidden while the page loads: the firmware shows what it has (no card flashes with the design's
    # picture); the title and the close cross at once
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


def card_parts(x: int, y: int, w: int):
    """The fields on a card of the panel info (and the entity cards) by their
    place in it (layout.SYSTEM_PARTS: top and height): from the text's left
    edge (SYSTEM_TEXT_X) to 6 px short of the card's right one, or `clear` px
    short of it (of the badge top right)."""
    tx = layout.SYSTEM_TEXT_X
    return lambda name, clear=6: (x + tx, y + layout.SYSTEM_PARTS[name][0], w - tx - clear, layout.SYSTEM_PARTS[name][1])


def network_page(p: Project) -> None:
    """The network page, over the info page (its IP card tapped; Engine::draw_network_page): the title
    with a back arrow (its touch area goes back), nine rows of a label and a value -- the addresses, gateway,
    DNS and time server; an IPv6 address has up to 39 characters, so the value column is wide and its font
    one step smaller than the entity rows'."""
    page = new_page(p, "netpage", swipe=False, touch=True)
    title(page)
    hotspot(page, "back_hs", layout.BACK_TOUCH, event("netpage", "back", "release"))
    x0, x1 = layout.MARGIN + 2, VISIBLE_W - layout.MARGIN - 2
    lw = layout.NETWORK_LABEL_W
    for r in range(layout.NETWORK_ROWS_N):
        y = layout.NETWORK_ROWS_Y + r * layout.NETWORK_ROWS_STEP
        if layout.NETWORK_STACKED:   # the value under its label, the whole width (an IPv6 address is long)
            text(page, f"row{r + 1}_text", (x0, y, x1 - x0, 20), 0, ycen=1, maxl=20, pco=LIGHT_GREY)
            text(page, f"row{r + 1}_value", (x0, y + 20, x1 - x0, 24), 1, ycen=1, maxl=48)
            continue
        text(page, f"row{r + 1}_text", (x0, y, lw, 26), 1, ycen=1, maxl=20, pco=LIGHT_GREY)
        text(page, f"row{r + 1}_value", (x0 + lw, y, x1 - x0 - lw, 26), 1, xcen=2, ycen=1, maxl=48)
    close(page)
    on_load(page, loaded(page))


COVER_BUTTON = "#3a3a40"   # the cover group page's small buttons (drawn by the firmware, Engine::draw_covers)


def cover_page(p: Project) -> None:
    """The cover page (boards/nspanel/nextion/layout.py COVER_*; Engine::draw_cover): the cover on the left,
    drawn by the firmware in its style -- a touch on it, and the finger moving, report where ("cdrw": x * 1000 +
    y, while it moves too) and where it was let go ("cdru") --; on the right the position large, the state in
    words and which way it moves, up / stop / down in a row and three preset pills under them (the firmware draws
    the rounded shapes, an icon or label field sits on each, a touch area on top)."""
    page = new_page(p, "cover", touch=True)
    title(page)
    text(page, "value", layout.COVER_VALUE, 5, maxl=8)
    text(page, "state", layout.COVER_STATE, 2, maxl=24, pco=LIGHT_GREY)
    text(page, "moving_icon", layout.COVER_MOVING_ICON, ICON_FONT, xcen=1, maxl=4, pco=LIGHT_GREY)
    text(page, "moving", layout.COVER_MOVING, 2, maxl=24, pco=LIGHT_GREY)
    # the drawing: where the finger is, as x * 1000 + y, when it comes down and every WHEEL_TICK_MS while it
    # moves (a timer: a hotspot reports no moving); where it was when let go -- in sys2: the page's swipe
    # detection uses sys0 and sys1
    where = ["sys2=tch0*1000", "sys2+=tch1", "printh 91", 'prints "cdrw",0', "printh 00", "prints sys2,0",
             "printh FF FF FF"]
    timer(page, "draw_tm", WHEEL_TICK_MS, ["if(tch0>0)", "{"] + ["  " + l for l in where] + ["}"])
    let_go = ["draw_tm.en=0", "printh 91", 'prints "cdru",0', "printh 00", "prints sys2,0", "printh FF FF FF"]
    hotspot(page, "draw_hs", layout.COVER_DRAW, let_go, where + ["draw_tm.en=1"])
    for k, (x, y, w, h) in enumerate(layout.COVER_BUTTONS, start=1):
        icon(page, f"btn{k}_icon", x + w / 2, y + h / 2, ICON_FONT_LARGE, pco=LIGHT_GREY, bco=TILE_OFF_565)
        # touched: the icon in the accent at once, by the display itself (the firmware answers after the command)
        hotspot(page, f"btn{k}_hs", (x, y, w, h),
                event("cover", ("open_cover", "stop_cover", "close_cover")[k - 1], "release"),
                [f"btn{k}_icon.pco=th_accent"])
    inset = layout.COVER_PRESET_INSET
    for k, (x, y, w, h) in enumerate(layout.COVER_PRESETS, start=1):
        text(page, f"preset{k}", (x + inset, y + 2, w - 2 * inset, h - 4), 2, xcen=1, maxl=16, pco=LIGHT_GREY,
             bco=TILE_OFF_565)
        hotspot(page, f"preset{k}_hs", (x, y, w, h), event("cover", f"preset{k}", "release"),   # over its label
                [f"preset{k}.pco=th_accent"])
    close(page)
    # everything hidden while the page loads: the firmware shows what the cover has; the title and the close
    # cross at once
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


def covers_page(p: Project) -> None:
    """The cover group page (boards/nspanel/nextion/layout.py COVERS_*; Engine::draw_covers): the whole group at
    the top ("all", taller, larger buttons), four members a sheet under it -- three, when the group has presets:
    their pills in the first row's place (preset1_hs .. preset3_hs). Per row an invisible box for its
    small drawing (rowN_mini: the manifest's "boxes"), a touch area over the row that opens the member's cover
    page (rowN_hs, its box the card the firmware draws), the name and the position on the card's colour, and
    three small buttons -- the firmware draws their rounded squares, an icon field sits on each, a touch area on
    top. Dots for the sheets."""
    page = new_page(p, "covers", touch=True)
    title(page)
    text(page, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    gap, end = layout.COVERS_BUTTON_GAP, layout.COVERS_BUTTON_END
    x0, w = layout.MARGIN, VISIBLE_W - 2 * layout.MARGIN
    button_bco = rgb565(COVER_BUTTON)
    rows = [("all", layout.COVERS_ALL_Y, layout.COVERS_ALL_H, layout.COVERS_ALL_BUTTON, 3, ICON_FONT_LARGE)]
    rows += [(f"row{r}", layout.COVERS_ROWS_Y + (r - 1) * layout.COVERS_ROWS_STEP, layout.COVERS_ROWS_H,
              (layout.COVERS_BUTTON, layout.COVERS_BUTTON), 2, ICON_FONT) for r in range(1, layout.COVERS_ROWS_N + 1)]
    for n, y, h, (bw, bh), font, icon_font in rows:
        mx, my, mw, mh = layout.COVERS_MINI
        hotspot(page, f"{n}_mini", (x0 + mx, y + (h - mh) // 2, mw, mh))
        first = x0 + w - end - 3 * bw - 2 * gap   # the first button's left edge
        tx = x0 + layout.COVERS_TEXT_X
        if layout.COVERS_STACKED:
            # two lines: the name across the row, under it a member's position and the buttons at the bottom right
            line = layout.COVERS_LINE_H
            text(page, f"{n}_text", (tx, y + 4, x0 + w - end - tx, line), font, maxl=24, pco=WHITE, bco=TILE_OFF_565)
            if n != "all":
                text(page, f"{n}_value", (tx, y + h - 4 - line, layout.COVERS_VALUE_W, line), 2, maxl=16,
                     pco=LIGHT_GREY, bco=TILE_OFF_565)
            by = y + h - end - bh
        else:
            # the name; a member's position before the buttons (the whole group has none: its name up to them)
            text_w = layout.COVERS_TEXT_W if n != "all" else first - 6 - tx
            text(page, f"{n}_text", (tx, y + 2, text_w, h - 4), font, maxl=24, pco=WHITE, bco=TILE_OFF_565)
            if n != "all":
                text(page, f"{n}_value", (first - 6 - layout.COVERS_VALUE_W, y + 2, layout.COVERS_VALUE_W, h - 4), 2,
                     xcen=2, maxl=16, pco=LIGHT_GREY, bco=TILE_OFF_565)
            by = y + (h - bh) // 2
        buttons = [(part, first + k * (bw + gap), by) for k, part in enumerate(("up", "stop", "down"))]
        inset = 4 if bh < 40 else 6
        for part, bx, by in buttons:
            text(page, f"{n}_{part}", (bx + inset, by + inset, bw - 2 * inset, bh - 2 * inset), icon_font, xcen=1,
                 maxl=4, pco=LIGHT_GREY, bco=button_bco)
        # the touch areas over the fields (a field takes a touch too): the row's, then the buttons' on top of it
        hotspot(page, f"{n}_hs", (x0, y, w, h), event("covers", n, "release"))
        for part, bx, by in buttons:   # touched: the icon in the accent at once (the display's own; the firmware
            hotspot(page, f"{n}_{part}_hs", (bx, by, bw, bh),   # turns it back a while after the command)
                    event("covers", f"{n}_{part}", "release"), [f"{n}_{part}.pco=th_accent"])
    # the group's presets, in the first row's place (made after it: on top); the firmware draws the pills with
    # their labels and shows the touch areas only when the group has presets (the first row hidden then)
    for k, (x, y, w, h) in enumerate(layout.COVERS_PRESETS, start=1):
        hotspot(page, f"preset{k}_hs", (x, y, w, h), event("covers", f"preset{k}", "release"))
    close(page)
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


def thermostat_page(p: Project) -> None:
    """The thermostat page (boards/nspanel/nextion/layout.py THERMO_*; Engine::draw_thermostat): the ring the
    firmware draws -- a touch on it, and the finger moving, report where ("tdrw": x * 1000 + y, while it moves too)
    and where it was let go ("tdru") --; inside it the temperature now, the target and whether it heats or cools;
    minus and plus beside it (the firmware draws their discs, an icon field sits on each, a touch area on top); the
    modes as pills under it -- a touch area for each of 1 to 4 pills, the firmware shows the set there are and draws
    the pills under them."""
    page = new_page(p, "thermostat", touch=True)
    title(page)
    text(page, "now", layout.THERMO_NOW, 6, xcen=1, maxl=10)
    text(page, "target", layout.THERMO_TARGET, 2, xcen=1, maxl=24, pco=LIGHT_GREY)
    hotspot(page, "state_hs", layout.THERMO_STATE)   # where the firmware writes heating / cooling with its icon
    where = ["sys2=tch0*1000", "sys2+=tch1", "printh 91", 'prints "tdrw",0', "printh 00", "prints sys2,0",
             "printh FF FF FF"]
    timer(page, "ring_tm", WHEEL_TICK_MS, ["if(tch0>0)", "{"] + ["  " + l for l in where] + ["}"])
    let_go = ["ring_tm.en=0", "printh 91", 'prints "tdru",0', "printh 00", "prints sys2,0", "printh FF FF FF"]
    hotspot(page, "ring_hs", layout.THERMO_RING, let_go, where + ["ring_tm.en=1"])
    r = layout.THERMO_BUTTON_R
    for name, glyph, (cx, cy) in zip(("minus", "plus"), ("minus", "plus"), layout.THERMO_BUTTONS):
        # touched: the icon in the accent at once, by the display itself (the firmware answers after the command)
        icon(page, name, cx, cy, ICON_FONT_LARGE, glyph=glyph, pco=WHITE, bco=TILE_OFF_565)
        hotspot(page, f"{name}_hs", (cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8),
                event("thermostat", name, "release"), [f"{name}.pco=th_accent"])
    for n in range(1, 5):   # over the ring's touch area (made later: on top)
        for k, box in enumerate(layout.thermo_modes(n), start=1):
            hotspot(page, f"m{n}{k}_hs", box, event("thermostat", f"mode{k}", "release"))
    close(page)
    # everything hidden while the page loads: the firmware shows what there is; the title and the close cross at once
    on_load(page, loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


# The keypad page's code: its places as OHpanel's own icons (own_icons.py: smaller than MDI's circles, apart), in
# event code as the bytes of their UTF-8 the project's encoding (iso-8859-1) writes; at most KEYPAD_LONGEST of them.
KEYPAD_TYPED, KEYPAD_EMPTY = "ohpanel-code-typed", "ohpanel-code-empty"
KEYPAD_BACKSPACE = "EEAD9B"   # "backspace-outline"
KEYPAD_LONGEST = 8
KEYPAD_DIGIT_FONT = 4      # 36 px
KEYPAD_LABELS = ["1", "2", "3", "4", "5", "6", "7", "8", "9", None, "0", "C"]


def keypad_page(p: Project) -> None:
    """The keypad page (page type "keypad", Engine::draw_keypad): a code typed on the keys, sent with one of up to
    four buttons. The code is the display's own until then: each key adds to it (the variable "code"), the places
    under the state fill ("typed": as many as the firmware says, "places", else one per character typed), a key
    pressed shows a disc in the accent behind it, drawn here (cirs, then its label again with xstr; ref, when let
    go, puts the field back); the keys only tell the firmware that they were touched. A button sends the code with
    its event ("button<k>", the code as the value) and empties it. The bottom row is backspace, 0, C -- or with
    "phone" ("phone.val", set by the firmware) *, 0, #, backspace then on the code's places. The state and the
    buttons (pills, the set for as many as there are) are drawn by the firmware where touch areas mark them. In the
    swipe row a swipe may end on a key or a button: one let go after the finger moved as far as a swipe does
    (SWIPE_MIN) types and sends nothing."""
    page = new_page(p, "keypad", touch=True)
    title(page)
    hotspot(page, "state_hs", layout.KEYPAD_STATE)   # where the firmware writes the state, its icon and words
    text(page, "typed", layout.KEYPAD_CODE, ICON_FONT, xcen=1, maxl=3 * KEYPAD_LONGEST + 2, pco=WHITE)
    text(page, "message", layout.KEYPAD_CODE, 2, xcen=1, maxl=48, pco=LIGHT_GREY)   # sent, not accepted, ...
    cx, cy, cw, ch = layout.KEYPAD_CODE
    back = icon(page, "back", cx + cw - 18, cy + ch / 2, ICON_FONT, glyph="backspace-outline", pco=LIGHT_GREY)
    back.set("txt", bytes.fromhex(KEYPAD_BACKSPACE))
    new(page, VARIABLE, "code", sta=1, txt="", txt_maxl=KEYPAD_LONGEST * 2)
    new(page, VARIABLE, "tmp", sta=1, txt="", txt_maxl=KEYPAD_LONGEST * 2)
    new(page, VARIABLE, "places", val=0)
    new(page, VARIABLE, "longest", val=KEYPAD_LONGEST)
    new(page, VARIABLE, "phone", val=0)
    new(page, VARIABLE, "moved", val=0)
    # where the finger came down, for a key or a button let go (the swipe's own touch area keeps it up to date)
    down = ["swipex=tch0", "swipey=tch1"]

    def unmoved(code: list[str], moved: list[str]) -> list[str]:
        """`code` for a finger let go about where it came down (swipe_detection's test), else `moved`."""
        test = ["moved.val=0", "sys0=swipex-tch2", "sys1=swipey-tch3"]
        for cond in (f"sys0>{SWIPE_MIN}", f"sys0<-{SWIPE_MIN}", f"sys1>{SWIPE_MIN}", f"sys1<-{SWIPE_MIN}"):
            test += [f"if({cond})", "{", "  moved.val=1", "}"]
        return test + ["if(moved.val==0)", "{"] + ["  " + l for l in code] + ["}else", "{"] + \
            ["  " + l for l in moved] + ["}"]
    import own_icons
    full, empty = (chr(own_icons.code_points()[n]).encode().decode("latin-1") for n in (KEYPAD_TYPED, KEYPAD_EMPTY))
    places = ["strlen code.txt,sys0", "sys2=places.val", "if(sys2==0)", "{", "  sys2=sys0", "}", 'typed.txt=""',
              "sys1=0", "while(sys1<sys2)", "{", "  if(sys1<sys0)", "  {", f'    typed.txt+="{full}"', "  }else", "  {",
              f'    typed.txt+="{empty}"', "  }", "  sys1++", "}", "vis message,0", "vis typed,1",
              "if(phone.val==1)", "{", "  ref back", "}"]   # the field painted its whole box: backspace on it again
    add = lambda c: ["strlen code.txt,sys0", "if(sys0<longest.val)", "{", f'  code.txt+="{c}"', "}"]   # noqa: E731
    backspace = ["strlen code.txt,sys0", "if(sys0>0)", "{", "  sys0--", "  substr code.txt,tmp.txt,0,sys0",
                 "  code.txt=tmp.txt", "}"]
    hotspot(page, "back_hs", (cx + cw - 40, cy - 6, 40, ch + 12), backspace + places, [])
    kw, kh = layout.KEYPAD_KEY
    r = layout.KEYPAD_DISC
    for i, label in enumerate(KEYPAD_LABELS):
        x0, y0 = layout.KEYPAD_COLS[i % 3], layout.KEYPAD_ROWS[i // 3]
        n = f"k{i + 1}"
        box = (x0 - kw // 2, y0 - kh // 2, kw, kh)
        text(page, n, box, KEYPAD_DIGIT_FONT, xcen=1, maxl=4, pco=WHITE, txt=label or "*")
        disc = f"cirs {x0},{y0},{r},th_accent"
        draw = lambda obj, font: f"xstr {box[0]},{box[1]},{kw},{kh},{font},{WHITE},0,1,1,3,{obj}.txt"   # noqa: E731
        if i == 9:   # backspace, or "*"
            ico = icon(page, "k10i", x0, y0, ICON_FONT_LARGE, glyph="backspace-outline", pco=WHITE)
            ico.set("txt", bytes.fromhex(KEYPAD_BACKSPACE))
            ix, iy, iw, ih = (ico.get(k) for k in ("x", "y", "w", "h"))
            press = [disc, "if(phone.val==1)", "{", "  " + draw(n, KEYPAD_DIGIT_FONT), "}else", "{",
                     # on the disc's colour, not on nothing: the icon font's smoothed edges came out black
                     f"  xstr {ix},{iy},{iw},{ih},{ICON_FONT_LARGE},{WHITE},th_accent,1,1,1,k10i.txt", "}"]
            # the icon's field is smaller than the disc: the key's background back first (Program.s bg_pic)
            act = ["if(phone.val==1)", "{", f"  ref {n}"] + ["  " + l for l in add("*")] + ["}else", "{",
                   f"  picq {box[0]},{box[1]},{kw},{kh},bg_pic", "  ref k10i"] + ["  " + l for l in backspace] + ["}"]
        elif i == 11:   # C, or "#"
            press = [disc, draw(n, KEYPAD_DIGIT_FONT)]
            act = [f"ref {n}", "if(phone.val==1)", "{"] + ["  " + l for l in add("#")] + ["}else", "{", '  code.txt=""', "}"]
        else:
            press = [disc, draw(n, KEYPAD_DIGIT_FONT)]
            act = [f"ref {n}"] + add(label)
        # moved: only the key as it was (the disc away)
        undo = [f"picq {box[0]},{box[1]},{kw},{kh},bg_pic", f"ref {n}", "ref k10i"] if i == 9 else [f"ref {n}"]
        hotspot(page, f"{n}_hs", box, unmoved(act + places + event("keypad", "key", "release"), undo),
                down + press + event("keypad", "key", "press"))
    # the buttons: a touch area each, the set for as many as there are; the code goes with the event, then it is gone
    for count in range(1, 5):
        for k, box in enumerate(layout.keypad_buttons(count), start=1):
            payload = f'{{\\"page\\": \\"keypad\\", \\"component\\": \\"button{k}\\", \\"value\\": \\"'
            send = ["printh 92", 'prints "nspanelevent",0', "printh 00", f'prints "{payload}",0', "prints code.txt,0",
                    'prints "\\"}",0', "printh 00", "printh FF FF FF"]
            hotspot(page, f"b{count}{k}_hs", box, unmoved(send + ['code.txt=""'] + places, []),
                    down + event("keypad", "button", "press"))
    close(page)
    # what the firmware shows: the button set there are, backspace on the code's places with "phone"; "*" for
    # backspace with it
    hidden = ["message", "back", "back_hs", "k10"] + [f"b{n}{k}_hs" for n in range(1, 5) for k in range(1, n + 1)]
    on_load(page, loaded(page) + [f"vis {o},0" for o in hidden])


# The page dots ("dots" on the pages that have them): a dot a sheet of the swipe row's pages (Engine::draw_dots), side
# by side without a space -- as many as the narrower of the fields takes (the manifest's "dots"). A dot is 3 bytes
# (UTF-8), its field holds DOTS_MAX of them.
DOTS_GLYPH = "circle-medium"
DOTS_MAX = 24
DOTS_BYTES = 3 * DOTS_MAX


def dots_max() -> int:
    advance = fonts.icon_ink(ICON_FONT, DOTS_GLYPH)[0]
    return min(layout.ENTITY_DOTS[2] // advance, layout.BUTTON_DOTS[2] // advance, DOTS_MAX)



def buttons_page(p: Project) -> None:
    """The button page (page type "buttons"; the favourites, page type
    "media_favorites", draw on it too): 2 rows of 4 tiles on the layout's grid
    (boards/nspanel/nextion/layout.py), each a picture (off / on, per background:
    compose.py slots button1..8) with an icon, a value and a label on it, an
    icon centred above the label (a button's with "layout": "center", a
    favourite's -- or a station's logo the firmware draws over it), the touch
    area on top; the page dots at the bottom (written by the firmware, "●○○":
    where a page of the swipe row sits in it, on a favourites page its
    sheets)."""
    page = new_page(p, "buttons", touch=True)
    title(page)
    for i, (x, y) in enumerate(layout.button_tiles(), start=1):
        b = f"tile{i}"
        picture(page, f"{b}_pic", (x, y, layout.TILE_W, layout.TILE_H), PICTURES[f"button{i}"]["off"])
        # a button: the icon top left, the name on the bottom line, its state (if any) above it, the bar of a value
        # in % below them (a progress bar the firmware fills, colours and shows) -- one place each, with or without
        # a state or a bar
        rel = lambda box: (x + box[0], y + box[1], box[2], box[3])
        text(page, f"{b}_icon", rel(layout.TILE_ICON), ICON_FONT_LARGE, xcen=1, maxl=10, pco=0, bco=0)
        text(page, f"{b}_name", rel(layout.TILE_NAME), 1, maxl=16, pco=0, bco=0)
        text(page, f"{b}_value", rel(layout.TILE_STATE), 0, maxl=12, pco=0, bco=0)
        progress(page, f"{b}_bar", rel(layout.TILE_BAR), pco=rgb565("#0080f8"), bco=rgb565("#4a4a4a"))
        # a button with "layout": "center" (and a favourite): the icon large in the middle, the name centred under it
        text(page, f"{b}_text", rel(layout.TILE_TEXT), 0, xcen=1, maxl=10, pco=0, bco=0)
        dx, dy, w, h = layout.FAV_ICON
        icon(page, f"{b}_center", x + dx + w / 2, y + dy + h / 2, ICON_FONT_XL, maxl=4, pco=0, bco=0)
        hotspot(page, f"{b}_hs", (x, y, layout.TILE_W, layout.TILE_H), event("buttons", b, "release"),
                event("buttons", b, "press"))
    text(page, "dots", layout.BUTTON_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    close(page)
    on_load(page, loaded(page) + ["vis 255,0", "vis close_hs,1"] + [f"vis tile{i}_center,0" for i in range(1, 9)])


def settings_page(p: Project) -> None:
    """OHpanel's settings page (page type "settings", Engine::draw_settings):
    the title, three sliders -- brightness in use, when idle, idle after
    (steps) -- with a label and the value each, and the restart button, a tile
    like the button pages' across the page (its pictures composed per
    background, compose.py "restart"; the firmware picks the variant)."""
    page = new_page(p, "settings")
    title(page)
    colours = []
    x, w = layout.SETTINGS_X, layout.SETTINGS_W
    for k, y in enumerate(layout.SETTINGS_ROW_Y, start=1):
        text(page, f"set{k}_text", (x, y, layout.SETTINGS_TEXT_W, 28), TEXT_FONT, ycen=2, maxl=20, pco=LIGHT_GREY)
        text(page, f"set{k}_value", (x + w - layout.SETTINGS_VALUE_W, y, layout.SETTINGS_VALUE_W, 28), TEXT_FONT,
             xcen=2, ycen=2, maxl=10, pco=LIGHT_GREY)
        # reporting as "set0" .. "set2"
        slider(page, f"set{k}_slider", (x, y + layout.SETTINGS_SLIDER_DY, w, layout.SETTINGS_SLIDER_H), f"set{k - 1}")
        colours += [f"set{k}_slider.bco1=th_accent", f"set{k}_slider.pco=th_text"]
    restart_button(page, "restart", "restart")
    close(page)
    on_load(page, colours + ["title.pco=th_text"] + loaded(page))


def restart_button(page, name: str, component: str, font=2):
    """The wide button at the bottom of the settings and the volume page (its
    pictures off / on per background: compose.py slot "restart")."""
    pics = PICTURES["restart"]
    b = new(page, BUTTON, name, layout.SETTINGS_BUTTON, sta=2, style=0, borderw=0, font=font, pic=pics["off"],
            pic2=pics["on"], pco=WHITE, pco2=rgb565(TILE_OFF), xcen=1, ycen=1, txt="", txt_maxl=32)
    return b.event("codesup", event(page.name, component, "release"))


def boot_page(p: Project) -> None:
    """The boot page (boards/nspanel/nextion/layout.py BOOT_*): what the panel is doing,
    five rows of a label and a value (firmware, device, IP address, IPv6 address -- below the code, the whole
    width and a smaller font, for the 39 characters one can have --, more), the
    hotspot's QR code beside them (the firmware draws it in hotspot mode; an
    invisible hotspot marks where: the manifest's "boxes") with the same
    margin to the visible edge as the texts' on the left, the logo (a picture
    per background, compose.py slot "logo") at the bottom."""
    page = new_page(p, "boot", swipe=False)
    text(page, "status", layout.BOOT_TITLE, 4, maxl=30)   # what the panel is doing, in its language (Engine::draw_boot)
    qr_x = layout.BOOT_QR[0]
    lx, lw = layout.BOOT_LABEL
    for k, (row, y) in enumerate(zip(("firmware", "device", "ip", "ipv6", "info"), layout.BOOT_ROWS_Y)):
        text(page, f"{row}_text", (lx, y, lw, 20), 2, ycen=2, maxl=16)   # 8 letters, 2 bytes each in Greek
        # the value fields beside the code end short of it: a text field repaints its whole box (cropped from the
        # background) and would cut into the code; the rows from the fourth on are below it
        below = row in layout.BOOT_BELOW_QR
        end = qr_x - layout.BOOT_VALUE_GAP if not below else VISIBLE_W - 3
        text(page, f"{row}_value", (layout.BOOT_VALUE_X, y, end - layout.BOOT_VALUE_X, 20), 1 if row == "ipv6" else 2,
             ycen=2, maxl=128 if row == "info" else 48 if row == "ipv6" else 30)
    hotspot(page, "qr_area", layout.BOOT_QR)
    picture(page, "logo_pic", layout.BOOT_LOGO, PICTURES["logo"]["boot"])
    on_load(page, loaded(page))


def splash_page(p: Project) -> None:
    """What the display shows itself when it starts (Program.s), before the
    firmware has found it -- it knows neither the background nor the theme
    then: the logo, larger, drawn in on black (logo_animation). The firmware
    opens the boot page when it takes over; nothing here reports to it."""
    page = new_page(p, "splash", background=False, swipe=False)
    picture(page, "splash_pic", layout.SPLASH_LOGO, ALL_PICTURES[SPLASH_BACKGROUND]["splash"]["logo"])
    logo_animation(page, "splash_pic", layout.SPLASH_LOGO, "splash")


# The splash page's logo drawn in (logo_animation): the mark opening from its middle, then "OHpanel" and
# "for NSPanel" wiped in from the left, a step every LOGO_STEP_MS. Places in mark units (64: the logo's height,
# assets/generate.py logo()).
LOGO_MARK_STEPS = 8
LOGO_WORD_BOTTOM = 48           # "OHpanel" above, "for NSPanel" below
LOGO_WORD_STEPS = 10
LOGO_TAG_STEPS = 5
LOGO_STEP_MS = 50
# The splash page is on black, whatever the background configured (assets/compose.py).
SPLASH_BACKGROUND = "black"


def logo_left(box) -> float:
    """Where the mark starts in a logo picture of the box's size (assets/generate.py)."""
    sys.path.insert(0, os.path.join(HERE, "assets"))
    from generate import logo_left as left
    return left(box[2], box[3])


def logo_animation(page, pic: str, box, prefix: str) -> None:
    """The logo picture `pic` (at `box`, hidden while the page loads:
    hidden_until_drawn()) drawn in from the page's start, a piece at a time,
    with xpic from its own picture -- no further pictures, so no more room in
    the display's flash. The timer <prefix>_tm runs it; its last step shows
    `pic`: what was drawn stays when the page is drawn again."""
    bx, by, w, h = box
    u, left = h / 64, logo_left(box)
    pieces = []
    for k in range(1, LOGO_MARK_STEPS + 1):   # a square from the mark's middle
        s = round(h / 2 * k / LOGO_MARK_STEPS)
        x0, y0 = round(left + h / 2) - s, round(h / 2) - s
        pieces.append((max(x0, 0), max(y0, 0), 2 * s, 2 * s))
    word_x, word_y = round(left + h + 8 * u), round(LOGO_WORD_BOTTOM * u)
    for (y, ph), steps in (((0, word_y), LOGO_WORD_STEPS), ((word_y, h - word_y), LOGO_TAG_STEPS)):
        edges = [word_x + round((w - word_x) * k / steps) for k in range(steps + 1)]
        pieces += [(a, y, b - a, ph) for a, b in zip(edges, edges[1:])]
    n, tm = f"{prefix}_n", f"{prefix}_tm"
    code = [f"{n}.val++"]
    for k, (x, y, pw, ph) in enumerate(pieces):
        pw, ph = min(pw, w - x), min(ph, h - y)
        code += [("if" if k == 0 else "}else if") + f"({n}.val=={k + 1})", "{",
                 f"  xpic {bx + x},{by + y},{pw},{ph},{x},{y},{pic}.pic"]
    code += ["}else", "{", f"  {tm}.en=0", f"  vis {pic},1", "}"]
    new(page, VARIABLE, n, val=0)
    timer(page, tm, LOGO_STEP_MS, code, enabled=True)


def blank_page(p: Project) -> None:
    """Black: the screen "off" while the firmware has it blanked
    (display.screen, Engine::screen_tick); a touch on it only says so."""
    page = new_page(p, "blank", background=False, swipe=False)
    page.objects()[0].event("codesup", event("blank", "wake", "release"))
    on_load(page, loaded(page))


def media_page(p: Project) -> None:
    """The media page (boards/nspanel/nextion/layout.py MEDIA_*): what plays (title,
    artist, album), the art (a box picture with a large icon on it -- the
    firmware draws the cover over it), previous / next (play/pause between
    them the firmware draws: Engine::draw_play), the volume's and the
    favourites' discs (pictures with the speaker and the star on them),
    hotspots for them."""
    media = new_page(p, "media", swipe=False, touch=True)
    title(media, pco=WHITE)
    for name, box, font in (("track", layout.MEDIA_TITLE, 4), ("artist", layout.MEDIA_SUB, 3),
                            ("album", layout.MEDIA_SOURCE, 2)):
        text(media, name, box, font, maxl=60)
    # the art box and the pills: pictures per background (compose.py), a text on each in the box's colour
    for name, box, slot in (("art_pic", layout.MEDIA_ART, "art"), ("volume_pic", layout.MEDIA_VOLUME, "vol"),
                            ("fav_pic", layout.MEDIA_FAV, "fav")):
        picture(media, name, box, PICTURES[slot]["box"])
    # the icons centred by what shows of them: the art's, the speaker and the star on their discs, previous
    # and next
    centre = lambda box: (box[0] + box[2] / 2, box[1] + box[3] / 2)
    icon(media, "art_icon", *centre(layout.MEDIA_ART), 6, bco=TILE_OFF_565)
    icon(media, "volume_icon", *centre(layout.MEDIA_VOLUME), ICON_FONT_LARGE, "volume-high", bco=PILL_565)
    icon(media, "fav_icon", *centre(layout.MEDIA_FAV), ICON_FONT_LARGE, "star", bco=PILL_565)
    px, cx, nx, y = layout.MEDIA_CONTROLS
    icon(media, "prev_icon", px, y, 6, "skip-previous")
    icon(media, "next_icon", nx, y, 6, "skip-next")

    # hotspots over the controls and the pills
    def spot(name, box, component):
        hotspot(media, name, box, event("media", component, "release"))
    d = layout.MEDIA_PLAY_DISC   # the disc's box: the manifest's "boxes" too (drawn_boxes)
    spot("prev_hs", (px - 36, y - 36, 72, 72), "previous")
    spot("play_hs", (cx - d // 2, y - d // 2, d, d), "play")
    spot("next_hs", (nx - 36, y - 36, 72, 72), "next")
    for name, (vx, vy, vw, vh), component in (("volume_hs", layout.MEDIA_VOLUME, "volume"),
                                              ("fav_hs", layout.MEDIA_FAV, "favorites")):
        spot(name, (vx - 4, vy - 6, vw + 8, vh + 12), component)
    close(media)
    # everything hidden while the page loads, as on the button pages: the firmware shows what it has
    # set (no pill flashes with the design's picture); the title and the close cross at once
    on_load(media, loaded(media) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


def volume_page(p: Project) -> None:
    """The volume page ("volpage" -- not "volume": the display's own variable
    for its speaker), opened from the media page's disc (boards/nspanel/nextion/layout.py
    VOLUME_*): the title, minus / the volume / plus, a slider across the
    page, the mute button where the settings page has its restart button (its
    pictures). The slider reports as it moves ("vols") and when let go
    ("volu"); the rest on release."""
    page = new_page(p, "volpage", swipe=False)
    title(page)
    x, y, w, h = layout.VOLUME_ROW
    text(page, "value", (x + h, y, w - 2 * h, h), 5, xcen=1, maxl=10, pco=LIGHT_GREY)
    for name, bx in (("minus", x), ("plus", x + w - h)):
        text(page, name, (bx, y, h, h), 5, xcen=1, maxl=10, pco=LIGHT_GREY).event(
            "codesup", event("volpage", name, "release"))
    slider(page, "slider", layout.VOLUME_SLIDER, "vol")
    # the speaker glyph and the word in one field: the icon font has Latin-1 too
    restart_button(page, "mute", "mute", font=ICON_FONT)
    close(page)
    on_load(page, ["slider.bco1=th_accent", "slider.pco=th_text"] + loaded(page))


def qr_page(p: Project) -> None:
    """The QR page (boards/nspanel/nextion/layout.py QR_*): the title as on the other
    pages, the area the firmware draws the code in, on a white card of its own
    just around it (an invisible hotspot marks it: the manifest's "boxes",
    the web preview) -- the display's own QR object drew it column by column,
    visibly -- the caption under it, the close cross."""
    qr = new_page(p, "qr", touch=True)
    title(qr)
    hotspot(qr, "qr_area", layout.QR_AREA)
    for k, box in enumerate(layout.QR_CAPTION, start=1):
        text(qr, f"caption{k}", box, 2, xcen=1, maxl=80, pco=TITLE_GREY)
    # the page dots, in the swipe row (Engine::draw_dots)
    text(qr, "dots", layout.ENTITY_DOTS, ICON_FONT, xcen=1, maxl=DOTS_BYTES, pco=LIGHT_GREY)
    close(qr)
    on_load(qr, loaded(qr))


WHEEL_TICK_MS = 150   # the colour wheel reports where the finger is this often while it moves


def control_page(p: Project, name: str) -> None:
    """The light and the cover page (boards/nspanel/nextion/layout.py CONTROL_*, SIDE_*):
    the title, its icon and value large on the left, a slider upright in the
    middle (the theme's colours, as the settings page's), the column of tiles
    on the right with their hotspots (the light's: its views, and bottom left
    its on / off). The light page has a second slider on the colour
    temperature's track and the colour wheel (a picture per background,
    compose.py) with a hotspot that reports where it was touched; the
    firmware shows one of them at a time (Engine::draw_light). Sliders report
    as they move ("csls", "ctps") and when let go ("cslu", "ctpu"), the wheel
    when touched ("cwhl": x * 1000 + y)."""
    light = name == "light"
    page = new_page(p, name)
    title(page)
    hotspot(page, "close_hs", CLOSE_TOUCH, event(name, "close", "release"))
    # the icon and the value on the left
    icon(page, "icon", *layout.CONTROL_ICON, ICON_FONT_LARGE, pco=LIGHT_GREY)
    text(page, "value", layout.CONTROL_VALUE, layout.CONTROL_VALUE_FONT, xcen=1, pco=LIGHT_GREY)
    # the sliders upright, the handle across them
    colours = []
    for obj_name, tag in [("slider", "csl")] + ([("temp_slider", "ctp")] if light else []):
        o = slider(page, obj_name, layout.CONTROL_SLIDER, tag, upright=True, handle=layout.CONTROL_HANDLE)
        colours.append(f"{obj_name}.pco=th_text")
        if obj_name == "temp_slider":   # on its track (a picture: the same on every background), filled with it too
            o.set("sta", 2).set("pic", PICTURES["ctemp"]["track"]).set("pic1", PICTURES["ctemp"]["track"])
        else:
            colours.append(f"{obj_name}.bco1=th_accent")
    if light:
        picture(page, "wheel_pic", layout.COLOUR_WHEEL, PICTURES["wheel"]["disc"])
        # where the finger is, as x * 1000 + y: when it comes down, then every WHEEL_TICK_MS while it moves
        # (a timer: a hotspot reports no moving)
        where = ["sys0=tch0*1000", "sys0+=tch1", "printh 91", 'prints "cwhl",0', "printh 00", "prints sys0,0",
                 "printh FF FF FF"]
        timer(page, "wheel_tm", WHEEL_TICK_MS, ["if(tch0>0)", "{"] + ["  " + l for l in where] + ["}"])
        hotspot(page, "wheel_hs", layout.COLOUR_WHEEL, ["wheel_tm.en=0"], where + ["wheel_tm.en=1"])
        # the colour chosen: a disc the firmware draws (an invisible hotspot marks where: the manifest's "boxes")
        cx, cy, r = layout.COLOUR_SWATCH
        hotspot(page, "swatch", (cx - r, cy - r, 2 * r, 2 * r))
    # the column of tiles: a picture, the icon on it (the light's: above a label), a hotspot
    components = ["side1", "side2", "side3"] if light else ["open_cover", "stop_cover", "close_cover"]
    tiles = [(f"side{k}", xy, c) for k, (xy, c) in enumerate(zip(layout.SIDE_TILES, components), start=1)]
    if light:   # and its on / off, bottom left
        tiles.append(("power", layout.POWER_TILE, "power"))
    for k, (x, y), component in tiles:
        box = (x, y, layout.SIDE_TILE, layout.SIDE_TILE)
        picture(page, f"{k}_pic", box, PICTURES[k]["off"])
        icon(page, f"{k}_icon", x + layout.SIDE_TILE / 2, y + (layout.SIDE_ICON_CY if light else layout.SIDE_TILE / 2),
             ICON_FONT_LARGE, pco=LIGHT_GREY, bco=TILE_OFF_565)
        if light:
            text(page, f"{k}_text", (x + 8, y + layout.SIDE_TEXT_Y, layout.SIDE_TILE - 16, 20), 0, xcen=1, maxl=12,
                 pco=LIGHT_GREY, bco=TILE_OFF_565)
        hotspot(page, f"{k}_hs", box, event(name, component, "release"))
    picture(page, "close", CLOSE_ICON, PICTURES["close"]["x"]).event("codesup", event(name, "close", "release"))
    # everything hidden while the page loads: the firmware shows what the light has (its views); the title
    # and the close cross at once
    on_load(page, colours + loaded(page) + ["vis 255,0", "vis title,1", "vis close_hs,1", "vis close,1"])


# ------------------------------------------------------- across the pages --

# The fixed colours of the pages from the theme (boards/nspanel/nextion/assets/generate.py
# THEMES): global variables of Program.s the firmware sets for the background
# and theme chosen (Engine::send_theme), which the pages give their objects
# when they load -- no command for each object on each page load. Object ->
# variable; the firmware colours an object itself only when it has a colour of
# its own.
THEME_COLOURS = {
    "home": {"date": "th_dim", "info1_right": "th_dim", "info2_right": "th_dim",
             "row1_icon": "th_dim", "row2_icon": "th_dim",
             "info1_tag": "th_text", "info2_tag": "th_text",
             "info1_text": "th_text", "info2_text": "th_text",
             # the weather block's second line and the units: the numbers lead
             "low_arrow": "th_dim",
             **{f"{part}{k}": "th_dim" for part in ("high_arrow", "high") for k in range(0, layout.RANGE_STEPS + 1)},
             **{f"low{k}": "th_dim" for k in range(1, layout.RANGE_STEPS + 1)},
             **{f"outdoor_u{k}": "th_dim" for k in range(1, layout.OUTDOOR_UNIT_STEPS + 1)},
             **{f"{row}_u{k}": "th_dim" for row in ("row1", "row2") for k in range(1, layout.VALUE_UNIT_STEPS + 1)}},
    "weather": {"low": "th_dim", "low_arrow": "th_dim",   # the high leads
                **{f"{part}{k}": "th_dim" for part in ("low", "low_arrow") for k in range(1, layout.WEATHER_LINE_STEPS + 1)}},
    "qr": {"caption1": "th_text", "caption2": "th_text"},
}


def theme_colours(p: Project) -> None:
    """The pages' preinitialize code that gives their objects the theme's
    colours (the variables: program())."""
    for name, colours in THEME_COLOURS.items():
        page = p.page(name)
        missing = [o for o in colours if not page.has(o)]
        if missing:
            sys.exit(f"theme colours: {name} has no {missing}")
        root = page.objects()[0]
        root.event("codesload", [f"{o}.pco={v}" for o, v in colours.items()] + root.code("codesload"))


# A busy indicator on every page the firmware builds: three dots in the
# middle, hidden, and a timer the page starts when it loads. Unless the
# firmware has begun building the page by then (it stops the timer,
# Engine::render), the timer shows the dots and lights them up one after the
# other: the panel does not answer.
BUSY_PAGES = ("home", "weather", "buttons", "entities", "entities_cards", "entities_rows", "qr", "light", "cover", "covers", "media",
              "settings", "system", "thermostat")
BUSY_DELAY_MS = 800   # page changes take 120 to 650 ms (measured): no flicker on a normal one
BUSY_STEP_MS = 300
BUSY_GLYPH = "EEA79D"  # Material Design Icons "circle-medium" (boards/nspanel/nextion/icons.json), UTF-8
BUSY_DOT = 24          # a dot's box; side by side, not overlapping (they would repaint each other)


def busy_indicator(p: Project) -> None:
    dots = [f"busy{k}" for k in (1, 2, 3)]
    x0, y = (VISIBLE_W - 3 * BUSY_DOT) // 2, (layout.VISIBLE_H - BUSY_DOT) // 2
    for name in BUSY_PAGES:
        page = p.page(name)
        for k, dot in enumerate(dots):
            text(page, dot, (x0 + k * BUSY_DOT, y, BUSY_DOT, BUSY_DOT), ICON_FONT, xcen=1, maxl=4,
                 txt=bytes.fromhex(BUSY_GLYPH))
        timer(page, "busy_tm", BUSY_DELAY_MS, [
            f"if(busy_tm.tim!={BUSY_STEP_MS})",
            "{",
            f"  busy_tm.tim={BUSY_STEP_MS}",
            "  busy1.pco=th_accent",
        ] + [f"  vis {d},1" for d in dots] + [
            "}else if(busy1.pco==th_accent)",
            "{",
            "  busy1.pco=th_dim",
            "  busy2.pco=th_accent",
            "}else if(busy2.pco==th_accent)",
            "{",
            "  busy2.pco=th_dim",
            "  busy3.pco=th_accent",
            "}else",
            "{",
            "  busy3.pco=th_dim",
            "  busy1.pco=th_accent",
            "}",
        ])
        root = page.objects()[0]
        root.event("codesload", [f"vis {d},0" for d in dots] + [f"{d}.pco=th_dim" for d in dots]
                   + [f"busy_tm.tim={BUSY_DELAY_MS}", "busy_tm.en=1"] + root.code("codesload"))


def crops(page) -> list:
    """The objects of a page that crop from the default background."""
    return [o for o in page.objects()[1:]
            if o.c.attribute("picc") and o.c.attribute("sta") and o.get("sta") == 0 and o.get("picc") == BG]


def background_pages(p: Project) -> list[str]:
    """The pages that show the configurable background."""
    return [name for name, page in p.pages.items()
            if page.objects()[0].get("sta") == 2 and page.objects()[0].get("pic") == BG]


def no_close_on_start(p: Project) -> None:
    """The start page (the swipe row's first, when it is not home) has no close cross: the firmware sets no_close
    (Program.s) before it opens it (Engine::show_physical), and every page with a cross hides it as it loads -- after
    whatever its own loading showed, so it does not flash up."""
    for page in p.pages.values():
        if any(o.name == "close" for o in page.objects()[1:]):
            on_load(page, ["if(no_close==1)", "{", "  vis close,0", "  vis close_hs,0", "}"])


def hidden_until_drawn(p: Project, composites: set[int]) -> None:
    """A picture composed onto a background (compose.py: the logo, a weather
    icon, a tile) shows the default background's variant as designed; the
    firmware sets the one of the background in use only once the page has
    loaded -- so on a page that does not hide everything while it loads (vis
    255,0) such an object is hidden then, and the firmware shows it with its
    picture set (Engine::draw_boot, draw_weather, draw_settings,
    draw_volume_page). Not the close cross: the page takes it from bg_close
    before it draws (background_globals())."""
    for name, page in p.pages.items():
        root = page.objects()[0]
        if "vis 255,0" in root.code("codesload"):
            continue
        hide = [o.name for o in page.objects()[1:] if o.name != "close" and
                any(o.c.attribute(a) is not None and o.get(a) in composites for a in ("pic", "pic2"))
                and (o.get("type") == PICTURE or o.get("sta") == 2)]
        if hide:
            root.event("codesload", root.code("codesload") + [f"vis {n},0" for n in hide])


def background_globals(p: Project) -> None:
    """The chosen background as global variables of Program.s (bg_pic, and
    bg_close: the close cross baked onto it), set by the firmware
    (Engine::send_theme); every page that shows the background takes it in
    its preinitialize code -- the page's picture, its cropping fields and its
    close cross -- before it draws. Set after loading instead, each page showed
    the design's default background for a moment first."""
    for name in background_pages(p):
        page = p.page(name)
        lines = [f"{name}.pic=bg_pic"] + [f"{o.name}.picc=bg_pic" for o in crops(page)]
        if page.has("close"):
            lines.append("close.pic=bg_close")
        root = page.objects()[0]
        root.event("codesload", lines + root.code("codesload"))


def rgb565(hexcode: str) -> int:
    r, g, b = (int(hexcode[i:i + 2], 16) for i in (1, 3, 5))
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def int32(value: int) -> int:
    """Program.s only knows 4-byte signed integers."""
    return value - (1 << 32) if value >= 1 << 31 else value


def program(p: Project, version: str, bg: int, close_pic: int) -> None:
    """Program.s: what the display runs once at power-on. The global
    variables: swipex, swipey (swipe_detection()); the theme's colours (the
    default background's until the firmware sets them: Engine::send_theme);
    the design's version, which the firmware reads ("get design_ver") and
    compares with the manifest it was built with, to tell when the display
    needs the new design -- here, not on a page: the display announces "ready"
    (printh ... 88) before it opens the boot page, and the firmware asks right
    then (the 8 hex digits as one integer); the background's pictures (the
    default background's, background_globals()). Then the serial line's speed
    -- BAUD, for this run only ("baud=", not the stored "bauds="): the
    firmware talks to the design at it from the display's first byte on and
    never switches --, the touch panel's offset (on the NSPanel EU), and the
    splash page: the display knows neither the background nor the theme
    configured until the firmware has found it and sent them, so it shows
    only the logo on black before (its boot page in the default theme flashed
    up); the firmware then opens the boot page, in the configured theme."""
    sys.path.insert(0, os.path.join(HERE, "assets"))
    from generate import THEMES
    theme = THEMES[DEFAULT_BACKGROUND]
    accent, text_colour, dim = (rgb565(theme[k]) for k in ("accent", "text", "dim"))
    here = "boards/nspanel/nextion/ohpanel_hmi.py"
    p.program = "\r\n".join([
        f"//OHpanel's display design ({here}): run once at power-on",
        "int sys0=0,sys1=0,sys2=0//scratch",
        "int no_close=0//the page coming is the start page (not home): no close cross (no_close_on_start())",
        "int swipex=0,swipey=0//where the finger was last while it touched a page",
        f"int th_accent={accent},th_text={text_colour},th_dim={dim}//the theme's colours",
        f"int design_ver={int32(int(version, 16))}//the design's version {version}",
        f"int bg_pic={bg},bg_close={close_pic}//the background's pictures",
        f"baud={BAUD}//the serial line's speed, from here on",
        "recmod=0//commands, not raw data",
        "printh 00 00 00 ff ff ff 88 ff ff ff//started, ready",
        *([f"lcd_dev {layout.TOUCH_OFFSET}//the touch panel's offset on the NSPanel EU"] if layout.TOUCH_OFFSET else []),
        "page splash//the logo, on black, until the firmware takes over",
        "",
    ])


def drawn_boxes(p: Project) -> dict:
    """The manifest's "boxes" (page -> object -> [x, y, w, h]): the fields the
    firmware draws into itself -- a station logo, centred, in place of a
    favourite's icon (Engine::draw_logo; tools/station_logo.py makes logos
    up to 56 px, which fit), the cover over the art box (Engine::draw_cover),
    the play disc (Engine::draw_play), the colour wheel's ring and the colour
    chosen (Engine::draw_wheel_mark), the QR codes on their card
    (Engine::draw_code)."""
    box = lambda page, n: [p.page(page).obj(n).get(k) for k in ("x", "y", "w", "h")]
    favorites = {}
    for i, (x, y) in enumerate(layout.button_tiles(), start=1):
        dx, dy, w, h = layout.FAV_ICON
        favorites[f"tile{i}_center"] = [x + dx, y + dy, w, h]
    return {"buttons": favorites, "media": {"art_pic": box("media", "art_pic"), "play_hs": box("media", "play_hs")},
            "light": {"wheel_pic": box("light", "wheel_pic"), "swatch": box("light", "swatch")},
            "qr": {"qr_area": box("qr", "qr_area")},
            "cover": {n: box("cover", n) for n in ["draw_hs"] + [f"{b}{k}_hs" for b in ("btn", "preset") for k in range(1, 4)]},
            "covers": {f"{n}{part}": box("covers", f"{n}{part}")
                       for n in ["all"] + [f"row{r}" for r in range(1, layout.COVERS_ROWS_N + 1)]
                       for part in ("_mini", "_hs", "_up_hs", "_stop_hs", "_down_hs")} |
                      {f"preset{k}_hs": box("covers", f"preset{k}_hs") for k in range(1, 4)},
            "boot": {"qr_area": box("boot", "qr_area")},
            "thermostat": {n: box("thermostat", n) for n in ["ring_hs", "minus_hs", "plus_hs", "state_hs"] +
                           [f"m{n}{k}_hs" for n in range(1, 5) for k in range(1, n + 1)]},
            "keypad": {n: box("keypad", n) for n in ["state_hs"] +
                       [f"b{n}{k}_hs" for n in range(1, 5) for k in range(1, n + 1)]}}


def manifest(p: Project, bg_ids: dict[str, int], pictures: dict) -> dict:
    sys.path.insert(0, os.path.join(HERE, "assets"))
    from generate import BACKGROUNDS as TITLES, THEMES

    shown = background_pages(p)
    pages = {}
    for name, page in p.pages.items():
        entry = {"id": page.page_id}
        if name in shown:
            entry["background"] = True
            entry["crop"] = [o.name for o in crops(page)]
            if page.has("close"):
                entry["close"] = True
        pages[name] = entry
    return {
        "note": "generated by boards/nspanel/nextion/ohpanel_hmi.py -- do not edit",
        "backgrounds": [{"id": k, "title": TITLES[k][0], "pic": v, "theme": THEMES[k]} for k, v in bg_ids.items()],
        "default_background": DEFAULT_BACKGROUND,
        "pages": dict(sorted(pages.items(), key=lambda kv: kv[1]["id"])),
        "pictures": pictures,
    }


# ----------------------------------------------------------- pictures, fonts --

_INDEX: dict[str, dict] = {}


def picture_ids(path: str, folder: str) -> dict[str, int]:
    """Name -> picture id of every PNG in `folder`, found by comparing it with
    the previews in the project -- never by assuming where an import put it.
    Identical pictures map to the last one imported."""
    from PIL import Image

    if path not in _INDEX:
        index = {}
        for pid, png in sorted(picture_previews(read_container(open(path, "rb").read())).items()):
            img = Image.open(io.BytesIO(png)).convert("RGB")
            index[(img.size, img.tobytes())] = pid  # later imports win
        _INDEX[path] = index
    found = {}
    for f in sorted(glob.glob(os.path.join(folder, "*.png"))):
        want = Image.open(f).convert("RGB")
        name = os.path.splitext(os.path.basename(f))[0]
        pid = _INDEX[path].get((want.size, want.tobytes()))
        if pid is None:
            sys.exit(f"{name}.png is not in {path} -- import the pictures first (boards/nspanel/nextion/build.sh)")
        found[name] = pid
    return found


def composite_ids(path: str) -> dict:
    """Background -> slot -> picture -> id of the icons and tiles composed onto
    every background (boards/nspanel/nextion/assets/compose.py): what the firmware shows in
    the weather icon and button tile objects, by the background in use."""
    out: dict = {}
    for name, pid in picture_ids(path, os.path.join(HERE, "assets", "composites")).items():
        bg, slot, picture = name.split(".")
        out.setdefault(bg, {}).setdefault(slot, {})[picture] = pid
    return out


def prune(p: Project, keep: set[int]) -> dict[int, int]:
    """Removes every picture no page and nothing in `keep` (pictures the
    manifest names) uses -- the ones an import put in twice, say --; returns
    old id -> new id (pictures are numbered by their order: references are
    renumbered here)."""
    used = set(p.picture_refs()) | keep
    unused = [i for i in range(p.picture_count()) if i not in used]
    mapping = p.remove_pictures(unused)
    if unused:
        print(f"pruned: {len(unused)} pictures")
    return mapping


# Text fields the firmware shortens to fit, with "..." (boards/nspanel/nextion/display-manifest.json
# "fit"), whatever it writes into them (Engine::txt): what does not fit is cut
# off at the field's edge otherwise. Every text field of one line in a text
# font -- not the icon fonts (a field there holds an icon), not one that wraps
# its text (isbr), which the display breaks into lines itself.
ICON_FONTS = {ICON_FONT, ICON_FONT_LARGE, ICON_FONT_XL}
def font_metrics(path: str, font: int) -> dict:
    """Widths and blank margins (zifont.fit_metrics) of font number `font` of
    the built project."""
    entries = read_container(open(path, "rb").read())
    zi = [n.decode() for n in re.findall(rb"(\d+\.zi)\0", entries["main.HMI"])][font]
    return zifont.fit_metrics(entries[zi])


def fit(p: Project, path: str) -> tuple[dict, dict]:
    """The manifest's "fit" (page -> object -> [font, width, bytes]) of the text
    fields to fit, and "fonts" (font -> widths) of their fonts."""
    out, widths = {}, {}
    for page, edit in sorted(p.pages.items()):
        for obj in edit.objects():
            if obj.get("type") != TEXT:
                continue
            font = obj.get("font")
            if font in ICON_FONTS or obj.get("isbr"):
                continue
            out.setdefault(page, {})[obj.name] = [font, obj.get("w"), obj.get("txt_maxl")]
            if str(font) not in widths:
                widths[str(font)] = font_metrics(path, font)
    return out, dict(sorted(widths.items()))


# Program.s while the design's version is worked out (design_hash): the version cannot be part of what it is made from
NO_VERSION = "00000000"


def design_hash(path: str) -> str:
    """The design's version: the content of the built project (`path`, saved
    with NO_VERSION in Program.s) -- every page, picture and font, Program.s,
    the display's direction: all the TFT is compiled from. So a change to the
    recipe that leaves the design as it was keeps its version, and a panel
    needs no new display firmware for it. The container itself is left out,
    it changes with every save without its content changing."""
    h = hashlib.sha256()
    for name, data in sorted(read_container(open(path, "rb").read()).items()):
        h.update(name.encode() + b"\0" + data)
    return h.hexdigest()[:8]


def own_fonts(p: Project) -> None:
    """OHpanel's fonts (boards/nspanel/nextion/fonts.py, made from the TrueType fonts in
    boards/nspanel/nextion/assets/fonts), font id for font id."""
    if p.font_files():
        sys.exit(f"{len(p.font_files())} fonts in the seed: it should have none")
    for data in fonts.build(os.path.join(HERE, "..", "..", "..", "build", "fonts")):
        p.add_font(data)


# Every page in the order of their ids (the firmware goes by the manifest).
PAGES = ("home", "weather", "settings", "boot", "blank", "light", "cover", "buttons", "volpage", "qr", "entities",
         "media", "system", "netpage", "covers", "thermostat", "splash", "keypad")   # entities: entities_cards and entities_rows come with it


def main():
    global BG, PICTURES, ALL_PICTURES
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    bg_ids = picture_ids(src, BACKGROUNDS)
    print("backgrounds:", ", ".join(f"{k}={v}" for k, v in bg_ids.items()))
    pictures = composite_ids(src)
    BG, PICTURES, ALL_PICTURES = bg_ids[DEFAULT_BACKGROUND], pictures[DEFAULT_BACKGROUND], pictures

    p = Project(src)
    if list(p.pages) != [SEED_PAGE]:
        sys.exit(f"{src}: not the seed (pages {list(p.pages)})")
    for o in p.page(SEED_PAGE).objects()[1:]:
        TEMPLATES[o.get("type")] = o
    p.direction = DIRECTION
    own_fonts(p)

    builders = {"home": home_page, "weather": weather_page, "settings": settings_page, "boot": boot_page,
                "blank": blank_page, "light": lambda p: control_page(p, "light"),
                "cover": cover_page, "buttons": buttons_page, "volpage": volume_page,
                "qr": qr_page, "entities": entities_pages, "media": media_page, "system": system_page,
                "netpage": network_page, "covers": covers_page, "thermostat": thermostat_page, "splash": splash_page,
                "keypad": keypad_page}
    for name in PAGES:
        builders[name](p)
    p.remove_pages([SEED_PAGE])

    theme_colours(p)
    busy_indicator(p)
    background_globals(p)
    hidden_until_drawn(p, {pid for bg in pictures.values() for slot in bg.values() for pid in slot.values()})
    no_close_on_start(p)
    check_icons(p)

    print("composites:", sum(len(v) for s in pictures.values() for v in s.values()), "pictures")
    keep = set(bg_ids.values()) | {pid for bg in pictures.values() for slot in bg.values() for pid in slot.values()}
    mapping = prune(p, keep)
    bg_ids = {k: mapping[v] for k, v in bg_ids.items()}
    BG = bg_ids[DEFAULT_BACKGROUND]   # what the pages show from here on (the manifest's crop lists)
    pictures = {bg: {slot: {name: mapping[pid] for name, pid in pics.items()} for slot, pics in slots.items()}
                for bg, slots in pictures.items()}
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    program(p, NO_VERSION, bg_ids[DEFAULT_BACKGROUND], pictures[DEFAULT_BACKGROUND]["close"]["x"])
    changed = p.save(out)
    version = design_hash(out)
    print(f"design version: {version}")
    program(p, version, bg_ids[DEFAULT_BACKGROUND], pictures[DEFAULT_BACKGROUND]["close"]["x"])
    p.save(out)
    print(f"{out}: {len(changed)} sections changed")

    m = manifest(p, bg_ids, pictures)
    m["fit"], m["fonts"] = fit(p, out)
    m["dots"] = dots_max()   # how many page dots fit in their field
    m["units"] = UNITS
    m["range"] = RANGE
    # the weather page's low after the high (Engine::draw_weather): where its places start, a step apart, how many;
    # the high's font and where it starts
    m["weather_line"] = {"x0": layout.WEATHER_LINE_LOW_X0, "step": layout.WEATHER_LINE_STEP,
                         "n": layout.WEATHER_LINE_STEPS, "font": 4, "high_x": layout.WEATHER_LINE_HIGH[0]}
    m["boxes"] = drawn_boxes(p)
    # the display as the design is turned, and what of it shows (Engine::draw_note centres its card in that)
    m["screen"] = {"w": layout.SCREEN_W, "h": layout.SCREEN_H, "visible_w": layout.VISIBLE_W, "visible_h": layout.VISIBLE_H}
    m["variant"] = layout.VARIANT
    # every object by page: what the firmware may name (tools/check_names.py checks Engine*.cpp against it)
    m["objects"] = {name: [o.name for o in page.objects()[1:]] for name, page in p.pages.items()}
    m["design"] = version
    with open(MANIFEST, "w") as f:
        json.dump(m, f, indent=1)
        f.write("\n")
    print(f"{MANIFEST}: written")


if __name__ == "__main__":
    main()
