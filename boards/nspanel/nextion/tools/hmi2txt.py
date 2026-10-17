#!/usr/bin/env python3
"""Decode a Nextion Editor project (.HMI) into readable, diffable text files.

The .HMI file is a container: a file table followed by the entries
(pages *.pa, fonts *.zi, pictures *.i/*.is, Program.s, main.HMI).
Each page is a sequence of length-prefixed records:

  att-N            starts an object with N attribute records
  <name:16><value> attribute (name zero padded to 16 bytes, value = int or string)
  codes<event>-N   starts an event with N lines of Nextion code

Usage:
  hmi2txt.py <project.HMI> <output dir>

Writes Program.s.txt and one file per page (<page id>_<page name>.txt).
The page ids are the ones used by the display (e.g. "page 8"), taken from main.HMI.
"""

import os
import re
import struct
import sys

STRING_ATTRIBUTES = {"objname", "txt", "path", "dir", "filter", "format"}


def read_container(data):
    entries = {}
    (count,) = struct.unpack_from("<I", data, 0)
    for i in range(count):
        offset = 4 + i * 28
        name = data[offset:offset + 16].split(b"\0")[0].decode("latin1")
        start, size = struct.unpack_from("<II", data, offset + 16)
        if name:
            entries[name] = data[start:start + size]
    return entries


def page_order(main_hmi):
    return [int(n) for n in re.findall(rb"(\d+)\.pa\0", main_hmi)]


def decode_value(name, value):
    if name in STRING_ATTRIBUTES or len(value) > 4:
        return '"' + value.split(b"\0")[0].decode("utf8", "replace") + '"'
    if not value:
        return ""
    return str(int.from_bytes(value, "little", signed=len(value) == 4))


def decode_page(data):
    lines = []
    position = data.find(b"att-") - 4
    while position + 4 <= len(data):
        (length,) = struct.unpack_from("<I", data, position)
        position += 4
        if position + length > len(data):
            lines.append(f"!! unexpected record length {length} at offset {position}")
            break
        record = data[position:position + length]
        position += length
        text = record.decode("latin1")

        if re.fullmatch(r"att-\d+", text):
            lines.append("")
            lines.append("OBJECT")
        elif re.fullmatch(r"codes\w+-\d+", text):
            lines.append(f"  [{text[5:text.rindex('-')]} event]")
        elif length >= 16 and re.match(rb"[a-z_0-9]+\0+$", record[:16]):
            name = record[:16].split(b"\0")[0].decode()
            lines.append(f"  {name}={decode_value(name, record[16:])}")
        elif record.strip():
            lines.append("    | " + record.decode("utf8", "replace"))
    return "\n".join(lines) + "\n"


def page_name(data):
    return data[0x18:0x28].split(b"\0")[0].decode("latin1")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    source, target = sys.argv[1], sys.argv[2]
    os.makedirs(target, exist_ok=True)

    entries = read_container(open(source, "rb").read())

    if "Program.s" in entries:
        with open(os.path.join(target, "Program.s.txt"), "wb") as f:
            f.write(entries["Program.s"])

    for page_id, file_number in enumerate(page_order(entries["main.HMI"])):
        data = entries[f"{file_number}.pa"]
        name = page_name(data)
        with open(os.path.join(target, f"{page_id:02d}_{name}.txt"), "w") as f:
            f.write(f"PAGE id={page_id} name={name}\n")
            f.write(decode_page(data))
        print(f"{page_id:2d} {name}")


if __name__ == "__main__":
    main()
