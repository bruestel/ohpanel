"""
Compress www/index.html and www/layout-<variant>.json (the preview's page layouts of the
environment's panel variant, from boards/nspanel/nextion/tools/hmi2layout.py) and emit
them as C arrays for the firmware to serve (tools/ui_asset.py does the work).

Why this exists as a PlatformIO pre-build hook rather than a CMake rule:

  * EMBED_FILES generates an assembly stub through a CMake custom command.
    PlatformIO drives the ESP-IDF build through its own layer that never runs
    those commands, so the build then fails on a file nobody produced.
  * Generating at CMake *configure* time works, but PlatformIO does not re-run
    configure when an unrelated source file changes. CMAKE_CONFIGURE_DEPENDS did
    not help. The result was worse than a build error: every build succeeded
    while the firmware kept serving a stale page.

A pre-build hook runs on every build, so the embedded copy cannot drift from the
source again.
"""

import pathlib
import sys

Import("env")  # noqa: F821  (injected by PlatformIO)

sys.path.insert(0, str(pathlib.Path(env.subst("$PROJECT_DIR")) / "tools"))  # noqa: F821
import ui_asset  # noqa: E402
import variant  # noqa: E402

ui_asset.generate(layout=variant.layout(variant.current(env)))  # noqa: F821
