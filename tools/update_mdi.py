#!/usr/bin/env python3
"""
Brings the Material Design Icons to a release of the npm packages @mdi/font
and @mdi/svg (the newest by default), which is all the panel's icons are made
from:

- boards/nspanel/nextion/assets/fonts/materialdesignicons-webfont.ttf and MDI-LICENSE.txt
  (@mdi/font): the display's icon fonts (boards/nspanel/nextion/fonts.py) and the pictures
  of the web interface (tools/gen_display_assets.py) are drawn from it;
- boards/nspanel/nextion/icons.json: icon name -> UTF-8 glyph on the display, U+E000 + n for
  the font's U+F0001 + n (the display's fonts have 16-bit code points);
- boards/nspanel/nextion/icon-meta.json: the release, its categories, and per icon its
  categories and aliases (other words it is known by) for the icon chooser
  (@mdi/svg's meta.json, the source of pictogrammers.com/library/mdi too).

Names stay valid across releases where they can: a name of the table before
that the release has as an alias of an icon names that icon. "blank" (draws
nothing), "unknown" and "unavailable" are the panel's own.

Usage:
    python3 tools/update_mdi.py [version]
"""

import io
import json
import pathlib
import sys
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONTS = ROOT / "boards" / "nspanel" / "nextion" / "assets" / "fonts"
TABLE = ROOT / "boards" / "nspanel" / "nextion" / "icons.json"
META = ROOT / "boards" / "nspanel" / "nextion" / "icon-meta.json"
REGISTRY = "https://registry.npmjs.org/@mdi/"
FIRST = 0xF0001          # the font's first icon
DISPLAY = 0xE000         # ... on the display
OWN = {"blank": "￿", "unknown": "", "unavailable": ""}   # alert-circle-outline


def package(name: str, version: str) -> tuple[str, dict[str, bytes]]:
    """The files of an npm package release (version "latest" or a number)."""
    with urllib.request.urlopen(REGISTRY + f"{name}/{version}", timeout=30) as r:
        info = json.load(r)
    with urllib.request.urlopen(info["dist"]["tarball"], timeout=120) as r:
        tar = tarfile.open(fileobj=io.BytesIO(r.read()))
    files = {m.name.removeprefix("package/"): tar.extractfile(m).read() for m in tar.getmembers() if m.isfile()}
    return info["version"], files


def main():
    wanted = sys.argv[1] if len(sys.argv) > 1 else "latest"
    version, font = package("font", wanted)
    svg_version, svg = package("svg", version)
    if svg_version != version:
        sys.exit(f"@mdi/svg {svg_version} does not match @mdi/font {version}")
    icons = sorted(json.loads(svg["meta.json"]), key=lambda i: i["name"])

    glyph = {i["name"]: chr(DISPLAY + int(i["codepoint"], 16) - FIRST) for i in icons}
    if max(map(ord, glyph.values())) >= 0xFFFF:
        sys.exit("more icons than the display's 16-bit code points hold")
    alias_of = {a: i["name"] for i in icons for a in i["aliases"]}

    table = dict(OWN)
    table.update(glyph)
    before = json.loads(TABLE.read_text()) if TABLE.exists() else {}
    kept, gone = [], []
    for name in before:
        if name in table:
            continue
        if name in alias_of:
            table[name] = glyph[alias_of[name]]
            kept.append(name)
        else:
            gone.append(name)

    categories = sorted({t for i in icons for t in i["tags"]})
    meta = {
        "version": version,
        "categories": categories,
        "icons": {i["name"]: {"c": [categories.index(t) for t in i["tags"]], "a": i["aliases"]}
                  for i in icons if i["tags"] or i["aliases"]},
    }

    (FONTS / "materialdesignicons-webfont.ttf").write_bytes(font["fonts/materialdesignicons-webfont.ttf"])
    (FONTS / "MDI-LICENSE.txt").write_bytes(font["LICENSE"])
    TABLE.write_text(json.dumps({n: table[n].encode().hex().upper() for n in sorted(table)}, indent=4,
                                ensure_ascii=False) + "\n")
    META.write_text(json.dumps(meta, separators=(",", ":"), ensure_ascii=False) + "\n")
    print(f"Material Design Icons {version}: {len(icons)} icons, {len(categories)} categories")
    if kept:
        print(f"kept as aliases: {', '.join(kept)}")
    if gone:
        print(f"no longer in the release (show as unknown): {', '.join(gone)}")


if __name__ == "__main__":
    main()
