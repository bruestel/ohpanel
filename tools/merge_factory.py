"""
One image for a first, serial flash: the bootloader, the partition table, the
OTA data and the firmware (the icon set embedded), merged at their offsets:

    .pio/build/nspanel/ohpanel-nspanel-eu-factory-<version>.bin     written at 0x0

    esptool --chip esp32 write_flash 0x0 ohpanel-nspanel-eu-factory-<version>.bin

and the firmware alone, for an update over the web interface, under its release
name: .pio/build/nspanel/ohpanel-nspanel-eu-firmware-<version>.bin. The board is
the environment's custom_board (platformio.ini), the version the firmware's
(version_stamp.py).

The gaps between the parts are filled with 0xFF: writing it erases the nvs
partition (WiFi, passwords, openHAB) too -- right for a first flash, not for a
panel that keeps its settings; `pio run -t upload` writes the parts one by
one. See docs/flashing.md.

Runs as a PlatformIO post hook (after firmware.bin is built); the parts come
from FLASH_EXTRA_IMAGES, which the platform fills.
"""

import pathlib
import shutil
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO)


def merge(target, source, env):
    build = pathlib.Path(env.subst("$BUILD_DIR"))
    app = pathlib.Path(str(target[0]))
    parts = [(env.subst(str(off)), env.subst(str(path))) for off, path in env.get("FLASH_EXTRA_IMAGES", [])]
    parts.append((env.subst("$ESP32_APP_OFFSET") or "0x10000", str(app)))
    parts = [(hex(int(off, 0)), path) for off, path in parts]
    parts.sort(key=lambda p: int(p[0], 16))
    names = {pathlib.Path(p).name for _, p in parts}
    for needed in ("bootloader.bin", "partitions.bin"):
        if needed not in names:
            print(f"merge_factory: {needed} is missing, no factory image")
            return
    board = env.GetProjectOption("custom_board", "nspanel-eu")
    stamp = build / "ohpanel-version.txt"   # version_stamp.py
    version = stamp.read_text().strip() if stamp.exists() else "unknown"
    if version[:1] == "v" and version[1:2].isdigit():
        version = version[1:]   # a tag v1.2.0: ohpanel-...-1.2.0.bin
    for old in [*build.glob(f"ohpanel-{board}-*.bin"), build / "ohpanel-factory.bin"]:
        old.unlink(missing_ok=True)   # one of each: the current version's
    shutil.copyfile(app, build / f"ohpanel-{board}-firmware-{version}.bin")
    out = build / f"ohpanel-{board}-factory-{version}.bin"
    cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--chip", "esp32", "merge_bin", "-o", str(out),
           "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"]
    for off, path in parts:
        cmd += [off, path]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("merge_factory: esptool failed:\n" + result.stdout + result.stderr)
        return
    print(f"Factory image: {out} ({out.stat().st_size // 1024} KB: "
          + ", ".join(f"{pathlib.Path(p).name}@{o}" for o, p in parts) + ")")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge)  # noqa: F821
