"""OHpanel's layout on the NSPanel US (boards/nspanel/nextion/layout.py, OHPANEL_VARIANT "us"): the display upright,
320x480, all of it visible, the panel's two buttons under it -- left and right. The same names as
boards/nspanel/nextion/layout_eu.py, and on every page the same objects, as many of them: the firmware fills both
designs alike (as many rows, cards and tiles a sheet).

Titles start 12 px from the left as on the EU; the grid keeps that margin on both sides. Not yet seen on a panel.
"""

# The display as the design sees it: upright, as NSPanel-Easy turns its portrait design for the US (180 degrees in
# the Editor; Sonoff's own US design is turned like the EU's and drawn sideways).
SCREEN_W, SCREEN_H = 320, 480
DIRECTION = 180
TOUCH_OFFSET = None   # only the EU's display sits off its touch panel (NSPanel-Easy too sets it only there)

VISIBLE_W = SCREEN_W
VISIBLE_H = SCREEN_H
MARGIN = 12
RIGHT = VISIBLE_W - MARGIN            # 308: where the visible things end
# What shows, for centring by eye: the titles' glyphs end at y=24 (as on the EU); the page dots (in their 30 px box
# at y=446) start at y=457.
TITLE_GLYPH_BOTTOM = 24
DOTS_GLYPH_TOP = 457
TITLE = (10, 0, 260, 30)
CLOSE_TOUCH = (240, 0, 80, 44)
BACK_TOUCH = (0, 0, 230, 36)
WIDE_ROW_MAXL = 26                    # what fits in 252 px of font 3

# Button pages: 4 rows of 2 wide tiles (eight a sheet, as on the EU). A tile picture is 147x96 with the visible
# rounded box inset by 2 px (143x92), so object positions are 2 px left of and above what shows.
TILE_W, TILE_H = 147, 96
TILE_INSET = 2
TILE = TILE_W
TILE_VISIBLE_W, TILE_VISIBLE_H = TILE_W - 2 * TILE_INSET, TILE_H - 2 * TILE_INSET
COLUMNS, ROWS = 2, 4
GAP = (VISIBLE_W - 2 * MARGIN - COLUMNS * TILE_VISIBLE_W) // (COLUMNS - 1)   # 10
GRID_TOP = 46
BUTTON_DOTS = (MARGIN, 446, VISIBLE_W - 2 * MARGIN, 30)


def button_tiles():
    """(x, y) of the 8 tile pictures (tile1_pic..tile8_pic), row by row."""
    return [(MARGIN + c * (TILE_VISIBLE_W + GAP) - TILE_INSET, GRID_TOP + r * (TILE_VISIBLE_H + GAP) - TILE_INSET)
            for r in range(ROWS) for c in range(COLUMNS)]


# A button (ohpanel_hmi.py buttons_page, Engine::draw_button), relative to its tile picture: the icon top left, the
# name on the bottom line, its state above it, the bar of a value in % below them; a centred button's (and a
# favourite's) icon in the middle and its name under it -- the EU's places, the tile 4 px lower.
FAV_ICON = (TILE_INSET + 6, TILE_INSET + 6, TILE_VISIBLE_W - 12, 58)
TILE_PAD = TILE_INSET + 10
TILE_ICON = (TILE_INSET + 4, TILE_INSET + 6, 35, 35)
TILE_STATE = (TILE_PAD, 46, TILE_W - TILE_PAD - TILE_INSET - 4, 16)
TILE_NAME = (TILE_PAD, 62, TILE_W - TILE_PAD - TILE_INSET - 4, 20)
TILE_BAR = (TILE_PAD, 84, TILE_W - 2 * TILE_PAD, 4)
TILE_TEXT = (10, 67, TILE_W - 20, 20)


# The settings page: three rows of a label with its value and a slider below, the restart button at the bottom.
SETTINGS_X, SETTINGS_W = MARGIN, VISIBLE_W - 2 * MARGIN           # 12, 296
SETTINGS_ROW_Y = (52, 136, 220)
SETTINGS_SLIDER_DY, SETTINGS_SLIDER_H = 32, 30
SETTINGS_BUTTON = (SETTINGS_X - TILE_INSET, VISIBLE_H - MARGIN - 48, SETTINGS_W + 2 * TILE_INSET, 48)
SETTINGS_BUTTON_RADIUS = 14
SETTINGS_TEXT_W, SETTINGS_VALUE_W = 200, 96


# The close icon on the title's margins, as on the EU.
CLOSE_DISC = 25
CLOSE_ICON = (VISIBLE_W - MARGIN - CLOSE_DISC, 8, CLOSE_DISC, CLOSE_DISC)   # x 283..307


# Home ("C" of the sketches, 2026-10-23): on top the weather picture on the left, the clock and the date
# right-aligned beside it; under them the outdoor temperature large with today's low and high under it. At the
# bottom, standing on the relays: the two value rows on the left with the shortcuts beside them on the right, the
# two info lines, the relays' labels and bars over the two buttons.
STATUS_ICONS = [(MARGIN + 1 + 27 * k, 5) for k in range(10)]   # up to ten, 27 px apart: clear of the WiFi icon
WIFI_ICON = (RIGHT - 24, 5, 25, 25)
STATUS_VALUE = (RIGHT - 124, 5, 100, 25)
STATUS_VALUE_ICON = (RIGHT - 22, 5, 25, 25)
WEATHER_HOME = (MARGIN - 2, 34, 104, 104)
WEATHER_BOTTOM = WEATHER_HOME[1] + WEATHER_HOME[3] - 2           # 136
# the clock (font 6) right-aligned beside the weather picture, its digits from y=51; AM/PM small above its
# minutes, under the WiFi icon; the date under it (font 3) as on the EU, 65 px below the clock's top
CLOCK = (RIGHT - 170, 36, 170, 62)
CLOCK_BASELINE, DATE_BASELINE = 96, 125
MERIDIEM = (30, 40, 22)
DATE = (RIGHT - 180, 101, 180, 30)
# the outdoor temperature large (48 px font) from the left margin under the weather picture, its unit small at its
# top right after it; the low and high under it (as the EU's beside the weather picture)
OUTDOOR_BASELINE = 186
OUTDOOR_TEMP_CY = OUTDOOR_BASELINE - 15
OUTDOOR_TEMP = (MARGIN, OUTDOOR_TEMP_CY - 24, 130, 48)
OUTDOOR_UNIT_Y, OUTDOOR_UNIT_H, OUTDOOR_DIGIT = OUTDOOR_TEMP_CY - 19, 24, 25
OUTDOOR_UNIT_STEP = 13
FONT_OUTDOOR, FONT_OUTDOOR_UNIT = 5, 2
OUTDOOR_UNIT_STEPS = 7
FONT_RANGE = 2
RANGE_BASELINE = OUTDOOR_BASELINE + 29      # the outdoor field (to y=194) clear of the arrows' (from 195)
RANGE_Y, RANGE_H = RANGE_BASELINE + 5 - 22, 22
RANGE_ARROW_Y = (RANGE_BASELINE - 8) - 12
RANGE_ARROW_W = 24
RANGE_DOWN_X = MARGIN - 4
RANGE_LOW_X = RANGE_DOWN_X + RANGE_ARROW_W
RANGE_STEP, RANGE_STEPS = 6, 7
RANGE_GAP = 4
RANGE_HIGH_W = 38
RANGE_END = RIGHT                            # nothing beside them
# the relays' labels over the two buttons (their middles at x=80 and 240), a bar under each
RELAYS = [(20, 440, 120, 20), (180, 440, 120, 20)]
RELAY_BAR_DY, RELAY_BAR_H = 24, 3
# the info lines right above them
INFO_H = 21
INFO_Y = (386, 410)
INFO_ICON_UP, INFO_ICON_H = 4, 24
INFO_LEFT = (MARGIN, 26)
BADGE_RADIUS = 5
INFO_RIGHT = (RIGHT - 52, 52)
INFO_GAP = 8
INFO_MAIN_X = 44
# the two value rows above the info lines, the number fields touching
VALUE_ROWS = (334, 362)
VALUE_NUMBER_H = 28
VALUE_ICON_X = MARGIN - 2
VALUE_NUMBER = (37, 100)
VALUE_UNIT_W, VALUE_DIGIT, VALUE_UNIT_STEPS = 36, 13, 12
VALUE_UNIT_STEP = 7
FONT_VALUE, FONT_VALUE_UNIT = 3, 1
# the shortcuts (32 px font) in 35 px fields 47 px apart, right-aligned: the first three beside the value rows,
# the other four in a row above them
SHORTCUT, SHORTCUT_STEP = 35, 47
SHORTCUTS = [(RIGHT + 1 - SHORTCUT - k * SHORTCUT_STEP, y) for y, n in ((331, 3), (284, 4)) for k in reversed(range(n))]


# The weather page: the icon left, the high large beside it, the low under that -- or the high smaller with the
# low after it and the condition across the page under them --; six cards of an extra in two columns; the dots.
WEATHER_PAGE = (MARGIN + 4, 44, 100, 100)
WEATHER_HIGH = (128, 46, 180, 58)
WEATHER_LOW_ARROW = (128, 102, 26, 34)
WEATHER_LOW = (156, 102, 152, 34)
WEATHER_LINE_HIGH = (128, 48, 100, 46)
WEATHER_LINE_LOW_X0, WEATHER_LINE_STEP, WEATHER_LINE_STEPS = 172, 7, 6
WEATHER_LINE_LOW_Y, WEATHER_LINE_LOW_H = 56, 38
WEATHER_LINE_LOW_W = 80
WEATHER_CONDITION = (MARGIN, 150, VISIBLE_W - 2 * MARGIN, 36)
WEATHER_CHIP_W, WEATHER_CHIP_H, WEATHER_CHIP_RADIUS = (VISIBLE_W - 2 * MARGIN - 8) // 2, 60, 10   # 144 x 60
WEATHER_CHIPS = [(MARGIN + c * (WEATHER_CHIP_W + 8), 200 + r * (WEATHER_CHIP_H + 10)) for r in range(3) for c in range(2)]

# The entity page in three styles (list, cards, rows), seven rows a sheet as on the EU, taller.
ENTITY_LIST_Y, ENTITY_LIST_STEP, ENTITY_LIST_H, ENTITY_LIST_N = 46, 54, 50, 7
ENTITY_ICON_X = MARGIN + 18
ENTITY_TEXT_X = 56
ENTITY_VALUE_W = 116
ENTITY_VALUE_NARROW = 72
ENTITY_LINE = (90, 90, 90)
ENTITY_FONT = 2
ENTITY_ROWS_Y, ENTITY_ROWS_STEP, ENTITY_ROWS_H, ENTITY_ROWS_N = 44, 56, 48, 7
ENTITY_ROWS_RADIUS = 10
ENTITY_DOTS = (MARGIN, 450, VISIBLE_W - 2 * MARGIN, 26)

# The boot page: the title, the hotspot's QR code under it in the middle, the rows of a label and a value under the
# code, the logo at the bottom.
BOOT_TITLE = (16, 26, 288, 35)
BOOT_MARGIN_X, BOOT_MARGIN_Y = 18, 76
BOOT_QR_SIZE = 110
BOOT_QR = ((VISIBLE_W - BOOT_QR_SIZE) // 2, BOOT_MARGIN_Y, BOOT_QR_SIZE, BOOT_QR_SIZE)
BOOT_ROWS_Y = (206, 231, 256, 281, 306)   # firmware, device, IP, IPv6, more
BOOT_LABEL = (16, 90)
BOOT_VALUE_X = 108
BOOT_VALUE_GAP = 4
BOOT_BELOW_QR = ("firmware", "device", "ip", "ipv6", "info")   # every row: the whole width
BOOT_LOGO_W, BOOT_LOGO_H = 300, 64
BOOT_LOGO = ((VISIBLE_W - BOOT_LOGO_W) // 2, VISIBLE_H - 34 - BOOT_LOGO_H, BOOT_LOGO_W, BOOT_LOGO_H)
# The logo the display shows itself when it starts (layout_eu.py): as wide as the boot page's, the screen allowing
# no more.
SPLASH_LOGO_W, SPLASH_LOGO_H = 300, 64
SPLASH_LOGO = ((VISIBLE_W - SPLASH_LOGO_W) // 2, (VISIBLE_H - SPLASH_LOGO_H) // 2, SPLASH_LOGO_W, SPLASH_LOGO_H)

# The panel's own page: the eight cards in one column, a line each -- the label small above the value, or the
# label smaller and the value in two lines.
SYSTEM_CARD_W, SYSTEM_CARD_H = VISIBLE_W - 2 * MARGIN, 50     # 296 x 50
SYSTEM_CARDS = [(MARGIN, 36 + r * (SYSTEM_CARD_H + 2)) for r in range(8)]   # the page dots under them (ENTITY_DOTS)
SYSTEM_CARD_RADIUS = 12
SYSTEM_TEXT_X = 48
SYSTEM_TEXT_FONT, SYSTEM_VALUE_FONT = 0, 2
SYSTEM_PARTS = {"text": (4, 18), "value": (22, 26), "top": (2, 16), "line1": (17, 16), "line2": (33, 16)}
# the network page: a label with its value under it, the whole width (an IPv6 address is long)
NETWORK_ROWS_Y, NETWORK_ROWS_STEP, NETWORK_ROWS_N = 40, 46, 9
NETWORK_LABEL_W = 104
NETWORK_STACKED = True

# The media page: the art at the top in the middle, what plays under it, the controls under that, the favourites'
# and the volume's discs at the bottom on the margins.
MEDIA_ART = ((VISIBLE_W - 160) // 2, 44, 160, 160)
MEDIA_TITLE = (MARGIN, 214, VISIBLE_W - 2 * MARGIN, 44)
MEDIA_SUB = (MARGIN, 258, VISIBLE_W - 2 * MARGIN, 32)
MEDIA_SOURCE = (MARGIN, 292, VISIBLE_W - 2 * MARGIN, 26)
MEDIA_CONTROLS = (VISIBLE_W // 2 - 95, VISIBLE_W // 2, VISIBLE_W // 2 + 95, 368)
MEDIA_PLAY_DISC = 84
MEDIA_VOLUME = (RIGHT - 48, 420, 48, 48)
MEDIA_FAV = (MARGIN, 420, 48, 48)
ART_RADIUS, PILL_COLOUR = 18, (60, 66, 60)

# The volume page: the title, the volume large between minus and plus, the slider, the mute button at the bottom.
VOLUME_ROW = (MARGIN, 80, VISIBLE_W - 2 * MARGIN, 64)
VOLUME_SLIDER = (MARGIN, 180, VISIBLE_W - 2 * MARGIN, 30)

# The QR page: the code large under the title, the caption under it.
QR_AREA_SIZE = 260
QR_AREA = ((VISIBLE_W - QR_AREA_SIZE) // 2, 50, QR_AREA_SIZE, QR_AREA_SIZE)
QR_CAPTION = [(MARGIN, 330, VISIBLE_W - 2 * MARGIN, 24), (MARGIN, 356, VISIBLE_W - 2 * MARGIN, 24)]

# The light page (ohpanel_hmi.py control_page): the title, its icon and value under it on the left; the slider
# upright in the middle (or the colour wheel, larger: the whole width); a row of tiles at the bottom -- the on / off
# tile, then the light's views. The tiles are the button pages' drawn smaller, inset by 2 px like them.
SIDE_TILE = 72
SIDE_ICON_CY, SIDE_TEXT_Y = 28, 48
TILES_Y = VISIBLE_H - MARGIN - SIDE_TILE + TILE_INSET                  # 398
SIDE_TILES = [(MARGIN - TILE_INSET + k * (SIDE_TILE + 4), TILES_Y) for k in range(1, 4)]
POWER_TILE = (MARGIN - TILE_INSET, TILES_Y)
CONTROL_TOP, CONTROL_BOTTOM = 96, TILES_Y - 14                     # the slider and the wheel: 96 .. 384
CONTROL_SLIDER_W = 110
CONTROL_SLIDER = (VISIBLE_W // 2 - CONTROL_SLIDER_W // 2, CONTROL_TOP, CONTROL_SLIDER_W, CONTROL_BOTTOM - CONTROL_TOP)
CONTROL_HANDLE = 24
CONTROL_ICON = (MARGIN + 22, 62)                                     # the icon's middle, under the title
CONTROL_VALUE_W, CONTROL_VALUE_H = 120, 40
CONTROL_VALUE = (CONTROL_ICON[0] + 22, CONTROL_ICON[1] - CONTROL_VALUE_H // 2, CONTROL_VALUE_W, CONTROL_VALUE_H)
CONTROL_VALUE_FONT = 3
COLOUR_WHEEL_D = 280
COLOUR_WHEEL = (VISIBLE_W // 2 - COLOUR_WHEEL_D // 2, (CONTROL_TOP + CONTROL_BOTTOM) // 2 - COLOUR_WHEEL_D // 2,
                COLOUR_WHEEL_D, COLOUR_WHEEL_D)
COLOUR_SWATCH = (POWER_TILE[0] + SIDE_TILE // 2, POWER_TILE[1] + SIDE_TILE // 2, 22)
SLIDER_HANDLE = 14

# The cover page: the drawing on the left, the position large, the state and which way it moves on the right of it;
# up / stop / down under them across the page, the presets under those.
COVER_DRAW = (MARGIN, 44, 140, 256)
COVER_RIGHT = 164
COVER_VALUE = (COVER_RIGHT, 44, RIGHT - COVER_RIGHT, 56)
COVER_STATE = (COVER_RIGHT + 2, 100, RIGHT - COVER_RIGHT - 2, 24)
COVER_MOVING_ICON = (COVER_RIGHT + 2, 126, 24, 24)
COVER_MOVING = (COVER_RIGHT + 28, 124, RIGHT - COVER_RIGHT - 28, 26)
COVER_BUTTONS = [(MARGIN + k * 103, 316, 90, 64) for k in range(3)]
COVER_PRESETS = [(MARGIN + k * 103, 396, 90, 40) for k in range(3)]
COVER_PRESET_INSET = 8

# The thermostat page: the ring under the title, minus and plus under it on the left and the right, the modes as
# pills under them.
THERMO_CENTRE = (VISIBLE_W // 2, 186)
THERMO_RADIUS = 112
THERMO_RING_MARGIN = 18
THERMO_RING = (THERMO_CENTRE[0] - THERMO_RADIUS - THERMO_RING_MARGIN, THERMO_CENTRE[1] - THERMO_RADIUS -
               THERMO_RING_MARGIN, 2 * (THERMO_RADIUS + THERMO_RING_MARGIN),
               2 * (THERMO_RADIUS + THERMO_RING_MARGIN))
THERMO_NOW = (THERMO_CENTRE[0] - 85, THERMO_CENTRE[1] - 58, 170, 76)
THERMO_TARGET = (THERMO_CENTRE[0] - 88, THERMO_CENTRE[1] + 22, 176, 26)
THERMO_STATE = (THERMO_CENTRE[0] - 75, THERMO_CENTRE[1] + 46, 150, 24)
THERMO_BUTTON_R = 24
THERMO_BUTTONS = [(64, 352), (VISIBLE_W - 64, 352)]
THERMO_MODES_Y, THERMO_MODES_H, THERMO_MODES_GAP = 404, 34, 8
THERMO_MODE_W = {1: 120, 2: 120, 3: 92, 4: 68}


def thermo_modes(n: int) -> list:
    """The mode pills when there are n of them: x, y, w, h each, the row centred under the ring."""
    w = THERMO_MODE_W[n]
    left = THERMO_CENTRE[0] - (n * w + (n - 1) * THERMO_MODES_GAP) // 2
    return [(left + k * (w + THERMO_MODES_GAP), THERMO_MODES_Y, w, THERMO_MODES_H) for k in range(n)]


# The keypad page: the state and the code's places under the title, the keys under them, the buttons at the bottom
# (one to three side by side, four two pairs).
KEYPAD_ROWS = (166, 228, 290, 352)
KEYPAD_COLS = (80, 160, 240)
KEYPAD_KEY = (76, 60)
KEYPAD_DISC = 27
KEYPAD_STATE = (MARGIN + 4, 51, VISIBLE_W - 2 * (MARGIN + 4), 30)
KEYPAD_CODE = (MARGIN + 4, 91, VISIBLE_W - 2 * (MARGIN + 4), 30)
KEYPAD_BUTTONS_Y = 394


def keypad_buttons(n: int) -> list:
    """The buttons when there are n of them, at the bottom: one to three side by side, 52 px high; four as two pairs."""
    x, w = MARGIN + 4, VISIBLE_W - 2 * (MARGIN + 4)
    if n == 4:
        half = (w - 8) // 2
        return [(x + (k % 2) * (half + 8), KEYPAD_BUTTONS_Y + (k // 2) * 44, half, 36) for k in range(4)]
    each = (w - 8 * (n - 1)) // n
    return [(x + k * (each + 8), KEYPAD_BUTTONS_Y + 14, each, 52) for k in range(n)]


# The cover group page: the whole group at the top, four members a sheet under it; each row in two lines -- the
# name across it, under it a member's position on the left and up / stop / down on the right.
COVERS_ALL_Y, COVERS_ALL_H = 38, 84
COVERS_ALL_BUTTON = (72, 40)
COVERS_ROWS_Y, COVERS_ROWS_STEP, COVERS_ROWS_N = 128, 78, 4
COVERS_ROWS_H = 72
COVERS_MINI = (10, 10, 26, 30)
COVERS_TEXT_X, COVERS_TEXT_W = 46, 0         # the name: to the row's end (stacked)
COVERS_VALUE_W = 70
COVERS_BUTTON, COVERS_BUTTON_GAP, COVERS_BUTTON_END = 34, 8, 8
COVERS_STACKED, COVERS_LINE_H = True, 28
COVERS_PRESET_W = (VISIBLE_W - 2 * MARGIN - 2 * 8) // 3
COVERS_PRESET_H = 44                         # pills: as high as the EU's rows (the firmware rounds their ends)
COVERS_PRESETS = [(MARGIN + k * (COVERS_PRESET_W + 8), COVERS_ROWS_Y + (COVERS_ROWS_H - COVERS_PRESET_H) // 2,
                   COVERS_PRESET_W, COVERS_PRESET_H) for k in range(3)]
