#!/usr/bin/env bash
# Appends pictures to a Nextion .HMI by driving the Nextion Editor (the .HMI picture format is not
# documented well enough to write it directly). New pictures get the next free ids, in file-name order,
# so existing ids -- which pages refer to -- never move.
#
# Usage: import-pictures.sh <in.HMI> <out.HMI> <folder with .png files>...
#
# Editor automation: see editor-lib.sh.

set -euo pipefail

TOOL=import-pictures
if [ $# -lt 3 ]; then
    echo "Usage: $0 <in.HMI> <out.HMI> <folder>..." >&2
    exit 1
fi
SOURCE="$(realpath "$1")"
TARGET="$(realpath -m "$2")"
shift 2
FOLDERS=()
for f in "$@"; do FOLDERS+=("$(realpath "$f")"); done
[ -f "$SOURCE" ] || { echo "File not found: $SOURCE" >&2; exit 1; }
COUNT=0
for f in "${FOLDERS[@]}"; do COUNT=$((COUNT + $(find "$f" -maxdepth 1 -name '*.png' | wc -l))); done
[ "$COUNT" -gt 0 ] || { echo "No .png files in ${FOLDERS[*]}" >&2; exit 1; }
mkdir -p "$(dirname "$TARGET")"

. "$(dirname "$(realpath "$0")")/editor-lib.sh"

# The file dialog only shows the folder's files, so copy just the pictures into a clean folder.
mkdir "$WORK/pictures"
for f in "${FOLDERS[@]}"; do cp "$f"/*.png "$WORK/pictures/"; done
NAME="$(basename "${SOURCE%.*}")"
cp "$SOURCE" "$WORK/$NAME.HMI"

editor_start
editor_open "$WORK/$NAME.HMI"

log "Importing $COUNT pictures"
click 17 459 3                                  # Picture panel: Add
click 550 458 0.5                               # file name field
type_text "$(winpath "$WORK/pictures")"
xdotool key Return                              # into the folder
sleep 2
click 550 300 0.5                               # file list
xdotool key ctrl+a                              # select all
sleep 1
click 737 458 3                                 # Open
# Pictures in a project: the .i entries main.HMI lists.
picture_count() {
    python3 -c "
import re, sys
sys.path.insert(0, '$(dirname "$(realpath "$0")")')
from hmi2txt import read_container
print(len(re.findall(rb'(\\d+)\\.i\\0', read_container(open(sys.argv[1], 'rb').read())['main.HMI'])))" "$1"
}
EXPECTED=$(( $(picture_count "$SOURCE") + COUNT ))

# "Import successfully N pieces" -- wait for the import to finish, confirm, save.
# With hundreds of pictures the import can pause long enough to look finished,
# and a save then misses pictures: so check the saved project and go round
# again until all are in.
for round in 1 2 3 4 5 6 7 8 9 10; do
    wait_stable "picture import" 480x180+272+292 8
    xdotool key Return
    sleep 2
    log "Saving"
    before=$(stat -c%Y "$WORK/$NAME.HMI")
    saved() { [ "$(stat -c%Y "$WORK/$NAME.HMI")" != "$before" ]; }
    # While it still imports, the Editor ignores a click on Save: click again
    # every 30 s until the file has been written (hundreds of pictures take
    # many minutes).
    for attempt in $(seq 1 40); do
        click 138 63 2                          # Save
        for _ in $(seq 1 30); do saved && break; sleep 1; done
        saved && break
    done
    saved || fail "the project was not saved"
    sleep 3
    have=$(picture_count "$WORK/$NAME.HMI")
    [ "$have" -ge "$EXPECTED" ] && break
    log "only $have of $EXPECTED pictures saved -- waiting for the import"
    sleep 20
done
[ "$have" -ge "$EXPECTED" ] || fail "the import did not finish ($have of $EXPECTED pictures)"

cp "$WORK/$NAME.HMI" "$TARGET"
log "Done: $TARGET"
