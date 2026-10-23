#!/usr/bin/env python3
"""
A release's one download: dist/ohpanel-<version>.zip with the binaries of every panel variant that has a display
design (tools/variant.py) and the licences that have to go with every copy of them (the fonts' licences ask for
that; see THIRD-PARTY-NOTICES.md).

    pio run $(python3 tools/variant.py)       # the firmware images, a set per variant
    boards/nspanel/nextion/build.sh <variant> # the display design (Nextion Editor under wine), kept in
                                              # boards/nspanel/nextion/display/ -- committed: CI cannot build it
    tools/make_release.py                     # dist/ohpanel-<version>.zip

A tag v<version> does all but the display design on GitHub (.github/workflows/release.yml) and publishes the release.

The version is the firmware's (git describe, as the build stamps it); the display design is the one the variant's
firmware expects (display-manifest-<variant>.json). Refused when a part is missing or the tree has uncommitted changes
(the firmware would call itself <version>-dirty).
"""

import glob
import json
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import variant  # noqa: E402

IDF = pathlib.Path.home() / ".platformio" / "packages" / "framework-espidf"

# the licences of what the images contain: (path, name in the zip's licences/)
LICENCES = [
    (ROOT / "boards/nspanel/nextion/assets/fonts/UFL.txt", "Ubuntu-font-UFL.txt"),
    (ROOT / "boards/nspanel/nextion/assets/fonts/OFL.txt", "Noto-Sans-OFL.txt"),
    (ROOT / "boards/nspanel/nextion/assets/fonts/MDI-LICENSE.txt", "Material-Design-Icons-LICENSE.txt"),
    (ROOT / "boards/nspanel/nextion/assets/meteocons/LICENSE", "Meteocons-LICENSE.txt"),
    (ROOT / "components/qrcodegen/LICENSE", "qrcodegen-LICENSE.txt"),
    (ROOT / "managed_components/espressif__cjson/cJSON/LICENSE", "cJSON-LICENSE.txt"),
    (ROOT / "managed_components/espressif__mdns/LICENSE", "esp-mdns-LICENSE.txt"),
    (IDF / "LICENSE", "ESP-IDF-LICENSE.txt"),
    (IDF / "components/freertos/FreeRTOS-Kernel/LICENSE.md", "FreeRTOS-Kernel-LICENSE.md"),
    (IDF / "components/lwip/lwip/COPYING", "lwIP-COPYING.txt"),
    (IDF / "components/mbedtls/mbedtls/LICENSE", "MbedTLS-LICENSE.txt"),
]


def one(pattern):
    found = sorted(glob.glob(str(pattern)))
    if len(found) != 1:
        sys.exit(f"make_release: expected one {pattern}, found {len(found)}")
    return pathlib.Path(found[0])


# what the README says of each variant's files
PANELS = {"eu": "the Sonoff NSPanel EU", "us": "the Sonoff NSPanel US (portrait)"}


def files(name):
    """(version, firmware, factory image, display firmware) of a panel variant."""
    board = variant.board(name)
    firmware = one(variant.build_dir(name) / f"ohpanel-{board}-firmware-*.bin")
    version = firmware.stem.removeprefix(f"ohpanel-{board}-firmware-")
    if version.endswith("-dirty"):
        sys.exit(f"make_release: the firmware is {version}: commit first, then build again")
    factory = one(variant.build_dir(name) / f"ohpanel-{board}-factory-{version}.bin")
    design = json.loads(variant.manifest(name).read_text())["design"]
    tft = ROOT / "boards" / "nspanel" / "nextion" / "display" / f"ohpanel-{board}-display-{design}.tft"
    if not tft.exists():
        sys.exit(f"make_release: {tft.relative_to(ROOT)} is missing: run boards/nspanel/nextion/build.sh {name} and "
                 "commit it")
    return version, firmware, factory, tft


def main():
    names = variant.designed()
    if not names:
        sys.exit("make_release: no panel variant has a display design")
    sets = {name: files(name) for name in names}
    versions = {v for v, *_ in sets.values()}
    if len(versions) != 1:
        sys.exit(f"make_release: the variants' firmware differ in version ({', '.join(sorted(versions))}): build all "
                 "from one commit")
    version = versions.pop()
    missing = [str(p) for p, _ in LICENCES if not p.exists()]
    if missing:
        sys.exit("make_release: licences missing:\n  " + "\n  ".join(missing))
    out = ROOT / "dist" / f"ohpanel-{version}.zip"
    out.parent.mkdir(exist_ok=True)
    top = f"ohpanel-{version}"
    readme = f"OHpanel {version} for the Sonoff NSPanel -- https://github.com/bruestel/ohpanel\n"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, (_, firmware, factory, tft) in sets.items():
            for path in (firmware, factory, tft):
                z.write(path, f"{top}/{path.name}")
            readme += (f"\nFor {PANELS[name]} ({variant.board(name)}: only for it -- the other's firmware is refused):\n"
                       f"{factory.name}\n    the first installation, over the serial port, at offset 0\n"
                       f"    (docs/install.md; or in the browser: https://bruestel.github.io/ohpanel/flash/)\n"
                       f"{firmware.name}\n    an update over WiFi: Settings > Firmware > Firmware update\n"
                       f"{tft.name}\n    the display design: Settings > Firmware > Display firmware (about 15 minutes)\n")
        for path in (ROOT / "LICENSE", ROOT / "THIRD-PARTY-NOTICES.md"):
            z.write(path, f"{top}/{path.name}")
        for path, name in LICENCES:
            z.write(path, f"{top}/licences/{name}")
        z.writestr(f"{top}/README.txt", readme +
                   "\nLICENSE, THIRD-PARTY-NOTICES.md and licences/: what the images contain and under which licences.\n")
    print(f"release: {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
