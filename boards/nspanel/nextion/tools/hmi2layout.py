#!/usr/bin/env python3
"""Extract the page layouts of a Nextion project (.HMI) for the web preview.

Writes the geometry, fonts and colours of every visible object on the pages
the panel configuration fills, the pictures they show (from the full-size
previews the Nextion Editor stores next to every picture), the selectable
backgrounds (from boards/nspanel/nextion/display-manifest-<variant>.json, written by the build) and the
weather icon sets (boards/nspanel/nextion/weather-icons.json), as JSON for the
web interface, which loads it from /api/v1/layout (the firmware embeds it):

  hmi2layout.py build/ohpanel.HMI www/layout-eu.json   (part of boards/nspanel/nextion/build.sh)

for the panel variant of OHPANEL_VARIANT (boards/nspanel/nextion/layout.py), which the layout names
with the display's size and what of it shows (the preview's size).

Run it again whenever the HMI or one of the catalogues changes.
"""

import base64
import io
import json
import os
import re
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hmi2txt import page_name, page_order, read_container  # noqa: E402
import zifont  # noqa: E402
from hmidiff import parse_page  # noqa: E402
from hmipatch import Project  # noqa: E402

# The pages the configuration fills; one template per page type.
PAGES = ["home", "buttons", "entities", "entities_cards", "entities_rows", "cover", "covers", "light", "system",
         "media", "weather", "qr", "settings", "thermostat"]

# Object types that draw something: text, scrolling text, button, picture,
# slider, number, dual-state button, QR code, progress bar.
VISIBLE = {116, 55, 98, 112, 1, 54, 53, 58, 106}

def drawn_areas():
    """The manifest's "boxes": page -> objects the firmware draws into itself
    (the QR page's code, station logos), kept in the layout whatever they are."""
    path = MANIFEST
    try:
        return json.load(open(path)).get("boxes", {})
    except (OSError, ValueError):
        return {}


# The attributes the preview uses, by their short name in the output.
ATTRS = {"objname": "n", "type": "t", "x": "x", "y": "y", "w": "w", "h": "h", "font": "f", "pco": "pco",
         "bco": "bco", "sta": "sta", "xcen": "xc", "ycen": "yc", "txt": "txt", "pic": "pic",
         "wid": "wid", "hig": "hig"}


def fonts(entries):
    """Font id -> name and pixel height, in the order main.HMI lists them."""
    order = re.findall(rb"(\d+)\.zi\0", entries["main.HMI"])
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import fonts as recipe_fonts
    out = []
    for font_id, number in enumerate(order):
        data = entries[f"{number.decode()}.zi"]
        name = data[44:44 + data[0x20]].decode("latin1")
        entry = {"name": name, "height": data[7], **char_metrics(data)}
        # where capitals and digits stand in the cell: the preview's own font is moved to stand there
        if entry["widths"] and (cap := zifont.cap_rows(data)):
            entry["cap"] = cap
        # where its icons' em square lies in the cell (boards/nspanel/nextion/fonts.py icon_em): the
        # preview places an icon picture as the display draws the character
        if font_id < len(recipe_fonts.FONTS) and any(p[0] == recipe_fonts.MDI for p in recipe_fonts.FONTS[font_id][2]):
            em, top = recipe_fonts.icon_em(font_id)
            entry["icon"] = {"em": em, "top": top}
        out.append(entry)
    return out


def char_metrics(data):
    """Widths and blank margins of the characters the firmware fits by
    (zifont.fit_metrics), so the preview draws a text as wide as the display
    does (the browser has a different font); none for a font that is not utf-8."""
    if data[44 + data[0x11] - 5:44 + data[0x11]] != b"utf-8":
        return {"first": zifont.FIT_RUNS[0][0], "widths": [], "lead": [], "trail": []}
    return zifont.fit_metrics(data)


# Besides page backgrounds the preview draws the relay indicators (off/on,
# their ids in the manifest). Weather icons and button tiles come from
# boards/nspanel/nextion/assets with their transparency, which the browser has (the display
# gets a variant per background instead, see boards/nspanel/nextion/assets/compose.py).
ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
sys.path.insert(0, ASSETS)
import compose  # noqa: E402


def weather_drop(path):
    """How many px further down home's weather icon is than the centred one
    the preview draws (compose.fit_icon with bottom=True)."""
    size = 116
    img = Image.open(path).convert("RGBA")
    a = compose.fit_icon(img, size, size).getchannel("A").getbbox()
    b = compose.fit_icon(img, size, size, bottom=True).getchannel("A").getbbox()
    return (b[1] - a[1]) if a and b else 0


def alpha_uri(path, size=None):
    """An RGBA PNG, reduced to a palette with alpha to keep the page small;
    a weather icon (`size` given: the largest it shows at) filling its place
    as compose.py makes it for the display."""
    img = Image.open(path).convert("RGBA")
    if size:
        img = compose.fit_icon(img, size, size)
    img = img.quantize(colors=64, method=Image.Quantize.FASTOCTREE)
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()

MANIFEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", f"display-manifest-{compose.layout.VARIANT}.json")
WEATHER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "weather-icons.json")


def picture_previews(entries):
    """Picture id -> PNG bytes of the editor's preview, where it is full size."""
    order = [int(n) for n in re.findall(rb"(\d+)\.i\0", entries["main.HMI"])]
    out = {}
    for pid, number in enumerate(order):
        info, preview = entries.get(f"{number}.i", b""), entries.get(f"{number}.is", b"")
        start = preview.find(b"\x89PNG")
        if len(info) < 16 or start < 0:
            continue
        width, height = struct.unpack_from("<HH", info, 12)
        png = preview[start:]
        pw, ph = struct.unpack(">II", png[16:24])
        if (pw, ph) == (width, height):  # small pictures only have a thumbnail
            out[pid] = png
    return out


def data_uri(png, jpeg=False):
    if jpeg:
        buf = io.BytesIO()
        img = Image.open(io.BytesIO(png)).convert("RGBA")
        # Transparent parts become the display's black, as on the panel.
        flat = Image.new("RGB", img.size, (0, 0, 0))
        flat.paste(img, mask=img.split()[3])
        flat.save(buf, "JPEG", quality=80, optimize=True)
        return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()
    return "data:image/png;base64," + base64.b64encode(png).decode()


EVENT = re.compile(r'\\"component\\": \\"([A-Za-z0-9_]+)\\", \\"value\\": \\"([A-Za-z0-9_]+)\\"')


def touch_areas(page):
    """What a touch reports to the firmware, so the preview can act like the
    panel: the objects whose release sends an event, topmost last."""
    out = []
    for obj in page.objects()[1:]:
        try:
            code = obj.code("codesup")
        except Exception:
            continue
        for line in code:
            m = EVENT.search(line)
            if m and m.group(1) not in ("swipe", "touch", "currentpage"):
                out.append({"n": obj.name, "x": obj.get("x"), "y": obj.get("y"), "w": obj.get("w"),
                            "h": obj.get("h"), "c": m.group(1), "v": m.group(2)})
                break
    return out


def value(raw):
    if raw.startswith('"'):
        return raw.strip('"')
    try:
        return int(raw)
    except ValueError:
        return raw


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    source, target = sys.argv[1], sys.argv[2]
    entries = read_container(open(source, "rb").read())

    project = Project(source)
    previews = picture_previews(entries)
    lay = compose.layout
    layout = {"variant": lay.VARIANT, "screen": [lay.SCREEN_W, lay.SCREEN_H], "visible": [lay.VISIBLE_W, lay.VISIBLE_H],
              "fonts": fonts(entries), "pages": {}, "pictures": {}, "backgrounds": [], "weather": {}}
    for number in page_order(entries["main.HMI"]):
        data = entries[f"{number}.pa"]
        name = page_name(data)
        if name not in PAGES:
            continue
        objects = parse_page(data)
        page_obj = objects.get(name, {"attrs": {}})["attrs"]
        page = {"pic": value(page_obj.get("pic", "0")), "objects": []}
        drawn = drawn_areas().get(name, {})   # areas the firmware draws in itself (invisible hotspots)
        for objname, obj in objects.items():
            a = obj["attrs"]
            if int(a.get("type", 0)) not in VISIBLE and objname not in drawn:
                continue
            page["objects"].append({short: value(a[long]) for long, short in ATTRS.items() if long in a})
        page["touch"] = touch_areas(project.page(name))
        layout["pages"][name] = page
        if page["pic"] in previews:
            layout["pictures"][page["pic"]] = data_uri(previews[page["pic"]], jpeg=True)

    manifest = json.load(open(MANIFEST))
    for bg in manifest["backgrounds"]:
        if bg["pic"] not in previews:
            sys.exit(f"background {bg['id']}: picture {bg['pic']} has no full-size preview in the HMI")
        layout["pictures"].setdefault(bg["pic"], data_uri(previews[bg["pic"]], jpeg=True))
        layout["backgrounds"].append(bg)
    layout["default_background"] = manifest["default_background"]
    # text fields the firmware shortens to fit (Engine::fitted; the widths are in "fonts")
    layout["fit"] = manifest.get("fit", {})
    layout["units"] = manifest.get("units", {})   # unit places after a number (Engine::show_number)
    layout["range"] = manifest.get("range", {})   # today's low and high (Engine::draw_range)

    for name, page in layout["pages"].items():
        page["background"] = manifest["pages"].get(name, {}).get("background", False)

    for set_id, icon_set in json.load(open(WEATHER)).items():
        layout["weather"][set_id] = icon_set
        for name in sorted(set(icon_set["codes"].values()) | {icon_set["default"]}):
            # drawn at 200 px; the preview shows them at home's size at most
            # home's icon stands on its field's bottom (compose.py): how far below the centred picture
            layout.setdefault("weather_drop", {})[name] = weather_drop(os.path.join(ASSETS, "weather", name + ".png"))
            layout["pictures"].setdefault("weather:" + name, alpha_uri(os.path.join(ASSETS, "weather", name + ".png"),
                                                                       compose.layout.WEATHER_HOME[2]))
    # the button pages' tiles and the settings page's restart button (wide_*)
    for name in ("off", "on", "wide_off", "side_off", "side_on"):
        layout["pictures"]["tile:" + name] = alpha_uri(os.path.join(lay.ASSETS, "tiles", name + ".png"))
    buf = io.BytesIO()
    compose.close_icon().save(buf, "PNG", optimize=True)
    layout["pictures"]["close:x"] = data_uri(buf.getvalue())

    # one line, keys in the order written: unchanged input gives the same file, and git sees no change
    blob = json.dumps(layout, ensure_ascii=False, separators=(",", ":"))
    open(target, "w", encoding="utf-8").write(blob)
    print(f"{len(layout['pages'])} pages, {sum(len(p['objects']) for p in layout['pages'].values())} objects, "
          f"{len(blob)} bytes -> {target}")


if __name__ == "__main__":
    main()
