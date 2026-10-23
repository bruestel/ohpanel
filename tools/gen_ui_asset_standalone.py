#!/usr/bin/env python3
"""
The same file as gen_ui_asset.py (both use tools/ui_asset.py), callable without PlatformIO.

Used by the CMake fallback for a first configure, and handy for plain idf.py
builds. Usage: gen_ui_asset_standalone.py <index.html> <layout-<variant>.json> <ui_asset.c>
"""

import pathlib
import sys

if len(sys.argv) != 4:
    sys.exit(__doc__)

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ui_asset  # noqa: E402

ui_asset.generate(*sys.argv[1:4])
