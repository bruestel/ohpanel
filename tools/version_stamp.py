"""
Keep the firmware's version current on every build.

CMakeLists.txt works the version out (git describe) when CMake configures, but
PlatformIO configures again only when CMakeLists.txt or the sdkconfig changed:
build after build after a commit, the image still called itself by the commit
the build directory was first configured at (and so did its build date).

This pre-build hook works the same version out on every build and, when it
differs from the one the last configure stamped, touches CMakeLists.txt -- its
content stays as it is -- so PlatformIO configures again and the descriptor
gets the new version. One slower build per new version; none otherwise.
"""

import pathlib
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO)

ROOT = pathlib.Path(env.subst("$PROJECT_DIR"))  # noqa: F821
BUILD = pathlib.Path(env.subst("$BUILD_DIR"))  # noqa: F821
STAMP = BUILD / "ohpanel-version.txt"


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False).stdout.strip()
    except OSError:
        return ""


# as CMakeLists.txt does it: refresh the index first, so a touched but unchanged
# file does not count as a change
git("update-index", "-q", "--refresh")
version = git("describe", "--tags", "--always", "--dirty")
if version:
    previous = STAMP.read_text().strip() if STAMP.exists() else ""
    if version != previous:
        BUILD.mkdir(parents=True, exist_ok=True)
        STAMP.write_text(version + "\n")
        (ROOT / "CMakeLists.txt").touch()   # also the first time: what was configured is unknown
        print(f"Firmware version {previous or '?'} -> {version}: configuring again")
