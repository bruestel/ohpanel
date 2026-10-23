"""Where OHpanel's own layout puts things on the display -- shared by the design
recipe (boards/nspanel/nextion/ohpanel_hmi.py) and the picture composer
(boards/nspanel/nextion/assets/compose.py), which must agree on every position.

The panel variant it is for comes from OHPANEL_VARIANT (boards/nspanel/nextion/build.sh):
"eu", the NSPanel EU, the default, or "us", the NSPanel US (portrait, not laid out yet).

The NSPanel EU's frame hides the rightmost 30 px of the 480x320 display, so
the visible area is 450x320. Titles start 12 px from the left (their glyphs,
font 3 in a 30 px label at x=10); the grid keeps that margin on both sides.
"""

import os
import sys

VARIANTS = ("eu", "us")
VARIANT = os.environ.get("OHPANEL_VARIANT", "eu")
if VARIANT not in VARIANTS:
    sys.exit(f"layout: no panel variant {VARIANT!r} (OHPANEL_VARIANT: {', '.join(VARIANTS)})")
if VARIANT != "eu":
    sys.exit(f"layout: the variant {VARIANT!r} is not laid out yet")

# The display as the design sees it, turned as the panel mounts it (the Editor's direction, ohpanel_hmi.py).
SCREEN_W, SCREEN_H = 480, 320
DIRECTION = 270
# The touch panel's offset (lcd_dev, ohpanel_hmi.py program()): the NSPanel EU's display sits a few millimetres off
# its touch panel; None where it needs none.
TOUCH_OFFSET = "fffb 0002 0000 0020"

VISIBLE_W = 450
VISIBLE_H = SCREEN_H
MARGIN = 12          # the titles' left margin, kept on the right too
# What shows, for centring by eye: the titles' glyphs end at y=24 (measured in
# the simulator); the page dots (circle-medium, 8 px in the 24 px icon font,
# centred in their 30 px box at y=288) start at y=299.
TITLE_GLYPH_BOTTOM = 24
DOTS_GLYPH_TOP = 299

# Button pages: 2 rows of 4 tiles. A tile picture is 100x100 with the visible
# rounded square inset by 2 px (96x96, boards/nspanel/nextion/assets/generate.py), so object
# positions are 2 px left of and above what shows.
TILE = 100
TILE_INSET = 2
TILE_VISIBLE = TILE - 2 * TILE_INSET
COLUMNS, ROWS = 4, 2
GAP = (VISIBLE_W - 2 * MARGIN - COLUMNS * TILE_VISIBLE) // (COLUMNS - 1)        # 14
GRID_H = ROWS * TILE_VISIBLE + (ROWS - 1) * GAP
# the same visible space above and below the grid
GRID_TOP = TITLE_GLYPH_BOTTOM + 1 + (DOTS_GLYPH_TOP - TITLE_GLYPH_BOTTOM - 1 - GRID_H) // 2   # 59


# The settings page (page type "settings"): three rows of a label with its
# value and a slider below, then the restart button -- a tile like the button
# pages' (rounded, inset by 2 px in its picture, boards/nspanel/nextion/assets/generate.py)
# across the page's width.
SETTINGS_X, SETTINGS_W = MARGIN, VISIBLE_W - 2 * MARGIN           # 12, 426
SETTINGS_ROW_Y = (44, 114, 184)          # label and value, 28 px high
SETTINGS_SLIDER_DY, SETTINGS_SLIDER_H = 30, 30   # as high as the volume page's: easy to take hold of
# x, y, w, h of its picture: at the bottom, as far from the edge as the sides (308 + MARGIN = the visible 320)
SETTINGS_BUTTON = (SETTINGS_X - TILE_INSET, 320 - MARGIN - 48, SETTINGS_W + 2 * TILE_INSET, 48)
SETTINGS_BUTTON_RADIUS = 14


def button_tiles():
    """(x, y) of the 8 tile pictures (tile1_pic..tile8_pic), row by row."""
    return [(MARGIN + c * (TILE_VISIBLE + GAP) - TILE_INSET, GRID_TOP + r * (TILE_VISIBLE + GAP) - TILE_INSET)
            for r in range(ROWS) for c in range(COLUMNS)]


# The close icon (a dark disc with a light cross, baked per background by
# boards/nspanel/nextion/assets/compose.py) on the title's margins: its
# right edge 12 px left of the visible edge, its top where the titles' glyphs
# start (y=8).
CLOSE_DISC = 25
CLOSE_ICON = (VISIBLE_W - MARGIN - CLOSE_DISC, 8, CLOSE_DISC, CLOSE_DISC)   # x, y, w, h: x 413..437



# Home, left half: everything from the left margin. The weather picture is 100x100
# (boards/nspanel/nextion/assets/weather) with the icon drawn inset, right under the status
# icons; the outdoor temperature right beside it (its icons reach its right
# edge at most). Below, two value rows -- an icon (24 px font) and a value
# each, the value where the info lines' main text starts -- then the two info
# lines.
WEATHER_HOME = (MARGIN, 36, 116, 116)        # x, y, w, h
# the status row: up to ten icons (24 px font) from the left, 29 px apart
STATUS_ICONS = [(MARGIN + 1 + 29 * k, 5) for k in range(10)]   # x, y of each 25x25 field
# Baselines as the display draws them (measured in the simulator): the
# clock's digits end at y=105, the date's at 134; a 48 px number centred in
# its field ends 15 px below the field's middle.
CLOCK_BASELINE, DATE_BASELINE = 105, 134
# AM/PM small above the clock's minutes, right-aligned with them: its field
# reaches into the top of the clock's (y=45..107), where the clock draws
# nothing -- its digits begin at y=60. The clock repaints its whole field, so
# the firmware redraws both together, AM/PM after it (Engine::draw_clock);
# a smaller clock field cuts the digits off (the display crops to the field).
MERIDIEM = (36, 40, 22)        # y, w, h: font 2, bottom-aligned: its letters at y=41..53
# The weather block ends where the weather icon does: the icons stand on their
# field's bottom (boards/nspanel/nextion/assets/compose.py), 2 px above its edge; today's low
# and high on that line, the outdoor temperature above them.
WEATHER_BOTTOM = WEATHER_HOME[1] + WEATHER_HOME[3] - 2           # 150
RANGE_BASELINE = WEATHER_BOTTOM
OUTDOOR_BASELINE = 121      # its field (to y=129) clear of the arrows' (from 130)
OUTDOOR_TEMP_CY = OUTDOOR_BASELINE - 15
# the number large (48 px font) and left-aligned beside the icon, its unit
# small at its top right after it (22 px font), like a superscript. The
# display cannot move a field, so the unit comes at UNIT_STEPS places half a
# digit apart (the fonts' digits are all as wide; a comma is narrower) and the
# firmware shows the one that follows the number.
# 14 px clear of the weather icon's picture; 130 px wide -- "-12,5°" with the degree sign in its own
# size (Engine::draw_home) -- ending before the date (x=273)
WEATHER_GAP = 14
OUTDOOR_TEMP = (WEATHER_HOME[0] + WEATHER_HOME[2] + WEATHER_GAP, OUTDOOR_TEMP_CY - 24, 130, 48)
OUTDOOR_UNIT_Y, OUTDOOR_UNIT_H, OUTDOOR_DIGIT = OUTDOOR_TEMP_CY - 19, 24, 25
OUTDOOR_UNIT_STEP = 13   # half a digit, rounded up
FONT_OUTDOOR, FONT_OUTDOOR_UNIT = 5, 2
OUTDOOR_UNIT_STEPS = 7   # the last place ends before the date
# today's low and high under it, a size smaller (22 px font): "↓9° ↑22°", a
# thin arrow (the 24 px icon font) and a value each; the fields bottom-aligned,
# their line 5 px below the baseline, clear of the weather picture (to x=128),
# the value rows (from y=160) and the date (from x=273). Fields cannot move,
# so as with the units: the low in a field as wide as it (RANGE_STEPS places
# half a digit apart), the high's arrow and value right after it at as many
# places (place 0: at the low's own place, for a high without a low).
FONT_RANGE = 2
RANGE_Y, RANGE_H = RANGE_BASELINE + 5 - 22, 22
RANGE_ARROW_Y = (RANGE_BASELINE - 8) - 12       # 24 px box: the thin arrow beside the digits (139..150)
RANGE_ARROW_W = 24
RANGE_DOWN_X = WEATHER_HOME[0] + WEATHER_HOME[2] + WEATHER_GAP - 4   # the low's arrow (its box has 4 px inside)
RANGE_LOW_X = RANGE_DOWN_X + RANGE_ARROW_W       # the low's value
RANGE_STEP, RANGE_STEPS = 6, 7                   # up to 42 px: "-12°" is 35, "9,5°" 34
RANGE_GAP = 4                                    # after the low (the arrow's box has its own margin)
RANGE_HIGH_W = 38                                # "-12°"; the last place ends before the date
# the info lines: 21 px apart, their fields as high, the text centred
INFO_H = 21
INFO_Y = (230, 251)       # the two lines' top
# the left field (a line number, mostly) may be a badge: a rounded box in a
# colour, a picture per colour and background (boards/nspanel/nextion/assets/compose.py)
INFO_LEFT = (MARGIN, 26)  # x, w: even, so a two-digit number has as much room left as right
BADGE_RADIUS = 5
# the right field (a time, mostly) clear of the main text by INFO_GAP
INFO_RIGHT = (196, 52)    # x, w
INFO_GAP = 8
INFO_MAIN_X = 44          # the main text: 6 px clear of the left field (12..38)
# the vertical centres of the two value rows: one group, the number fields
# (VALUE_NUMBER_H) touching -- overlapping ones would repaint each other
VALUE_ROWS = (174, 202)
VALUE_NUMBER_H = 28        # the 26 px font and a pixel each side
VALUE_ICON_X = MARGIN - 2  # the 24 px glyph in a 25 px box starts 2 px in
# a value row: icon | number | unit (small, at the top, right after it)
VALUE_NUMBER = (37, 100)   # x, w
VALUE_UNIT_W, VALUE_DIGIT, VALUE_UNIT_STEPS = 36, 13, 12
VALUE_UNIT_STEP = 7      # half a digit, rounded up: 12 places cover 6 digits
FONT_VALUE, FONT_VALUE_UNIT = 3, 1


# Home, right half: the WiFi icon at the right end of the status row; the
# clock, the date and the shortcut icons right-aligned at the right margin.
RIGHT = VISIBLE_W - MARGIN            # 438: where the visible things end
WIFI_ICON = (RIGHT - 24, 5, 25, 25)   # the 24 px glyph in a 25 px box
# beside the WiFi icon, right-aligned: a value and its icon (32 px font)
STATUS_VALUE = (RIGHT - 124, 5, 100, 25)
STATUS_VALUE_ICON = (RIGHT - 22, 5, 25, 25)
CLOCK = (RIGHT - 160, 45, 160, 62)    # font 6, right-aligned
DATE = (RIGHT - 165, 110, 165, 30)    # font 3, right-aligned, on the field's bottom
# the shortcut icons (32 px font) in 35 px fields 47 px apart, right-aligned in two rows: three, then four
SHORTCUT, SHORTCUT_STEP = 35, 47
SHORTCUTS = [(RIGHT + 1 - SHORTCUT - k * SHORTCUT_STEP, y) for y, n in ((176, 3), (224, 4)) for k in reversed(range(n))]
# the relays' labels, a bar under each that the firmware colours by the relay's state
RELAYS = [(47, 283, 120, 20), (288, 283, 120, 20)]   # x, y, w, h of each label: left, right
RELAY_BAR_DY, RELAY_BAR_H = 24, 3


# The weather page (page type "weather", the days of the forecast as sheets):
# the day and its date as the title; the icon left (boards/nspanel/nextion/assets/
# weather pictures, 100x100), the high large beside it, the low under it with
# its arrow, the condition in words under that; below, six cards of an extra (wind, rain, ...): an icon, the value,
# a small label; the sheets' dots at the bottom.
WEATHER_PAGE = (MARGIN + 4, 46, 100, 100)    # x, y, w, h of the icon's picture
WEATHER_HIGH = (128, 48, 300, 58)            # the high, font 5 (48 px)
WEATHER_LOW_ARROW = (128, 104, 26, 34)       # the low's arrow (24 px icon font) and the low, font 3, beside it
WEATHER_LOW = (156, 104, 270, 34)
# with the condition in words: the high a size smaller with the low right after it on one line ("16°  ↓ 11°"), the
# condition large under them. The display moves no fields: the low and its arrow wait at WEATHER_LINE_STEPS places
# after the high, the firmware shows the one its width needs (as home's low and high).
WEATHER_LINE_HIGH = (128, 50, 150, 46)        # font 4 (36 px), bottom-aligned
WEATHER_LINE_LOW_X0, WEATHER_LINE_STEP, WEATHER_LINE_STEPS = 172, 10, 10   # where the low's arrow may start
WEATHER_LINE_LOW_Y, WEATHER_LINE_LOW_H = 58, 38
WEATHER_CONDITION = (128, 102, 310, 36)      # font 3 (26 px)
WEATHER_CHIP_W, WEATHER_CHIP_H, WEATHER_CHIP_RADIUS = 136, 56, 10
WEATHER_CHIPS = [(MARGIN + c * (WEATHER_CHIP_W + 9), 166 + r * (WEATHER_CHIP_H + 8)) for r in range(2) for c in range(3)]

# The entity page (page type "entities") in three styles, each a page of the
# design of its own ("style": list, cards, rows):
#   list    rows of an icon (32 px font), the label and the value right-aligned
#           -- or one wide field --, a thin line between them;
#   cards   the panel info's cards (SYSTEM_CARDS, their pictures): an icon, a
#           small label above the value;
#   rows    a card per row across the page (compose.py "rowcard1".."rowcard7"),
#           an icon (24 px font), the label and the value right-aligned.
ENTITY_LIST_Y, ENTITY_LIST_STEP, ENTITY_LIST_H, ENTITY_LIST_N = 46, 35, 34, 7
ENTITY_ICON_X = MARGIN + 18                  # the list's and the rows' icon middle
ENTITY_TEXT_X = 56                           # label (and the wide field) from here
ENTITY_VALUE_W = 168                         # the value right-aligned at RIGHT
ENTITY_VALUE_NARROW = 92                     # a narrow value (a percentage, watts): the label longer beside it
ENTITY_LINE = (90, 90, 90)                   # the list's thin line between rows
ENTITY_FONT = 2
ENTITY_ROWS_Y, ENTITY_ROWS_STEP, ENTITY_ROWS_H, ENTITY_ROWS_N = 44, 36, 32, 7
ENTITY_ROWS_RADIUS = 10
ENTITY_DOTS = (VISIBLE_W // 2 - 100, 294, 200, 26)   # the sheets' dots: below the cards' last row (294)

# The boot page: the title ("Initializing ..."), four rows of a label and a
# value (font 2), the hotspot's QR code beside them, the logo at the bottom.
BOOT_TITLE = (27, 26, 250, 35)
BOOT_ROWS_Y = (70, 95, 120, 145, 170)   # firmware, device, IP, IPv6, more
BOOT_LABEL = (27, 95)            # x, w
BOOT_VALUE_X = 132

# The panel's own page (page type "system", the panel info): two columns of
# four cards (rounded, flat, a picture per background: compose.py "card1".."card8"),
# each an icon (32 px font) on the left, a small label above the value -- or,
# for a value too long for one line (a long WiFi name), the label smaller and
# the value in two lines; a small icon top right (the WiFi signal).
SYSTEM_CARD_W, SYSTEM_CARD_H = (VISIBLE_W - 2 * MARGIN - 6) // 2, 58     # 210 x 58
SYSTEM_CARDS = [(MARGIN + c * (SYSTEM_CARD_W + 6), 44 + r * (SYSTEM_CARD_H + 6)) for r in range(4) for c in range(2)]
SYSTEM_CARD_RADIUS = 12
SYSTEM_TEXT_X = 48            # label and value from the card's left
SYSTEM_TEXT_FONT, SYSTEM_VALUE_FONT = 0, 2
# the network page (the IP card tapped): rows of a label and a wide value, from under the title
NETWORK_ROWS_Y, NETWORK_ROWS_STEP, NETWORK_ROWS_N = 38, 30, 9
NETWORK_LABEL_W = 104

# The boot page's logo -- the mark and the wordmark, boards/nspanel/nextion/assets/generate.py
# logo() -- at its bottom, centred in the visible area, its bottom as far above
# the screen's as the title's capitals below the top (34 px).
BOOT_LOGO_W, BOOT_LOGO_H = 300, 64
BOOT_LOGO = ((VISIBLE_W - BOOT_LOGO_W) // 2, 320 - 34 - BOOT_LOGO_H, BOOT_LOGO_W, BOOT_LOGO_H)   # x, y, w, h


# The media page (page type "media", boards/nspanel/nextion/ohpanel_hmi.py media_page()):
# what plays filling the page -- the art (MEDIA_ART: a box with a large
# icon, or the cover the firmware draws over it), the title, artist and album
# beside it, the controls at the bottom in the middle (centred on the
# visible area), between the favourites' disc (MEDIA_FAV, when the page links
# a favourites page) on the left margin and the volume's on the right one.
#
# The art, the play/pause disc, the previous/next glyphs and both pills are
# drawn by the firmware (in the theme's colours); hotspots over them take the
# touches. Boxes are x, y, w, h; controls are centres, the disc with its
# radius.
MEDIA_ART = (24, 60, 140, 140)
MEDIA_TITLE = (184, 62, 254, 44)        # font 4
MEDIA_SUB = (184, 106, 254, 32)         # font 3: the artist
MEDIA_SOURCE = (184, 142, 254, 26)      # font 2: album or playlist
MEDIA_CONTROLS = (VISIBLE_W // 2 - 95, VISIBLE_W // 2, VISIBLE_W // 2 + 95, 252)   # previous, play, next x; y
# play/pause: a disc in the theme's accent with the symbol on it, both drawn by the firmware
# (Engine::draw_play) -- the display has no picture in a colour set at runtime
MEDIA_PLAY_DISC = 84
# the volume and the favourites: discs (a picture per background, compose.py slots vol, fav), level with
# the controls, the speaker and the star on them
MEDIA_VOLUME = (VISIBLE_W - MARGIN - 48, 228, 48, 48)
MEDIA_FAV = (MARGIN, 228, 48, 48)
ART_RADIUS, PILL_COLOUR = 18, (60, 66, 60)

# The favourites page (page type "favorites") is drawn on the button pages'
# design: its icon (or a station's logo over it) centred above the
# label of each tile -- FAV_ICON, relative to the tile picture.
FAV_ICON = (TILE_INSET + 6, TILE_INSET + 6, TILE_VISIBLE - 12, 62)
# A button (ohpanel_hmi.py buttons_page, Engine::draw_button): its icon top left, its name on the bottom line,
# its state (if it has one) just above the name, and below them -- for a value in % -- a bar (the firmware draws
# it): the same places on every tile, with or without a state or a bar. Relative to the tile picture; 12 px from
# the visible edges.
TILE_PAD = TILE_INSET + 10
TILE_ICON = (TILE_INSET + 4, TILE_INSET + 6, 35, 35)          # its ink 12 px from the left, 11 from the top
TILE_STATE = (TILE_PAD, 50, TILE - TILE_PAD - TILE_INSET - 4, 16)
TILE_NAME = (TILE_PAD, 66, TILE - TILE_PAD - TILE_INSET - 4, 20)
TILE_BAR = (TILE_PAD, 88, TILE - 2 * TILE_PAD, 4)

# The volume page (page "volume", boards/nspanel/nextion/ohpanel_hmi.py volume_page()), opened
# from the media page's pill: the title, the volume large between minus and
# plus, a slider across the page, the mute button where the settings page has
# its restart button (the same pictures).
VOLUME_ROW = (MARGIN, 58, VISIBLE_W - 2 * MARGIN, 64)    # minus, the number (font 5), plus
VOLUME_SLIDER = (MARGIN, 150, VISIBLE_W - 2 * MARGIN, 30)

# The QR page: the area under the title the firmware draws the code in
# (Engine::draw_qr_code: a white card just around the code, the modules as
# rectangles), centred; its caption below in two lines, then the page dots
# (y=288).
QR_AREA_SIZE = 196
QR_AREA = ((VISIBLE_W - QR_AREA_SIZE) // 2, 40, QR_AREA_SIZE, QR_AREA_SIZE)   # x, y, w, h: 127..322, 40..235
QR_CAPTION = [(MARGIN, 238, VISIBLE_W - 2 * MARGIN, 24), (MARGIN, 262, VISIBLE_W - 2 * MARGIN, 24)]   # x, y, w, h


# The light and cover pages (page types "light", "cover"; boards/nspanel/nextion/ohpanel_hmi.py
# control_page()): the title, a large slider upright in the middle (or, on the
# light page, the colour wheel), its icon and value large on its left, and on
# the right margin a column of tiles -- the light's views (brightness, colour
# temperature, colour), the cover's up / stop / down. The tiles are the
# button pages' drawn smaller (generate.py side tiles), inset by 2 px like them.
SIDE_TILE = 86
SIDE_TILES = [(VISIBLE_W - MARGIN - SIDE_TILE + TILE_INSET, 46 + k * 88) for k in range(3)]   # x, y of each picture
POWER_TILE = (MARGIN - TILE_INSET, SIDE_TILES[2][1])   # the light's on / off: bottom left, as the column's last on the right
# what shows of the tiles: from the top of the first to the bottom of the last -- the slider and the wheel the same
CONTROL_TOP = SIDE_TILES[0][1] + TILE_INSET                          # 48
CONTROL_BOTTOM = SIDE_TILES[2][1] + SIDE_TILE - TILE_INSET           # 306
CONTROL_SLIDER_W = 110                                               # wide: a finger finds it
CONTROL_SLIDER = (VISIBLE_W // 2 - CONTROL_SLIDER_W // 2, CONTROL_TOP, CONTROL_SLIDER_W,
                  CONTROL_BOTTOM - CONTROL_TOP)                      # x, y, w, h: on the visible area's middle
CONTROL_HANDLE = 24                                                  # the upright sliders' handle, px along them
# the left column mirrors the tiles on the right: the icon on the first row's middle and the value right under it,
# one group (font 3: "3100 K" fits the tile's width), the on / off tile at the bottom
CONTROL_COLUMN = POWER_TILE[0] + SIDE_TILE // 2                      # its middle, x
CONTROL_ICON = (CONTROL_COLUMN, SIDE_TILES[0][1] + SIDE_TILE // 2)   # the icon's middle
CONTROL_VALUE_W, CONTROL_VALUE_H = SIDE_TILE - 2 * TILE_INSET, 40
CONTROL_VALUE_DY = 38                                                # the value's middle below the icon's
CONTROL_VALUE = (CONTROL_COLUMN - CONTROL_VALUE_W // 2, CONTROL_ICON[1] + CONTROL_VALUE_DY - CONTROL_VALUE_H // 2,
                 CONTROL_VALUE_W, CONTROL_VALUE_H)
CONTROL_VALUE_FONT = 3
COLOUR_WHEEL_D = 250
COLOUR_WHEEL = (VISIBLE_W // 2 - COLOUR_WHEEL_D // 2, (CONTROL_TOP + CONTROL_BOTTOM) // 2 - COLOUR_WHEEL_D // 2,
                COLOUR_WHEEL_D, COLOUR_WHEEL_D)   # x, y, w, h of its picture (generate.py colour_wheel), the same
# the colour chosen, a disc the firmware draws in the colour view -- where the on / off tile is in the others
COLOUR_SWATCH = (POWER_TILE[0] + SIDE_TILE // 2, POWER_TILE[1] + SIDE_TILE // 2, 26)   # centre, radius
SLIDER_HANDLE = 14                           # every slider's handle, px along it

# The cover page (page type "cover" with an item; ohpanel_hmi.py cover_page): on the left the cover drawn by the
# firmware in its style over the page's height (Engine::draw_cover_art: a window with its shutter, an awning seen
# from above, a gate ...) -- dragging on it sets the position --; on the right what to read and what to touch:
# the position large, the state in words and which way it moves, up / stop / down in a row (rounded squares the
# firmware draws, an icon field on each), up to three presets as pills under them.
COVER_DRAW = (14, 44, 150, 246)              # where the firmware draws the cover
COVER_RIGHT = 186                            # the right column's left edge (to RIGHT)
COVER_VALUE = (COVER_RIGHT, 40, 252, 56)     # x, y, w, h: font 5 (48 px)
COVER_STATE = (COVER_RIGHT + 2, 96, 250, 24)
COVER_MOVING_ICON = (COVER_RIGHT + 2, 122, 24, 24)
COVER_MOVING = (COVER_RIGHT + 28, 120, 222, 26)
COVER_BUTTONS = [(COVER_RIGHT + k * 86, 154, 78, 64) for k in range(3)]   # x, y, w, h
COVER_PRESETS = [(COVER_RIGHT + k * 86, 232, 78, 34) for k in range(3)]   # the pills
COVER_PRESET_INSET = 8                       # their label fields inside the round ends

# The thermostat page (page type "thermostat"; ohpanel_hmi.py thermostat_page): a ring in the middle, drawn by the
# firmware (Engine::thermostat_ring: beads along three quarters of a circle, open at the bottom, filled as far as
# the target, a knob at its end) -- dragging on it sets the target --; inside it the temperature now large, the
# target under it and whether it heats or cools; minus and plus left and right of it (discs the firmware draws, an
# icon field on each); the modes as pills in a row under it (the firmware draws them, as many as there are, centred).
THERMO_CENTRE = (VISIBLE_W // 2, 160)        # the ring's middle
THERMO_RADIUS = 112                          # to the beads' middles
THERMO_RING_MARGIN = 18                      # the touch area beyond the beads
THERMO_RING = (THERMO_CENTRE[0] - THERMO_RADIUS - THERMO_RING_MARGIN, THERMO_CENTRE[1] - THERMO_RADIUS -
               THERMO_RING_MARGIN, 2 * (THERMO_RADIUS + THERMO_RING_MARGIN),
               2 * (THERMO_RADIUS + THERMO_RING_MARGIN))   # x, y, w, h: a square around it
# the fields inside the ring: a field repaints its whole box, so each stays clear of the beads (a bead's inner edge
# 104 px from the middle: at 58 px above it 86 px to each side, at 48 px below 92, at 70 px below 77)
THERMO_NOW = (THERMO_CENTRE[0] - 85, THERMO_CENTRE[1] - 58, 170, 76)    # font 6 (72 px)
THERMO_TARGET = (THERMO_CENTRE[0] - 88, THERMO_CENTRE[1] + 22, 176, 26)   # font 2
THERMO_STATE = (THERMO_CENTRE[0] - 75, THERMO_CENTRE[1] + 46, 150, 24)    # drawn by the firmware: icon and words
THERMO_BUTTON_R = 24                         # minus and plus: discs, their middles beside the ring
THERMO_BUTTONS = [(48, THERMO_CENTRE[1]), (VISIBLE_W - 48, THERMO_CENTRE[1])]
THERMO_MODES_Y, THERMO_MODES_H, THERMO_MODES_GAP = 280, 32, 10
THERMO_MODE_W = {1: 120, 2: 110, 3: 96, 4: 90}   # a pill's width by how many there are


def thermo_modes(n: int) -> list:
    """The mode pills when there are n of them: x, y, w, h each, the row centred under the ring."""
    w = THERMO_MODE_W[n]
    left = THERMO_CENTRE[0] - (n * w + (n - 1) * THERMO_MODES_GAP) // 2
    return [(left + k * (w + THERMO_MODES_GAP), THERMO_MODES_Y, w, THERMO_MODES_H) for k in range(n)]


# The cover group page (page type "cover" with a group; ohpanel_hmi.py covers_page): the whole group at the top on
# every sheet, taller, its buttons larger; under it a row per member, four on a sheet: a card (drawn by the
# firmware), a small drawing of the cover, the name, its position, and up / stop / down as small rounded buttons.
COVERS_ALL_Y, COVERS_ALL_H = 40, 58
COVERS_ALL_BUTTON = (92, 46)                 # the whole group's buttons: wide, the row's room beside "all"
COVERS_ROWS_Y, COVERS_ROWS_STEP, COVERS_ROWS_H, COVERS_ROWS_N = 104, 46, 42, 4
COVERS_MINI = (10, 10, 26, 30)               # x, y, w, h in the row
COVERS_TEXT_X, COVERS_TEXT_W = 46, 182       # the name ("Senkrechtmarkise" in font 2: 160 px)
COVERS_VALUE_W = 60                          # its position in % ("100 %": 55 px), right-aligned before the buttons
COVERS_BUTTON, COVERS_BUTTON_GAP, COVERS_BUTTON_END = 34, 8, 8   # the buttons: size, gap, room to the card's end
# the group's presets: three pills in the first row's place (the firmware shows them there when the group has
# presets, its members then three a sheet from the second row on)
COVERS_PRESET_W = (VISIBLE_W - 2 * MARGIN - 2 * 8) // 3
COVERS_PRESETS = [(MARGIN + k * (COVERS_PRESET_W + 8), COVERS_ROWS_Y, COVERS_PRESET_W, COVERS_ROWS_H) for k in range(3)]
