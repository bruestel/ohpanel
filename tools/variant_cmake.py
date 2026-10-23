"""
Hands the PlatformIO environment's panel variant (custom_variant, platformio.ini; tools/variant.py) to CMake as
OHPANEL_VARIANT, so platformio.ini names it once: CMakeLists.txt names the project by it, and the OTA handler
refuses an image of another project (components/app_web/src/WebSystem.cpp) -- an NSPanel US takes no firmware
for the EU and the other way round.

A pre-build hook: the ESP-IDF builder reads build.cmake_extra_args when it configures, after the pre hooks ran.
"""

import pathlib
import sys

Import("env")  # noqa: F821  (injected by PlatformIO)
sys.path.insert(0, str(pathlib.Path(env.subst("$PROJECT_DIR")) / "tools"))  # noqa: F821
import variant  # noqa: E402

board = env.BoardConfig()  # noqa: F821
args = board.get("build.cmake_extra_args", "")
if "OHPANEL_VARIANT" in args:
    sys.exit("variant_cmake: set the variant as custom_variant in platformio.ini, not in cmake_extra_args")
board.update("build.cmake_extra_args", f"{args} -DOHPANEL_VARIANT={variant.current(env)}".strip())  # noqa: F821
