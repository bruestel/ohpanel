"""
What the firmware needs to know about the display design, all embedded in it:

components/app_display/src/display_assets.c:
- the display manifest (boards/nspanel/nextion/display-manifest.json: pages, background
  pictures, objects that follow the background) and the weather icon sets
  (boards/nspanel/nextion/weather-icons.json), as text;
- the version of the icon set (assets_version).

The icon set, components/app_display/src/icon_set.bin (embedded with
EMBED_FILES, components/app_display/CMakeLists.txt):
- the icon table (boards/nspanel/nextion/icons.json: Material Design Icons name -> UTF-8
  glyph of the display's icon font, and OHpanel's own icons,
  boards/nspanel/nextion/own_icons.py), which the firmware inflates into PSRAM;
- for the web interface (preview, icon chooser), so it needs no icon font
  from the internet: a 24 px picture of every glyph, 2 bits per pixel, and
  the categories and aliases of the icons (boards/nspanel/nextion/icon-meta.json).

The icon set, little endian:

    0   magic "OHASSET2"
    8   u32 version: CRC-32 of everything after the header
    12  u32 length of everything after the header
    16  u32 number of icons
    20  u32 table size, inflated
    24  u32 table size, compressed
    28  u32 picture size in pixels (24)
    32  the table, zlib: "name\\0" + 3 glyph bytes per icon, sorted by name
        (all the firmware reads), then for the web interface:
        u32 number of glyphs, u32 pictures inflated, u32 pictures compressed,
        u32 meta inflated, u32 meta compressed
        the pictures, LZMA ("alone" format): per glyph (U+E000 + n) size*size
        pixels, row by row, 2 bits each (0 transparent .. 3 opaque), the first
        pixel in the top bits of a byte
        the meta, LZMA: JSON {"version", "categories": [name], "of": [per
        glyph its category numbers], "aliases": [per glyph its aliases]}

LZMA, not zlib, for these: the pictures take a third less, which is what lets
the newest icons, their categories and aliases fit the firmware's slot. The browser
has no LZMA, the web interface brings a decoder.

The pictures are drawn from the Material Design Icons font
(boards/nspanel/nextion/assets/fonts) by the code point the display's font has for the icon
(U+E000 + n there is U+F0001 + n in the font), so the web interface shows what
the panel shows, also for icons the font has renamed since.

Runs as a PlatformIO pre-build hook and standalone (the CMake fallback of a
first configure, and):
    python3 tools/gen_display_assets.py [--raw table.bin] [--image icon_set.bin]
"""

import hashlib
import json
import pathlib
import struct
import sys
import zlib

try:
    Import("env")  # noqa: F821  (injected by PlatformIO)
    ROOT = pathlib.Path(env.subst("$PROJECT_DIR"))  # noqa: F821
except NameError:
    env = None
    ROOT = pathlib.Path(__file__).resolve().parent.parent

SRC = ROOT / "boards" / "nspanel" / "nextion" / "icons.json"
FONT = ROOT / "boards" / "nspanel" / "nextion" / "assets" / "fonts" / "materialdesignicons-webfont.ttf"
OUT = ROOT / "components" / "app_display" / "src" / "display_assets.c"
ICON_SET = ROOT / "components" / "app_display" / "src" / "icon_set.bin"
MANIFEST = ROOT / "boards" / "nspanel" / "nextion" / "display-manifest.json"
WEATHER = ROOT / "boards" / "nspanel" / "nextion" / "weather-icons.json"
SCRIPT = ROOT / "tools" / "gen_display_assets.py"
META = ROOT / "boards" / "nspanel" / "nextion" / "icon-meta.json"
MAGIC = b"OHASSET2"
HEADER = 32
SIZE = 24


def c_json_z(name, path):
    """A JSON file, minified and zlib-compressed (Display.cpp inflates it with
    the ROM's inflater): `name`_z, its length `name`_z_size and the text's
    length `name`_size."""
    import json
    text = json.dumps(json.loads(path.read_text()), separators=(",", ":"), ensure_ascii=False).encode()
    packed = zlib.compress(text, 9)
    body = ",".join(f"0x{b:02x}" for b in packed)
    return (f"const unsigned char {name}_z[] = {{{body}}};\n"
            f"const uint32_t {name}_z_size = {len(packed)}u;\n"
            f"const uint32_t {name}_size = {len(text)}u;\n")


def own():
    """OHpanel's own icons (boards/nspanel/nextion/own_icons.py)."""
    sys.path.insert(0, str(ROOT / "boards" / "nspanel" / "nextion"))
    import own_icons
    return own_icons


def icons():
    """(name, glyph bytes) sorted by name: the MDI table and OHpanel's own icons."""
    out = []
    table = json.loads(SRC.read_text())
    for name, code in own().code_points().items():
        if name in table:
            raise SystemExit(f"gen_display_assets: own icon {name} is in the MDI table too")
        table[name] = chr(code).encode().hex()
    for name, glyph in sorted(table.items()):
        glyph = bytes.fromhex(glyph)
        if len(glyph) != 3:
            raise SystemExit(f"gen_display_assets: {name}: glyph is not 3 bytes")
        out.append((name, glyph))
    return out


def raw_table():
    return b"".join(name.encode("ascii") + b"\0" + glyph for name, glyph in icons())


def glyph_count():
    """Pictures for the glyphs U+E000 .. U+E000 + n - 1: up to the highest the table uses."""
    return max(ord(g.decode("utf-8")) for _, g in icons() if g != "\uffff".encode()) - 0xE000 + 1


def pictures():
    """The 2-bit pictures of all glyphs."""
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.truetype(str(FONT), SIZE)
    ascent, descent = font.getmetrics()
    baseline = round(SIZE * ascent / (ascent + descent))  # the em square fills the picture
    mine = {code - 0xE000: own().icons()[name][0] for name, code in own().code_points().items()}
    out = bytearray()
    for n in range(glyph_count()):
        if n in mine:
            img = mine[n](SIZE)
        else:
            img = Image.new("L", (SIZE, SIZE), 0)
            ImageDraw.Draw(img).text((0, baseline), chr(0xF0001 + n), font=font, fill=255, anchor="ls")
        levels = (np.asarray(img, dtype=np.uint16) * 3 + 127) // 255
        flat = levels.reshape(-1, 4)
        out += (flat[:, 0] << 6 | flat[:, 1] << 4 | flat[:, 2] << 2 | flat[:, 3]).astype(np.uint8).tobytes()
    return bytes(out)


def meta():
    """Categories and aliases per glyph, as JSON."""
    src = json.loads(META.read_text())
    n = glyph_count()
    of, aliases = [[] for _ in range(n)], [[] for _ in range(n)]
    table = dict(icons())
    for name, info in src["icons"].items():
        k = ord(table[name].decode("utf-8")) - 0xE000
        of[k], aliases[k] = info["c"], info["a"]
    categories = src["categories"] + [own().CATEGORY]
    for name, (_, words) in own().icons().items():
        k = ord(table[name].decode("utf-8")) - 0xE000
        of[k], aliases[k] = [len(categories) - 1], words
    return json.dumps({"version": src["version"], "categories": categories, "of": of, "aliases": aliases},
                      separators=(",", ":"), ensure_ascii=False).encode()


def lzma_alone(data):
    import lzma
    return lzma.compress(data, format=lzma.FORMAT_ALONE,
                         filters=[{"id": lzma.FILTER_LZMA1, "preset": 9 | lzma.PRESET_EXTREME}])


def image():
    """The icon set; cached in the build directory by its inputs."""
    key = hashlib.sha256()
    for path in [SRC, META, FONT, SCRIPT] + [pathlib.Path(p) for p in own().files()]:
        key.update(path.read_bytes())
    cache = ROOT / ".pio" / "assets-cache" / (key.hexdigest()[:16] + ".bin")
    if cache.is_file():
        return cache.read_bytes()
    try:
        import numpy  # noqa: F401
        import PIL  # noqa: F401
    except ImportError:
        # PlatformIO's Python may lack them: drawing goes to the system's
        import subprocess
        cache.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["python3", str(SCRIPT), "--image", str(cache)], check=True)
        return cache.read_bytes()
    table = raw_table()
    table_z = zlib.compress(table, 9)
    pics, info = pictures(), meta()
    pics_z, info_z = lzma_alone(pics), lzma_alone(info)
    payload = table_z + struct.pack("<5I", glyph_count(), len(pics), len(pics_z), len(info), len(info_z)) \
        + pics_z + info_z
    blob = MAGIC + struct.pack("<6I", zlib.crc32(payload), len(payload), len(icons()), len(table), len(table_z),
                               SIZE) + payload
    assert len(blob) - len(payload) == HEADER
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(blob)
    return blob


def generate(blob):
    version = struct.unpack_from("<I", blob, 8)[0]
    text = (
        "/* Generated by tools/gen_display_assets.py -- do not edit. */\n"
        "#include <stdint.h>\n"
        f"const uint32_t assets_version = 0x{version:08x}u;\n"
        + c_json_z("display_manifest_json", MANIFEST)
        + c_json_z("weather_icons_json", WEATHER)
    )
    if OUT.is_file() and OUT.read_text() == text:
        return
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)


def main():
    args = sys.argv[1:]
    if env is None and args:
        for flag, path in zip(args[::2], args[1::2]):
            if flag == "--raw":
                pathlib.Path(path).write_bytes(raw_table())
            elif flag == "--image":
                pathlib.Path(path).write_bytes(image())
            else:
                sys.exit(__doc__)
        return
    blob = image()
    generate(blob)
    if not ICON_SET.exists() or ICON_SET.read_bytes() != blob:  # unchanged: no rebuild
        ICON_SET.write_bytes(blob)
    print(f"gen_display_assets: {struct.unpack_from('<I', blob, 16)[0]} icons, {len(blob)} bytes "
          f"(version {struct.unpack_from('<I', blob, 8)[0]:08x}), manifest, weather icon sets")


main()
