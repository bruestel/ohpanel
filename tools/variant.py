"""
The panel variant a firmware is built for, and the files that differ by it:

    eu    the NSPanel EU (landscape, its frame hiding the rightmost 30 px), PlatformIO environment nspanel
    us    the NSPanel US (portrait), environment nspanel-us

A build hook takes it from its PlatformIO environment (custom_variant, platformio.ini), everything else from
OHPANEL_VARIANT (eu when unset) -- as boards/nspanel/nextion/build.sh hands it to the design's scripts
(boards/nspanel/nextion/layout.py).

Each variant has its own display design (display-manifest-<variant>.json, built by
boards/nspanel/nextion/build.sh), its own preview layout for the web interface (www/layout-<variant>.json) and its
own release files: ohpanel-nspanel-<variant>-{firmware,factory}-<version>.bin and
ohpanel-nspanel-<variant>-display-<design>.tft.
"""

import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
VARIANTS = ("eu", "us")
# the PlatformIO environment of each (platformio.ini)
ENVIRONMENTS = {"eu": "nspanel", "us": "nspanel-us"}


def current(env=None) -> str:
    """The variant of the PlatformIO environment `env`, or of OHPANEL_VARIANT."""
    if env is not None:
        name = env.GetProjectOption("custom_variant", "eu")
    else:
        name = os.environ.get("OHPANEL_VARIANT", "eu")
    if name not in VARIANTS:
        raise SystemExit(f"no panel variant {name!r} ({', '.join(VARIANTS)})")
    return name


def board(name: str) -> str:
    """The board in the release files' names."""
    return f"nspanel-{name}"


def manifest(name: str) -> pathlib.Path:
    return ROOT / "boards" / "nspanel" / "nextion" / f"display-manifest-{name}.json"


def layout(name: str) -> pathlib.Path:
    return ROOT / "www" / f"layout-{name}.json"


def build_dir(name: str) -> pathlib.Path:
    return ROOT / ".pio" / "build" / ENVIRONMENTS[name]


def designed() -> list[str]:
    """The variants whose display design is built (its manifest committed): the ones CI builds and a release has."""
    return [name for name in VARIANTS if manifest(name).exists()]


if __name__ == "__main__":
    # the PlatformIO environments to build, for the workflows: pio run $(python3 tools/variant.py)
    print(" ".join(f"-e {ENVIRONMENTS[name]}" for name in designed()))
