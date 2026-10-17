#!/usr/bin/env bash
# Builds OHpanel's display firmware from an empty project (boards/nspanel/nextion/seed):
#
#   1. generate the pictures             boards/nspanel/nextion/assets/generate.py, boards/nspanel/nextion/assets/compose.py
#   2. import them into the seed         boards/nspanel/nextion/tools/import-pictures.sh (drives the Nextion Editor)
#   3. build the design as code          boards/nspanel/nextion/ohpanel_hmi.py (with the fonts, boards/nspanel/nextion/fonts.py)
#   4. compile                           boards/nspanel/nextion/tools/compile-hmi.sh (drives the Nextion Editor)
#   5. refresh the web preview           boards/nspanel/nextion/tools/hmi2layout.py -> www/layout.json
#
# Steps 3 and 5 also rewrite boards/nspanel/nextion/display-manifest.json and www/layout.json, which are committed:
# the firmware is built against the design they describe.
#
# Output: build/ohpanel.HMI (the design, openable in the Editor) and build/ohpanel-nspanel-eu-display-<version>.tft
# (for the panel; the version is the design's, which the display reports and the firmware expects -- the web
# interface names the file it wants by it), build/ohpanel.tft a link to it, and a copy in boards/nspanel/nextion/display/,
# committed: a release takes it from there (GitHub cannot run the Editor).
# Steps 2 and 4 need the Nextion Editor under wine and take a few minutes each.

set -euo pipefail
cd "$(dirname "$(realpath "$0")")/../../.."   # the repository: boards/nspanel/nextion/build.sh

mkdir -p build
gunzip -c boards/nspanel/nextion/seed/seed.HMI.gz > build/seed.HMI

echo "== generating pictures"
python3 boards/nspanel/nextion/assets/generate.py

echo "== icons and tiles onto every background"
python3 boards/nspanel/nextion/assets/compose.py

echo "== importing pictures"
TIMEOUT=900 boards/nspanel/nextion/tools/import-pictures.sh build/seed.HMI build/stage-pictures.HMI boards/nspanel/nextion/assets/backgrounds \
    boards/nspanel/nextion/assets/composites

echo "== building the design"
python3 boards/nspanel/nextion/ohpanel_hmi.py build/stage-pictures.HMI build/ohpanel.HMI

echo "== compiling"
VERSION=$(python3 -c 'import json; print(json.load(open("boards/nspanel/nextion/display-manifest.json"))["design"])')
TFT=ohpanel-nspanel-eu-display-$VERSION.tft
rm -f build/ohpanel-nspanel-eu-display-*.tft build/ohpanel-display-firmware-*.tft build/ohpanel.tft
boards/nspanel/nextion/tools/compile-hmi.sh build/ohpanel.HMI "build/$TFT"
ln -s "$TFT" build/ohpanel.tft
echo "build/$TFT"
# the one the repository keeps for releases (tools/make_release.py, .github/workflows/release.yml): the new design, the
# old one gone -- commit it with the manifest
rm -f boards/nspanel/nextion/display/ohpanel-nspanel-eu-display-*.tft
mkdir -p boards/nspanel/nextion/display
cp "build/$TFT" boards/nspanel/nextion/display/

echo "== weather codes doc"
python3 tools/gen_weather_doc.py

echo "== web preview layout"
python3 boards/nspanel/nextion/tools/hmi2layout.py build/ohpanel.HMI www/layout.json
