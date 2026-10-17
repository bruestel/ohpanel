#!/usr/bin/env bash
# Compiles a Nextion .HMI into a .tft by driving the Nextion Editor (wine) on an invisible Xvfb display.
#
# Usage: compile-hmi.sh <project.HMI> <output.tft>
#
# The editor saves the project while compiling - this script always works on a temporary copy.
# Editor automation: see editor-lib.sh.

set -euo pipefail

TOOL=compile-hmi
if [ $# -ne 2 ]; then
    echo "Usage: $0 <project.HMI> <output.tft>" >&2
    exit 1
fi
SOURCE="$(realpath "$1")"
TARGET="$(realpath -m "$2")"
[ -f "$SOURCE" ] || { echo "File not found: $SOURCE" >&2; exit 1; }
mkdir -p "$(dirname "$TARGET")"

. "$(dirname "$(realpath "$0")")/editor-lib.sh"

NAME="$(basename "${SOURCE%.*}")"
cp "$SOURCE" "$WORK/$NAME.HMI"

editor_start
editor_open "$WORK/$NAME.HMI"

# --- compile
log "Compiling"
click 197 63 2                                  # Compile
# wait until the output pane stops changing (upper part only, right of the left margin - the caret
# blinks at the start of the last line, which is inside the upper part when the output is short)
wait_stable "compile" 260x110+235+550 8
click 370 650 0.5                               # output pane: scroll to the summary line
xdotool key ctrl+End
sleep 1
screenshot "$WORK/compile.png"
# compile errors are written in red to the output pane
if region 290x200+225+550 txt:- | grep '#FF0000' >/dev/null; then
    cp "$WORK/compile.png" "${TARGET%.*}.compile.png"
    fail "compile reported errors"
fi

# --- export tft
log "Exporting TFT"
click 16 38 1.5                                 # File
click 65 62 3                                   # TFT file output
click 485 380 0.5                               # output folder field
xdotool key ctrl+a
type_text "$(winpath "$WORK")"
click 504 415 2                                 # Output
tft_written() {
    [ -f "$WORK/$NAME.tft" ] || return 1
    local size1 size2
    size1=$(stat -c%s "$WORK/$NAME.tft"); sleep 2; size2=$(stat -c%s "$WORK/$NAME.tft")
    [ "$size1" -gt 0 ] && [ "$size1" = "$size2" ]
}
wait_for "tft export" 120 tft_written

cp "$WORK/$NAME.tft" "$TARGET"
cp "$WORK/compile.png" "${TARGET%.*}.compile.png"
log "Done: $TARGET ($(stat -c%s "$TARGET") bytes), compiler output: ${TARGET%.*}.compile.png"
