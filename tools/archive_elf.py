"""
Keep the firmware.elf of every build, named after the firmware version and
the start of the ELF's SHA-256:

    build/elf/<version>-<sha>.elf      e.g. build/elf/v0.3-12-g5a97b4f-3f9c2a1b7.elf

A core dump can only be decoded with the .elf of exactly the firmware that
crashed, and PlatformIO overwrites .pio/build/nspanel/firmware.elf with every
build. The dump records that ELF's hash (the device shows it as
crash.elf_sha256 in the diagnostics), so the file to use is the one whose name
ends in that hash. See docs/crash-analysis.md.

Runs as a PlatformIO post hook (after firmware.elf is linked).
"""

import hashlib
import pathlib
import shutil
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO)

ROOT = pathlib.Path(env.subst("$PROJECT_DIR"))  # noqa: F821
ARCHIVE = ROOT / "build" / "elf"
# CONFIG_APP_RETRIEVE_LEN_ELF_SHA (9): as many hex digits as the app descriptor,
# and therefore the core dump and the crash line in the web interface, show
SHA_DIGITS = 9


def version():
    """The version CMakeLists.txt stamps into the firmware, worked out the same way."""
    def git(*args):
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()

    git("update-index", "-q", "--refresh")
    return git("describe", "--tags", "--always", "--dirty") or "unknown"


def archive(target, source, env):
    elf = pathlib.Path(str(target[0]))
    sha = hashlib.sha256(elf.read_bytes()).hexdigest()[:SHA_DIGITS]
    ARCHIVE.mkdir(parents=True, exist_ok=True)
    out = ARCHIVE / f"{version()}-{sha}.elf"
    if not out.exists():
        shutil.copyfile(elf, out)
    print(f"ELF archived: {out.relative_to(ROOT)}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", archive)  # noqa: F821
