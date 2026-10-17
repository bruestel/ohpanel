#!/usr/bin/env bash
# Makes the seed of OHpanel's display design (boards/nspanel/nextion/seed/seed.HMI.gz) by driving the Nextion
# Editor (wine) on an invisible Xvfb display: a new project for the NX4832F035 (Discovery series), turned to
# 270 degrees as the NSPanel mounts it, iso-8859-1, with one page holding one object of each type the design
# uses -- text, number, button, progress bar, picture, hotspot, touch area, slider, timer, variable -- as the Editor makes
# them. The recipe (boards/nspanel/nextion/ohpanel_hmi.py) copies these for every object it builds.
#
# Usage: make-seed.sh [output.HMI.gz]     (default: boards/nspanel/nextion/seed/seed.HMI.gz)
#
# Editor automation: see editor-lib.sh.

set -euo pipefail

TOOL=make-seed
HERE="$(dirname "$(realpath "$0")")"
TARGET="$(realpath -m "${1:-$HERE/../seed/seed.HMI.gz}")"

. "$HERE/editor-lib.sh"

editor_start
log "New project"
click 90 63 3                                   # New
click 540 457 0.5                               # file name field of the save dialog
type_text "$(winpath "$WORK/seed.HMI")"
xdotool key Return
sleep 3
click 452 176 2                                 # Discovery series
click 350 358 1                                 # NX4832F035_011
click 101 117 2                                 # Display
click 828 200 1                                 # 270 degrees, horizontal (encoding: iso-8859-1, the default)
click 769 687 3                                 # OK

log "Adding the objects"
for y in 138 162 186 234 258 282 330 354; do    # text, scrolling text, number, button, progress bar, picture, hotspot,
                                                # touch area
    click 45 "$y" 2
done
for _ in 1 2 3 4 5 6; do click 201 406 0.3; done   # scroll the toolbox down
sleep 1
for y in 282 306 330; do                        # slider, timer, variable
    click 45 "$y" 2
done
click 138 63 3                                  # Save

[ -s "$WORK/seed.HMI" ] || fail "the editor saved no project"
HMITOOLS="$HERE" python3 -c '
import os, sys
sys.path.insert(0, os.environ["HMITOOLS"])
from hmipatch import Project
p = Project(sys.argv[1])
types = sorted(o.get("type") for o in p.page("page0").objects()[1:])
want = sorted([116, 55, 54, 98, 106, 112, 109, 5, 1, 51, 52])
if list(p.pages) != ["page0"] or types != want or p.direction != 270:
    sys.exit(f"unexpected seed: pages {list(p.pages)}, object types {types}, direction {p.direction}")
# written again: both copies of the section directory alike (the Editor left the second one pointing to the
# project head from before the settings changed, and reads that one)
p.direction = 270
p.save(sys.argv[1])
' "$WORK/seed.HMI"
mkdir -p "$(dirname "$TARGET")"
gzip -9nc "$WORK/seed.HMI" > "$TARGET"
log "Done: $TARGET"
