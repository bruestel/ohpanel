"""Edit a Nextion Editor project (.HMI) from Python.

A thin layer over the container format in nhw/: load a project, change
attributes and event code of existing objects, save it the way the Editor
itself does (append-only, see nhw/README.md). The Editor then only compiles.

    from hmipatch import Project
    p = Project("in.HMI")
    home = p.page("home")
    home.obj("date").set("w", 200)
    home.obj("swipe").event("codesup", ["printh 23 02 54 01"])
    home.clone("time", "clock2").set("y", 10)
    p.save("out.HMI")

Only what the format reference confirms is touched: attribute values keep
their width (strings their encoding), event blocks keep their marker name.
"""

from __future__ import annotations

import copy
import gzip
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nhw.nextion_hmi_checksum import with_main_checksum  # noqa: E402
from nhw.nextion_hmi_binary import (  # noqa: E402
    NAME_FIELD_SIZE,
    PAGE_NAME_OFFSET,
    CodeLine,
    Component,
    Marker,
    Page,
    Section,
    parse_container,
    parse_page,
)

# Event code and texts are stored in the project's encoding; this project uses
# iso-8859-1 (see the Editor's status bar).
TEXT_ENCODING = "latin-1"


class PatchError(RuntimeError):
    pass


class Obj:
    """An object (component, or the page itself) on a page."""

    def __init__(self, page: "PageEdit", component: Component):
        self._page = page
        self.c = component

    @property
    def name(self) -> str:
        return self.c.objname

    def get(self, attr: str):
        a = self.c.require(attr)
        if attr in ("objname", "txt", "path", "dir", "filter", "format"):
            return a.value.decode(TEXT_ENCODING)
        return a.as_int()

    def set(self, attr: str, value) -> "Obj":
        """Integers keep the attribute's width; strings (and raw bytes, e.g.
        UTF-8 icon glyphs for an icon font) replace the value."""
        a = self.c.attribute(attr)
        if a is None:
            raise PatchError(f"{self._page.name}.{self.name}: no attribute {attr!r}")
        if isinstance(value, bytes):
            a.value = value
        elif isinstance(value, str):
            a.value = value.encode(TEXT_ENCODING)
        else:
            if value < 0 or value >= 1 << (8 * len(a.value)):
                raise PatchError(f"{self._page.name}.{self.name}.{attr}: {value} does not fit")
            a.set_int(value)
        # position and size come with derived end coordinates
        if attr in ("x", "w") and self.c.attribute("endx") is not None:
            self.c.require("endx").set_int(self.get("x") + self.get("w") - 1)
        if attr in ("y", "h") and self.c.attribute("endy") is not None:
            self.c.require("endy").set_int(self.get("y") + self.get("h") - 1)
        self._page.dirty = True
        return self

    def code(self, marker: str) -> list[str]:
        """The code lines of an event block, e.g. marker "codesup"."""
        records = self.c.records
        for i, r in enumerate(records):
            if isinstance(r, Marker):
                head, count = r.split_count()
                if head == marker and count is not None:
                    return [x.raw.decode(TEXT_ENCODING) for x in records[i + 1 : i + 1 + count]]
        raise PatchError(f"{self._page.name}.{self.name}: no event {marker!r}")

    def events(self) -> list[str]:
        """The markers of its event blocks, e.g. ["codesdown", "codesup"]."""
        out = []
        for r in self.c.records:
            if isinstance(r, Marker):
                head, count = r.split_count()
                if count is not None and head.startswith("codes"):
                    out.append(head)
        return out

    def event(self, marker: str, lines: list[str]) -> "Obj":
        """Replaces the code of an event block."""
        records = self.c.records
        for i, r in enumerate(records):
            if isinstance(r, Marker):
                head, count = r.split_count()
                if head == marker and count is not None:
                    del records[i + 1 : i + 1 + count]
                    r.text = f"{marker}-{len(lines)}"
                    for k, line in enumerate(lines):
                        records.insert(i + 1 + k, CodeLine(raw=line.encode(TEXT_ENCODING)))
                    self._page.dirty = True
                    return self
        raise PatchError(f"{self._page.name}.{self.name}: no event {marker!r}")


class PageEdit:
    def __init__(self, section: Section, page_id: int):
        self.section = section
        self.page: Page = parse_page(section.data)
        self.page_id = page_id  # what "page <id>" on the display means
        self.dirty = False

    @property
    def name(self) -> str:
        return self.page.name

    def rename(self, new: str) -> None:
        """Renames the page: the name field of its header and its page object.
        References in code (other pages, Program.s) are not touched."""
        raw = new.encode("ascii")
        if len(raw) >= NAME_FIELD_SIZE:
            raise PatchError(f"page name too long: {new!r}")
        self.page.header[PAGE_NAME_OFFSET:PAGE_NAME_OFFSET + NAME_FIELD_SIZE] = raw.ljust(NAME_FIELD_SIZE, b"\0")
        self.page.components[0].require("objname").value = raw
        self.dirty = True

    def objects(self) -> list[Obj]:
        return [Obj(self, c) for c in self.page.components]

    def obj(self, name: str) -> Obj:
        for c in self.page.components:
            if c.objname == name:
                return Obj(self, c)
        raise PatchError(f"page {self.name}: no object {name!r}")

    def has(self, name: str) -> bool:
        return any(c.objname == name for c in self.page.components)

    def remove(self, names) -> None:
        """Removes objects; the ones after them move up (their ids change, as
        when the Editor deletes them). Code that still names one fails to
        compile in the Editor."""
        names = set(names)
        missing = names - {c.objname for c in self.page.components}
        if missing:
            raise PatchError(f"page {self.name}: no objects {sorted(missing)}")
        if self.page.components[0].objname in names:
            raise PatchError(f"page {self.name}: the page object cannot be removed")
        keep = [(c, f) for c, f in zip(self.page.components, self.page.content_flags) if c.objname not in names]
        self.page.components = [c for c, _ in keep]
        self.page.content_flags = [f for _, f in keep]
        for i, c in enumerate(self.page.components):
            c.require("id").set_int(i)
        self.dirty = True

    def add_copy(self, source: Obj, name: str) -> Obj:
        """Like clone(), but the source may be on another page (or in another
        project: the seed's objects, see Project.add_page())."""
        if self.has(name):
            raise PatchError(f"page {self.name}: {name!r} exists already")
        if len(name.encode("ascii")) > 14:
            raise PatchError(f"object name longer than 14 characters: {name!r}")
        component = copy.deepcopy(source.c)
        component.require("objname").value = name.encode("ascii")
        component.require("id").set_int(len(self.page.components))
        self.page.components.append(component)
        self.page.content_flags.append(0)
        self.dirty = True
        return Obj(self, component)

    def clone(self, source: str, name: str) -> Obj:
        """A copy of an existing object under a new name, appended to the page
        (so it is drawn on top and gets the next object id). Clone-and-patch is
        the only safe way to create objects: every attribute the Editor expects
        is already there."""
        if self.has(name):
            raise PatchError(f"page {self.name}: {name!r} exists already")
        if len(name.encode("ascii")) > 14:
            raise PatchError(f"object name longer than 14 characters: {name!r}")
        component = copy.deepcopy(self.obj(source).c)
        component.require("objname").value = name.encode("ascii")
        component.require("id").set_int(len(self.page.components))
        self.page.components.append(component)
        self.page.content_flags.append(0)
        self.dirty = True
        return Obj(self, component)


# main.HMI: a 96-byte head (checksum first, number of records at 28), then one
# 16-byte record per picture, font and page: its type ("i", "zi", "pa") and its
# section's file name, each zero-padded to 8 bytes. The order of the records
# of a type is the id the display uses ("page 3", pic=12, font=2).
MAIN_HEAD = 96
MAIN_RECORD = 16
MAIN_COUNT_AT = 28
# The display's direction in the head: 0, 1, 2, 3 for 0, 90, 180, 270 degrees
# (Setting > Display); the compiled .tft carries it on (byte 20 of its header).
MAIN_DIRECTION_AT = 12

# Attributes that hold a picture id (65535: none).
PICTURE_ATTRS = ("pic", "pic1", "pic2", "picc", "picc1", "picc2", "bpic", "ppic")


class Project:
    def __init__(self, path: str):
        raw = open(path, "rb").read()
        if raw[:2] == b"\x1f\x8b":   # gzipped (the seed is kept that way: the container is mostly zeros)
            raw = gzip.decompress(raw)
        self.hmi = parse_container(raw)
        main = self.hmi.live("main.HMI").data
        self._main_head = bytearray(main[:MAIN_HEAD])
        self._records = [main[i:i + MAIN_RECORD] for i in range(MAIN_HEAD, len(main), MAIN_RECORD)]
        self._main_changed = False
        self.pages: dict[str, PageEdit] = {}
        for section in self.hmi.pages():
            edit = PageEdit(section, 0)
            self.pages[edit.name] = edit
        self._number_pages()

    def _files(self, kind: str) -> list[str]:
        """File names of the records of a type, in id order."""
        return [r[8:].rstrip(b"\0").decode() for r in self._records if r[:8].rstrip(b"\0") == kind.encode()]

    def _number_pages(self) -> None:
        order = self._files("pa")
        for edit in self.pages.values():
            edit.page_id = order.index(edit.section.name)

    @property
    def direction(self) -> int:
        """The display's direction in degrees (0, 90, 180, 270)."""
        return self._main_head[MAIN_DIRECTION_AT] * 90

    @direction.setter
    def direction(self, degrees: int) -> None:
        if degrees not in (0, 90, 180, 270):
            raise PatchError(f"no display direction: {degrees}")
        self._main_head[MAIN_DIRECTION_AT] = degrees // 90
        self._main_changed = True

    def font_files(self) -> list[str]:
        """The .zi entries of the fonts, in font id order."""
        return self._files("zi")

    def replace_font(self, font: int, data: bytes) -> None:
        """Puts another .zi font in place of font number `font` (written on save)."""
        self._fonts = getattr(self, "_fonts", {})
        self._fonts[self.font_files()[font]] = data

    def _free_name(self, ext: str) -> str:
        """A section name the project has not used yet, "<n>.<ext>" as the Editor names them."""
        used = [int(sec.name.split(".")[0]) for sec in self.hmi.sections
                if sec.name.endswith("." + ext) and sec.name.split(".")[0].isdigit()]
        return f"{max(used, default=-1) + 1}.{ext}"

    def _append(self, name: str, data: bytes) -> Section:
        """A new section at the end of the container, as the Editor appends them."""
        section = Section(name_field=name.encode("ascii"), start=self.hmi.total_size, size=len(data), deleted=0,
                          reserved=b"\0\0\0", data=data)
        self.hmi.sections.append(section)
        self.hmi.gap = self.hmi.gap + bytes(len(data))
        self.hmi.total_size += len(data)
        return section

    def _add_record(self, kind: str, name: str) -> None:
        self._records.append(kind.encode().ljust(8, b"\0") + name.encode().ljust(8, b"\0"))
        self._main_changed = True

    def add_font(self, data: bytes) -> int:
        """Adds a .zi font (fonts.py); returns its font id."""
        name = self._free_name("zi")
        self._append(name, data)
        self._add_record("zi", name)
        return len(self.font_files()) - 1

    def add_page(self, name: str, like: "PageEdit") -> "PageEdit":
        """A new, empty page after the others: a copy of the page object of
        `like` (the seed's page, say) under a new name, nothing on it."""
        if name in self.pages:
            raise PatchError(f"page {name!r} exists already")
        page = copy.deepcopy(like.page)
        page.components, page.content_flags = page.components[:1], page.content_flags[:1]
        section = self._append(self._free_name("pa"), page.encode())
        self._add_record("pa", section.name)
        edit = PageEdit(section, 0)
        edit.rename(name)
        self.pages[name] = edit
        self._number_pages()
        return edit

    def picture_count(self) -> int:
        return len(self._files("i"))

    def _drop_section(self, name: str) -> None:
        section = self.hmi.live(name)
        if section is not None:
            section.deleted = 1
            section.name_field = b"\x00" + section.name_field[1:]

    def _drop_records(self, names: set[str]) -> None:
        self._records = [r for r in self._records if r[8:].rstrip(b"\0").decode() not in names]
        self._main_changed = True

    def remove_pages(self, names) -> None:
        """Removes pages; the pages after them move up (their ids change, which
        is why nothing may refer to a page by number -- check with
        page_number_refs())."""
        files = set()
        for name in names:
            edit = self.page(name)
            files.add(edit.section.name)
            self._drop_section(edit.section.name)
            del self.pages[name]
        self._drop_records(files)
        self._number_pages()

    def picture_refs(self) -> dict[int, list[str]]:
        """Picture id -> "page.object.attribute" of every reference."""
        refs: dict[int, list[str]] = {}
        for name, edit in self.pages.items():
            for obj in edit.objects():
                for attr in PICTURE_ATTRS:
                    if obj.c.attribute(attr) is not None and obj.get(attr) != 65535:
                        refs.setdefault(obj.get(attr), []).append(f"{name}.{obj.name}.{attr}")
        return refs

    def remove_pictures(self, ids) -> dict[int, int]:
        """Removes pictures and renumbers the references of the rest; returns
        old id -> new id of the pictures that stay. Fails if a page still
        refers to one that goes."""
        ids = set(ids)
        refs = self.picture_refs()
        still = sorted(i for i in ids if i in refs)
        if still:
            raise PatchError(f"pictures still referenced: {[(i, refs[i][0]) for i in still]}")
        files = self._files("i")
        mapping, n = {}, 0
        for old in range(len(files)):
            if old not in ids:
                mapping[old] = n
                n += 1
        for old in ids:
            self._drop_section(files[old])
            self._drop_section(files[old][:-2] + ".is")  # the editor's preview
        self._drop_records({files[i] for i in ids})
        for edit in self.pages.values():
            for obj in edit.objects():
                for attr in PICTURE_ATTRS:
                    if obj.c.attribute(attr) is not None and obj.get(attr) != 65535:
                        new = mapping[obj.get(attr)]
                        if new != obj.get(attr):
                            obj.set(attr, new)
        return mapping

    @property
    def program(self) -> str:
        """Program.s: the code the display runs once at power-on (global
        variables, start page) -- as changed so far."""
        pending = getattr(self, "_program", None)
        return (pending if pending is not None else self.hmi.live("Program.s").data).decode(TEXT_ENCODING)

    @program.setter
    def program(self, text: str) -> None:
        self._program = text.encode(TEXT_ENCODING)

    def rename_page(self, old: str, new: str) -> None:
        if new in self.pages:
            raise PatchError(f"page {new!r} exists already")
        edit = self.page(old)
        edit.rename(new)
        self.pages = {(new if k == old else k): v for k, v in self.pages.items()}

    def page(self, name: str) -> PageEdit:
        if name not in self.pages:
            raise PatchError(f"no page {name!r}")
        return self.pages[name]

    def _replace(self, old: Section, data: bytes) -> None:
        """The Editor's journal rule: tombstone the old record, append the new payload."""
        new = Section(name_field=old.name_field, start=self.hmi.total_size, size=len(data),
                      deleted=0, reserved=old.reserved, data=data)
        old.deleted = 1
        old.name_field = b"\x00" + old.name_field[1:]
        self.hmi.sections.append(new)
        self.hmi.gap = self.hmi.gap + bytes(len(data))
        self.hmi.total_size += len(data)

    def save(self, path: str) -> list[str]:
        """Writes the project; returns the names of the pages that changed."""
        changed = []
        new_program = getattr(self, "_program", None)
        if new_program is not None and new_program != self.hmi.live("Program.s").data:
            self._replace(self.hmi.live("Program.s"), new_program)
            changed.append("Program.s")
        for name, data in getattr(self, "_fonts", {}).items():
            if data != self.hmi.live(name).data:
                self._replace(self.hmi.live(name), data)
                changed.append(name)
        if self._main_changed:
            head = bytearray(self._main_head)
            head[MAIN_COUNT_AT:MAIN_COUNT_AT + 4] = len(self._records).to_bytes(4, "little")
            self._replace(self.hmi.live("main.HMI"), with_main_checksum(bytes(head) + b"".join(self._records)))
            changed.append("main.HMI")
        for name, edit in self.pages.items():
            if not edit.dirty:
                continue
            data = edit.page.encode()
            if data != edit.section.data:
                self._replace(edit.section, data)
                changed.append(name)
        with open(path, "wb") as f:
            f.write(self.hmi.encode())
        return changed
