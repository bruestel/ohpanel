"""Where OHpanel's own layout puts things on the display -- shared by the design
recipe (boards/nspanel/nextion/ohpanel_hmi.py), the picture generator and composer
(boards/nspanel/nextion/assets/generate.py, compose.py) and the web preview's
extractor (boards/nspanel/nextion/tools/hmi2layout.py), which must agree on every position.

The panel variant it is for comes from OHPANEL_VARIANT (boards/nspanel/nextion/build.sh):
"eu", the NSPanel EU (landscape, layout_eu.py), the default, "us", the NSPanel US
(portrait, layout_us.py), or "us-landscape", the NSPanel US on its side, its buttons on the right
(layout_us_landscape.py). All have the same names; this one is one of them.
"""

import os
import sys

VARIANTS = ("eu", "us", "us-landscape")
VARIANT = os.environ.get("OHPANEL_VARIANT", "eu")
if VARIANT not in VARIANTS:
    sys.exit(f"layout: no panel variant {VARIANT!r} (OHPANEL_VARIANT: {', '.join(VARIANTS)})")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if VARIANT == "eu":
    from layout_eu import *  # noqa: E402,F401,F403
elif VARIANT == "us":
    from layout_us import *  # noqa: E402,F401,F403
else:
    from layout_us_landscape import *  # noqa: E402,F401,F403

# The variant's pictures that depend on its layout (generate.py writes them: backgrounds at the display's size,
# tiles, controls, the logo); the weather icons are the same for all.
ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", VARIANT)
