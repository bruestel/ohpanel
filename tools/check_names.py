#!/usr/bin/env python3
"""Checks that every object the firmware names by a fixed string exists in the
display design: the names in components/app_display/src/Engine*.cpp against
the manifests' "objects" (boards/nspanel/nextion/display-manifest-<variant>.json, written by
boards/nspanel/nextion/ohpanel_hmi.py), of every panel variant built. A name left behind after a rename sends commands
to an object that is not there -- the display only answers with an error.

A string that is put together with more ("tile" + n + "_pic") counts when an
object begins with it. Run by test/host/run.sh."""

import glob
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ENGINE = sorted(glob.glob(os.path.join(ROOT, "components", "app_display", "src", "Engine*.cpp")))
MANIFESTS = sorted(glob.glob(os.path.join(ROOT, "boards", "nspanel", "nextion", "display-manifest-*.json")))

# calls whose first argument is an object's name
CALLS = r"\b(?:txt|num|vis|show_text|show_icon|show_frames|page_colour|draw_code)\("


def main():
    if not MANIFESTS:
        sys.exit("names: no display manifest -- build the design (boards/nspanel/nextion/build.sh)")
    found = []
    for path in ENGINE:
        found += names_in(path)
    failed = False
    for manifest in MANIFESTS:
        design = os.path.basename(manifest)
        objects = json.load(open(manifest)).get("objects")
        if not objects:
            sys.exit(f"names: {design} has no \"objects\" -- build the design (boards/nspanel/nextion/ohpanel_hmi.py)")
        names = {n for page in objects.values() for n in page}
        missing = sorted({(name, where) for name, prefix, where in found
                          if name not in names and not (prefix and any(n.startswith(name) for n in names))},
                         key=lambda x: x[1])
        for name, where in missing:
            print(f"names: {where}: no object \"{name}\" in the design ({design})", file=sys.stderr)
        failed |= bool(missing)
        if not missing:
            print(f"names: all {len({n for n, _, _ in found})} object names in Engine*.cpp are in the design ({design})")
    if failed:
        sys.exit(1)


def names_in(path):
    """(name, put together with more, file:line) of each object name in the file."""
    source = open(path).read()
    where = lambda at: f"{os.path.basename(path)}:{source.count(chr(10), 0, at) + 1}"
    found = []
    # a name as the first argument, a name in a list the loop after it shows or sets, "ref x" / "vis x" sent as is
    for m in re.finditer(CALLS + r'"([a-z][a-z0-9_]*)"(\s*\+)?', source):
        found.append((m.group(1), bool(m.group(2)), where(m.start())))
    for m in re.finditer(r'for \(const (?:char \*|std::string &)\w+ : \{([^}]*)\}\)\s*\n?\s*(?:vis|num|txt)\(', source):
        for n in re.finditer(r'"([a-z][a-z0-9_]*)"', m.group(1)):
            found.append((n.group(1), False, where(m.start())))
    for m in re.finditer(r'send\("(?:ref|vis) ([a-z][a-z0-9_]*)', source):
        found.append((m.group(1), False, where(m.start())))
    return found


if __name__ == "__main__":
    main()
