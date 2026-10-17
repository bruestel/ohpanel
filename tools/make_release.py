#!/usr/bin/env python3
"""
A release's one download: dist/ohpanel-<version>.zip with the binaries and the licences that have to go
with every copy of them (the fonts' licences ask for that; see THIRD-PARTY-NOTICES.md).

    pio run -e nspanel                    # the firmware images
    boards/nspanel/nextion/build.sh       # the display design (Nextion Editor under wine), kept in
                                          # boards/nspanel/nextion/display/ -- committed: CI cannot build it
    tools/make_release.py                 # dist/ohpanel-<version>.zip

A tag v<version> does all but the display design on GitHub (.github/workflows/release.yml) and publishes the release.

The version is the firmware's (git describe, as the build stamps it); the display design is the one this
firmware expects (display-manifest.json). Refused when a part is missing or the tree has uncommitted changes
(the firmware would call itself <version>-dirty).
"""

import glob
import json
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "build" / "nspanel"
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


def main():
    firmware = one(BUILD / "ohpanel-nspanel-eu-firmware-*.bin")
    version = firmware.stem.removeprefix("ohpanel-nspanel-eu-firmware-")
    if version.endswith("-dirty"):
        sys.exit(f"make_release: the firmware is {version}: commit first, then build again")
    factory = one(BUILD / f"ohpanel-nspanel-eu-factory-{version}.bin")
    design = json.loads((ROOT / "boards/nspanel/nextion/display-manifest.json").read_text())["design"]
    tft = ROOT / "boards" / "nspanel" / "nextion" / "display" / f"ohpanel-nspanel-eu-display-{design}.tft"
    if not tft.exists():
        sys.exit(f"make_release: {tft.relative_to(ROOT)} is missing: run boards/nspanel/nextion/build.sh and commit it")
    missing = [str(p) for p, _ in LICENCES if not p.exists()]
    if missing:
        sys.exit("make_release: licences missing:\n  " + "\n  ".join(missing))
    out = ROOT / "dist" / f"ohpanel-{version}.zip"
    out.parent.mkdir(exist_ok=True)
    top = f"ohpanel-{version}"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in (firmware, factory, tft, ROOT / "LICENSE", ROOT / "THIRD-PARTY-NOTICES.md"):
            z.write(path, f"{top}/{path.name}")
        for path, name in LICENCES:
            z.write(path, f"{top}/licences/{name}")
        z.writestr(f"{top}/README.txt",
                   f"OHpanel {version} for the Sonoff NSPanel EU -- https://github.com/bruestel/ohpanel\n\n"
                   f"{factory.name}\n    the first installation, over the serial port, at offset 0\n"
                   f"    (docs/install.md; or in the browser: https://bruestel.github.io/ohpanel/flash/)\n"
                   f"{firmware.name}\n    an update over WiFi: Settings > Firmware > Firmware update\n"
                   f"{tft.name}\n    the display design: Settings > Firmware > Display firmware (about 15 minutes)\n\n"
                   "LICENSE, THIRD-PARTY-NOTICES.md and licences/: what the images contain and under which licences.\n")
    print(f"release: {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
