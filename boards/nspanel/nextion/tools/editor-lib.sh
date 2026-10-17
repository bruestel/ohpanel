# Shared helpers for driving the Nextion Editor (wine) on an invisible Xvfb display.
# Sourced by compile-hmi.sh and import-pictures.sh; expects TOOL (log prefix) and TARGET (for error screenshots).
#
# Requirements: wine prefix with Nextion Editor 1.68.1, Xvfb, xdotool, ImageMagick.
# Click positions are for the 1024x768 wine virtual desktop of the prefix and may need adjusting after editor updates.

WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/wineprefixes/.nextion}"
EDITOR_DIR="$WINEPREFIX/drive_c/Program Files (x86)/Nextion Editor"
XVFB_DISPLAY="${XVFB_DISPLAY:-:77}"
TIMEOUT="${TIMEOUT:-300}"
XVFB_PID=""
WORK="$(mktemp -d)"

export DISPLAY="$XVFB_DISPLAY" WINEPREFIX WINEDEBUG=-all
unset WAYLAND_DISPLAY

editor_cleanup() {
    WINEPREFIX="$WINEPREFIX" wineserver -k 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap editor_cleanup EXIT

log() { echo "[$TOOL] $*"; }
winpath() { printf '%s' "Z:$(printf '%s' "$1" | tr / '\\')"; }
click() { xdotool mousemove --window "$WINDOW" "$1" "$2" click 1; sleep "${3:-1}"; }
type_text() { xdotool type --delay 20 "$1"; }
region() { import -window "$WINDOW" png:- 2>/dev/null | magick png:- -crop "$1" +repage -depth 8 "$2" 2>/dev/null; } # <WxH+X+Y> <format:->
pixel() { region "1x1+$1+$2" txt:- | grep -o -m1 '#[0-9A-F]\{6\}'; }
screenshot() { import -window "$WINDOW" "$1"; }
fail() {
    screenshot "${TARGET%.*}.error.png" 2>/dev/null || true
    echo "[$TOOL] ERROR: $* (screenshot: ${TARGET%.*}.error.png)" >&2
    exit 1
}
wait_for() { # <description> <seconds> <command...>
    local description="$1" seconds="$2" start=$SECONDS
    shift 2
    until "$@"; do
        [ $((SECONDS - start)) -lt "$seconds" ] || fail "timeout while waiting for: $description"
        sleep 1
    done
}
# Waits until a screen region stops changing for `count` seconds.
wait_stable() { # <description> <WxH+X+Y> <count>
    local previous="" current stable=0 start=$SECONDS
    while [ $stable -lt "$3" ]; do
        current="$(region "$2" rgb:- | md5sum)"
        if [ "$current" = "$previous" ]; then stable=$((stable + 1)); else stable=0; fi
        previous="$current"
        [ $((SECONDS - start)) -lt "$TIMEOUT" ] || fail "timeout while waiting for: $1"
        sleep 1
    done
}

editor_start() {
    if pgrep -f "Nextion Editor.exe" >/dev/null; then
        echo "Nextion Editor is already running in this wine prefix - please close it first." >&2
        exit 1
    fi
    log "Starting Xvfb on $XVFB_DISPLAY"
    Xvfb "$XVFB_DISPLAY" -screen 0 1280x900x24 -nolisten tcp >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1
    log "Starting Nextion Editor"
    (cd "$EDITOR_DIR" && wine "Nextion Editor.exe" >/dev/null 2>&1 &)
    wait_for "wine desktop" 60 sh -c 'xdotool search --name "^Wine Desktop$" >/dev/null'
    WINDOW="$(xdotool search --name "^Wine Desktop$" | head -1)"
    # the menu bar of the editor replaces the splash screen (teal desktop) once the editor is ready
    editor_ready() { [ "$(pixel 600 38)" = "#BFDBFF" ]; }
    wait_for "editor startup" 120 editor_ready
    sleep 3
}

# Opens <file.HMI>, which must be a path the editor may rewrite.
editor_open() {
    log "Opening $(basename "$1")"
    click 36 63 3                               # Open
    click 550 458 0.5                           # file name field
    xdotool key ctrl+a
    type_text "$(winpath "$1")"
    xdotool key Return
    # the first page ("home") gets selected in the page list once the project is loaded
    project_loaded() { region 160x20+830+152 txt:- | grep '#0000FF' >/dev/null; }
    wait_for "project load" 120 project_loaded
    sleep 2
}
