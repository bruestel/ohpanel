#!/usr/bin/env bash
# Builds OHpanel's display firmware from an empty project (boards/nspanel/nextion/seed), for a panel variant:
#
#   boards/nspanel/nextion/build.sh [eu|us]      eu (the NSPanel EU) when not given
#
# The variant goes to every step as OHPANEL_VARIANT (boards/nspanel/nextion/layout.py lays it out):
#
#   1. generate the pictures             boards/nspanel/nextion/assets/generate.py, boards/nspanel/nextion/assets/compose.py
#   2. import them into the seed         boards/nspanel/nextion/tools/import-pictures.sh (drives the Nextion Editor)
#   3. build the design as code          boards/nspanel/nextion/ohpanel_hmi.py (with the fonts, boards/nspanel/nextion/fonts.py)
#   4. compile                           boards/nspanel/nextion/tools/compile-hmi.sh (drives the Nextion Editor)
#   5. refresh the web preview           boards/nspanel/nextion/tools/hmi2layout.py -> www/layout-<variant>.json
#
# Steps 3 and 5 also rewrite boards/nspanel/nextion/display-manifest-<variant>.json and www/layout-<variant>.json,
# which are committed: the variant's firmware is built against the design they describe.
#
# Output: build/ohpanel-<variant>.HMI (the design, openable in the Editor) and
# build/ohpanel-nspanel-<variant>-display-<version>.tft (for the panel; the version is the design's, which the
# display reports and the firmware expects -- the web interface names the file it wants by it), and a copy in
# boards/nspanel/nextion/display/, committed: a release takes it from there (GitHub cannot run the Editor).
# Steps 2 and 4 need the Nextion Editor under wine and take a few minutes each.

set -euo pipefail
cd "$(dirname "$(realpath "$0")")/../../.."   # the repository: boards/nspanel/nextion/build.sh

export OHPANEL_VARIANT=${1:-eu}
python3 -c 'import sys; sys.path.insert(0, "boards/nspanel/nextion"); import layout'   # a variant it knows
echo "== variant $OHPANEL_VARIANT"

mkdir -p build
gunzip -c boards/nspanel/nextion/seed/seed.HMI.gz > build/seed.HMI

echo "== generating pictures"
python3 boards/nspanel/nextion/assets/generate.py

echo "== icons and tiles onto every background"
python3 boards/nspanel/nextion/assets/compose.py

echo "== importing pictures"
TIMEOUT=900 boards/nspanel/nextion/tools/import-pictures.sh build/seed.HMI "build/stage-pictures-$OHPANEL_VARIANT.HMI" \
    boards/nspanel/nextion/assets/backgrounds \
    boards/nspanel/nextion/assets/composites

echo "== building the design"
HMI=build/ohpanel-$OHPANEL_VARIANT.HMI
python3 boards/nspanel/nextion/ohpanel_hmi.py "build/stage-pictures-$OHPANEL_VARIANT.HMI" "$HMI"

echo "== compiling"
VERSION=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["design"])' \
    "boards/nspanel/nextion/display-manifest-$OHPANEL_VARIANT.json")
TFT=ohpanel-nspanel-$OHPANEL_VARIANT-display-$VERSION.tft
rm -f "build/ohpanel-nspanel-$OHPANEL_VARIANT-display-"*.tft
boards/nspanel/nextion/tools/compile-hmi.sh "$HMI" "build/$TFT"
echo "build/$TFT"
# the one the repository keeps for releases (tools/make_release.py, .github/workflows/release.yml): the new design, the
# old one gone -- commit it with the manifest
rm -f "boards/nspanel/nextion/display/ohpanel-nspanel-$OHPANEL_VARIANT-display-"*.tft
mkdir -p boards/nspanel/nextion/display
cp "build/$TFT" boards/nspanel/nextion/display/

echo "== weather codes doc"
python3 tools/gen_weather_doc.py

echo "== web preview layout"
python3 boards/nspanel/nextion/tools/hmi2layout.py "$HMI" "www/layout-$OHPANEL_VARIANT.json"
