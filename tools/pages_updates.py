#!/usr/bin/env python3
"""
The releases' files for the Pages site (.github/workflows/pages.yml), which the browser can fetch from another
origin (a release's own files it cannot): per channel -- "stable", the latest release; "test", the latest test
build (a prerelease) when it is newer -- each panel variant's factory image, firmware and display design, and
update.json, what the web interface's update and the browser flasher read:

    {"version": "0.11.0", "tag": "v0.11.0", "url": <the release's page>,
     "variants": {"eu": {"firmware": "nspanel-eu-firmware.bin", "firmware_size": n, "firmware_sha256": "...",
                         "factory": "nspanel-eu-factory.bin", "display": "nspanel-eu-display.tft",
                         "display_size": n, "display_sha256": "...", "design": "8072d6ec"}, ...}}

and per channel and variant a manifest for the flasher (flash/manifest-<variant>[-test].json; manifest.json the
EU's stable one, as before there were variants). Also the licences of what the files contain (licences/).

    tools/pages_updates.py SITE       (needs gh, logged in or GH_TOKEN; GITHUB_REPOSITORY, else bruestel/ohpanel)
"""

import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.environ.get("GITHUB_REPOSITORY", "bruestel/ohpanel")


def releases() -> list[dict]:
    out = subprocess.run(["gh", "release", "list", "--repo", REPO, "--limit", "30", "--json",
                          "tagName,isPrerelease,isDraft,publishedAt"], capture_output=True, text=True, check=True)
    return [r for r in json.loads(out.stdout) if not r["isDraft"]]


def version_key(tag: str) -> tuple:
    """v0.11.0-rc1 before v0.11.0: a test build comes before its release."""
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$", tag)
    if not m:
        return (-1,)
    major, minor, patch, rc = m.groups()
    return (int(major), int(minor), int(patch), int(rc) if rc else 1_000_000)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def channel(tag: str, out: pathlib.Path, licences: pathlib.Path) -> dict:
    """A release's files into `out`; its update.json's content."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["gh", "release", "download", tag, "--repo", REPO, "--pattern", "ohpanel-*.zip", "--dir", tmp],
                       check=True)
        for z in pathlib.Path(tmp).glob("ohpanel-*.zip"):   # a zip per panel variant (one for all before 0.11.0)
            zipfile.ZipFile(z).extractall(tmp)
        files = [p for p in pathlib.Path(tmp).glob("ohpanel-*/*") if p.is_file()]
        for p in pathlib.Path(tmp).glob("ohpanel-*/licences/*"):
            shutil.copy(p, licences / p.name)
        out.mkdir(parents=True, exist_ok=True)
        version = tag.removeprefix("v")
        variants = {}
        for f in files:
            m = re.match(r"ohpanel-(nspanel-([a-z]+))-(firmware|factory)-.*\.bin$|ohpanel-(nspanel-([a-z]+))-display-"
                         r"([0-9a-f]{8})\.tft$", f.name)
            if not m:
                continue
            if m.group(1):
                board, variant, kind = m.group(1), m.group(2), m.group(3)
                name = f"{board}-{kind}.bin"
            else:
                board, variant, kind = m.group(4), m.group(5), "display"
                name = f"{board}-display.tft"
                variants.setdefault(variant, {})["design"] = m.group(6)
            shutil.copy(f, out / name)
            v = variants.setdefault(variant, {})
            v[kind] = name
            if kind != "factory":
                v[kind + "_size"] = f.stat().st_size
                v[kind + "_sha256"] = sha256(f)
    return {"version": version, "tag": tag, "url": f"https://github.com/{REPO}/releases/tag/{tag}",
            "variants": dict(sorted(variants.items()))}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    site = pathlib.Path(sys.argv[1])
    licences = site / "licences"
    licences.mkdir(parents=True, exist_ok=True)
    flash = site / "flash"
    flash.mkdir(parents=True, exist_ok=True)
    all_ = releases()
    stable = max((r["tagName"] for r in all_ if not r["isPrerelease"]), key=version_key, default=None)
    test = max((r["tagName"] for r in all_ if r["isPrerelease"]), key=version_key, default=None)
    if test and stable and version_key(test) < version_key(stable):
        test = None   # a test build older than the release: nothing to test
    for name, tag in (("stable", stable), ("test", test)):
        if not tag:
            print(f"{name}: none")
            continue
        info = channel(tag, site / "update" / name, licences)
        (site / "update" / name / "update.json").write_text(json.dumps(info, indent=1) + "\n")
        for variant, v in info["variants"].items():
            if "factory" not in v:
                continue
            manifest = {"name": "OHpanel", "version": info["version"], "new_install_prompt_erase": True,
                        "builds": [{"chipFamily": "ESP32",
                                    "parts": [{"path": f"../update/{name}/{v['factory']}", "offset": 0}]}]}
            suffix = "" if name == "stable" else "-test"
            (flash / f"manifest-{variant}{suffix}.json").write_text(json.dumps(manifest) + "\n")
        print(f"{name}: {tag}, {', '.join(info['variants'])}")
    if (flash / "manifest-eu.json").exists():
        shutil.copy(flash / "manifest-eu.json", flash / "manifest.json")


if __name__ == "__main__":
    main()
