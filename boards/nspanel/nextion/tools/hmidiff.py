#!/usr/bin/env python3
"""Compare two Nextion Editor projects (.HMI) per page and object.

Usage:
  hmidiff.py <old.HMI> <new.HMI>

Reports added/removed pages and objects, changed attributes and changed event code.
Objects are matched by page name and objname.
"""

import difflib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hmi2txt import decode_value, page_name, page_order, read_container  # noqa: E402


def parse_page(data):
    """Returns {objname: {"id": int, "attrs": {name: value}, "events": {event: [lines]}}} in display order."""
    objects = {}
    current = None
    event = None
    position = data.find(b"att-") - 4
    while position + 4 <= len(data):
        (length,) = struct.unpack_from("<I", data, position)
        position += 4
        if position + length > len(data):
            break
        record = data[position:position + length]
        position += length
        text = record.decode("latin1")

        if re.fullmatch(r"att-\d+", text):
            current = {"attrs": {}, "events": {}}
            event = None
        elif re.fullmatch(r"codes\w+-\d+", text):
            event = text[5:text.rindex("-")]
            current["events"][event] = []
        elif event is None and length >= 16 and re.match(rb"[a-z_0-9]+\0+$", record[:16]):
            name = record[:16].split(b"\0")[0].decode()
            current["attrs"][name] = decode_value(name, record[16:])
            if name == "objname":
                objects[current["attrs"][name].strip('"')] = current
        elif event is not None and record.strip():
            current["events"][event].append(record.decode("utf8", "replace"))
    for obj in objects.values():
        obj["events"] = {k: v for k, v in obj["events"].items() if any(line.strip() for line in v)}
    return objects


def load(path):
    entries = read_container(open(path, "rb").read())
    pages = {}
    for page_id, number in enumerate(page_order(entries["main.HMI"])):
        data = entries[f"{number}.pa"]
        pages[page_name(data)] = (page_id, parse_page(data))
    return pages, entries.get("Program.s", b"").decode("utf8", "replace")


def diff_lines(old, new, indent):
    for line in difflib.unified_diff(old, new, lineterm="", n=1):
        if line.startswith(("---", "+++")):
            continue
        print(indent + line)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    old_pages, old_program = load(sys.argv[1])
    new_pages, new_program = load(sys.argv[2])

    if old_program != new_program:
        print("== Program.s")
        diff_lines(old_program.splitlines(), new_program.splitlines(), "   ")

    for name in old_pages.keys() - new_pages.keys():
        print(f"== page removed: {name}")
    for name in new_pages.keys() - old_pages.keys():
        print(f"== page added: {name}")

    for name, (new_id, new_objects) in new_pages.items():
        if name not in old_pages:
            continue
        old_id, old_objects = old_pages[name]
        out = []
        if old_id != new_id:
            out.append(f"   page id {old_id} -> {new_id}")
        for objname in old_objects.keys() - new_objects.keys():
            out.append(f"   - object removed: {objname}")
        for objname in new_objects.keys() - old_objects.keys():
            out.append(f"   + object added: {objname} (id={new_objects[objname]['attrs'].get('id')})")
        changed = []
        for objname, new in new_objects.items():
            old = old_objects.get(objname)
            if old is None:
                continue
            attrs = [f"{k}: {old['attrs'].get(k)} -> {v}" for k, v in new["attrs"].items() if old["attrs"].get(k) != v]
            attrs += [f"{k}: {v} -> (removed)" for k, v in old["attrs"].items() if k not in new["attrs"]]
            events = [e for e in set(old["events"]) | set(new["events"]) if old["events"].get(e) != new["events"].get(e)]
            if attrs or events:
                changed.append((objname, attrs, events, old, new))
        if out or changed:
            print(f"== page {new_id} {name}")
            for line in out:
                print(line)
            for objname, attrs, events, old, new in changed:
                print(f"   * {objname}: " + ", ".join(attrs))
                for event in sorted(events):
                    print(f"     [{event} event]")
                    diff_lines(old["events"].get(event, []), new["events"].get(event, []), "       ")


if __name__ == "__main__":
    main()
