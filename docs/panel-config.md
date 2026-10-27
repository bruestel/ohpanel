# Panel configuration

Everything a panel shows (pages, buttons, entries, the home screen) is one
JSON document, stored on the panel and edited in its web interface (tab
*Panel*). One firmware serves every room; the rooms differ only in this file.
[`demo/config.json`](../demo/config.json) is an example with every page type, the one the
[demo](https://bruestel.github.io/ohpanel/demo/) shows; [openHAB items for the example](openhab-items.md) has the
items it names, as an `.items` file.

States and labels come from openHAB: the panel subscribes to every item the
configuration names and shows openHAB's formatted value (`displayState`: the
item's pattern, MAP transformation or option label, with unit). The
configuration therefore says *which* item goes *where*; how a value is
written can be set as well, see [Formatting values](#formatting-values).

## Conventions

- Every member called `item` is an openHAB item name and is subscribed
  automatically. Item names consist of `A-Z a-z 0-9 _`.
- `icon` is a [Material Design Icons](https://pictogrammers.com/library/mdi/)
  name, e.g. `desk-lamp` (the display font contains all of them; the
  release is in `boards/nspanel/nextion/icon-meta.json`, `tools/update_mdi.py` brings a newer
  one). Names an older release used and the current one keeps as an alias
  still work. Besides MDI the panel has its own icons, all named
  `ohpanel-...` (category "OHpanel" in the icon chooser, `boards/nspanel/nextion/own_icons.py`):
  animations whose frames keep still what does not move
  (`ohpanel-equalizer-1` .. `-8`, `ohpanel-equalizer-retro-1` .. `-8` (a hi-fi
  spectrum display with peak hold), `ohpanel-speaker-wave-1` .. `-3`),
  `ohpanel-openhab` and the keypad's code places (`ohpanel-code-empty`, `ohpanel-code-typed`).
- `color` is a palette name (`white`, `black`, `red`, `green`, `blue`,
  `yellow`, `brown`, `grey`, `light-grey`, `dark-grey`), one of the theme's
  colours (`theme-accent`, `theme-text`, `theme-dim`, which follow the theme
  and the background; not in `display.theme` itself) or `#rrggbb`.
- Page `id`s are 1-24 characters `a-z 0-9 -`; they are what `open` and
  `swipe` refer to.
- Strings shown on the display are cut to what the display field holds.

## Top level

| Member | Type | |
|---|---|---|
| `version` | `1` | required; a document with another version is refused |
| `display` | object | language and formats of the display texts, see below |
| `home` | object | the home screen, see below |
| `swipe` | array of page ids | the pages reached by swiping left/right, in order, round from the last to the first; home, the start screen, is always first, wherever (or whether) the row names it. Only button pages (`buttons`); every other page (entities, weather, QR, media, cover, light, thermostat, keypad, the info and the settings page) opens from a tile or a shortcut |
| `pages` | array of pages | every other page |

## Formatting values

Where a value of an item shows (value rows on home, the info lines, the
rows of an entities page), `format` sets how it is written, and `factor`
scales a number first. Without them the panel shows what openHAB formats.

| `format` | |
|---|---|
| a pattern with `%` | the panel writes the value itself, see below |
| any other text | shown instead of the value, e.g. in a rule: `{"equals": "1", "format": "On"}` |
| `"date"`, `"time"`, `"datetime"` | a Unix timestamp in a Number item as date and/or time, in the formats of `display` (a DateTime item with a pattern in openHAB needs none of this) |

A pattern is written as openHAB's state patterns are:

| Placeholder | writes | 21.456 °C | 12345.6 W |
|---|---|---|---|
| `%.1f` | the number with 1 decimal (`%.0f` rounded, up to `%.6f`; `%f`: 6) | `21,5` | `12345,6` |
| `%d` | the number rounded to a whole one | `21` | `12346` |
| `%,.0f` | a `,` after the `%` groups thousands | `21` | `12.346` |
| `%unit%` | the item's unit from openHAB (of a Quantity; `%` of a Percent item) | `°C` | `W` |
| `%s` | the state as openHAB sends it (`OPEN`, `21.456 °C`) | | |
| `%%` | a percent sign | | |

Anything else is written as it is: `"%.1f %unit%"` → `21,5 °C`,
`"Battery %d%%"` → `Battery 87%`, `"%,.0f W"` → `12.346 W`. The separators
follow `display.language`: German writes `21,5` and `12.346`, English `21.5`
and `12,346`. A `%f` or `%d` of a state that is no number writes openHAB's
text instead. At most 48 characters.

`factor` multiplies the number before it is written: `0.001` with
`"%.2f kW"` shows a power in W as `12,35 kW`. `%unit%` stays the item's unit,
so a pattern names the new one; without a pattern a scaled number shows
without a unit (with `decimals` where there is one).

A value row on home shows a plain number large with its unit small beside it;
with a `format` it shows the whole text instead.

## Display

| Member | Type | |
|---|---|---|
| `language` | `en` (default), `bg`, `cs`, `da`, `de`, `el`, `es`, `et`, `fi`, `fr`, `ga`, `hr`, `hu`, `it`, `lt`, `lv`, `mt`, `nb`, `nl`, `pl`, `pt`, `ro`, `sk`, `sl`, `sv` | language of the texts the panel itself writes (on/off, weekdays, months, "no value", the info, settings, cover and thermostat words) and how it writes numbers: `1,234.5` (en, ga, mt), `1.234,5` (de, da, el, es, hr, it, nl, ro, sl), `1 234,5` (bg, cs, et, fi, fr, hu, lt, lv, nb, pl, pt, sk, sv) |
| `date_format` | string | how dates are written; default: the language's own, short enough for the home screen (`%a, %e %b` "Mon, 5 Oct", `%a, %e. %b` "Mo, 5. Okt.", `%a %e %b` "lun 5 oct", `%a %-d.%-m.` "ma 5.10." ...); e.g. `%a, %e. %b %y` ("Mo, 5. Okt. 26") or `%d/%m/%Y` |
| `time_format` | string | e.g. `%H:%M` (default, "09:35"), `%-H:%M` ("9:35"), `%I:%M %p` ("01:35 PM") or `%-I:%M %p` ("1:35 PM"); with `%p` the display shows AM/PM small above the minutes |
| `home_after` | number | seconds without a touch (on the screen or a button) after which another page goes back to home; not a media page while its player plays (paused, it counts from then); default 60, 0: never; at most 3600 |
| `page_fade` | number | a swipe to another page fades the screen out and in again, that many ms each way (in 4 steps), hiding how the display draws the new page; default 0 (not), at most 1000 |
| `cover_reveal` | string | how the media page's cover appears: `dissolve` (in a random order, the default), `center` (from the middle outwards), `sharpen` (blurred first, 8 x 8 and then 4 x 4 points averaged, then sharp; a third more to draw), `wipe` (row by row from the top) |
| `screen` | object | the screen's brightness, see below |
| `notifications` | `{"item"}` | a String item whose new states show as notifications, see below |
| `background` | background id | the picture behind every page: `aurora` (default), `candy`, `dunes`, `forest`, `black`: the backgrounds of the display design, listed in `boards/nspanel/nextion/display-manifest-<variant>.json` |
| `theme` | `{"accent", "text", "dim"}`, each a colour (see above) | the colours the panel draws with; each one left out is the background's own (Aurora: a light blue accent, white text, light grey dim). `accent` marks what is active or chosen: lit buttons and icons, sliders, the chosen pill, the thermostat's ring, the QR code; `text` is the texts' colour; `dim` the quieter ones: the date, small labels, inactive icons. The web interface's *Display* card picks them, and resets one to the background's |
| `texts` | object | overrides single texts of the language: `on`, `off`, `undefined` (shown for NULL/UNDEF states), the name lists `weekdays`, `weekdays_short` (7, starting with Sunday), `months`, `months_short` (12), and the settings page's `settings_title`, `settings` (its 3 slider labels), `never`, `restart`, `restart_confirm` |

`display.screen` sets how bright the screen is and when it is off:

| Member | Type | |
|---|---|---|
| `brightness` | number or `{"value", "item"}` | % while in use, 1 to 100 (default 85) |
| `idle_brightness` | number or `{"value", "item"}` | % once nothing touched the panel for `idle_after` seconds, 0 to 100 (default 60); 0: off |
| `idle_after` | number or `{"value", "item"}` | seconds, 0 to 3600 (default 60); 0: never idle |
| `night` | `{"from", "to"}` or `{"item"}` | off at night, once idle: by the clock (`"23:00"` .. `"06:00"`, across midnight too) or while a Switch item is ON |

Off (at night, or idle at 0 %), the display shows a black page; a touch on
it only wakes the screen (it presses nothing) and opens home. The two front
buttons switch as always but leave it off. A number with an
`item` (a Dimmer or Number) is kept in step with it: what openHAB says wins,
also after a restart, and is kept in the configuration; a change made here
(the web interface) is sent to the item.

`display.notifications.item` names a String item; each new state of it shows
as a card over the page on screen (the page stands still under it) until a
touch anywhere closes it; the touch does nothing else. The state is
`level|title|text`, `level|text`, `title|text` or just `text`; the level,
`info` (blue, default), `warning` (yellow) or `alert` (red), colours the
card's bar and title. The text wraps; a written `\n` breaks the line. A new
notification wakes the screen; an empty state or NULL takes the card away.
The state the item has when the panel starts shows nothing, and openHAB only
reports changes: to show the same text again, set the item to NULL before.

```
// an openHAB rule (DSL)
Panel_Notify.postUpdate(NULL)
Panel_Notify.sendCommand("warning|Window open|The bathroom window has been open for 30 minutes.")
```

The formats use these placeholders: `%a`/`%A` weekday short/long, `%b`/`%B`
month short/long, `%d` day with two digits, `%e` day, `%m` month as number,
`%y`/`%Y` year with two/four digits, `%H` hour 00-23, `%I` hour 01-12, `%M`
minute, `%p` AM/PM, `%%` a percent sign; a `-` after the `%` drops the leading
zero of a number (`%-H`, `%-I`, `%-d`, `%-m`). They apply to the clock and date on
the home screen, weather dates and fields with `format`. Values openHAB
formats itself (`displayState`) keep openHAB's format.

## Home screen

| Member | Type | |
|---|---|---|
| `relays` | up to 2 relays | what the two hardware buttons do, see below |
| `values` | up to 2 value rows | below the weather: an icon and a value each, `{"item", "icon"?, "color"?, "format"?, "factor"?, "decimals"?, "value_style"?}` (`indoor_temperature: {item}` is the older name of the first, with a house as its icon) |
| `outdoor_temperature` | `{item, decimals?}` | beside the weather icon, large, on the clock's line; a temperature as weather apps show it (`17°`) |
| `outdoor_low`, `outdoor_high` | `{item, decimals?}` | today's low and high under it, on the date's line, each after an arrow (`↓9° ↑22°`; up to `-12°` for the low), e.g. items linked to a weather add-on's daily forecast for today (OpenWeatherMap: `forecastToday#max-temperature` / `#min-temperature`) |
| `weather_icon` | `{item, codes}` | an item holding a weather condition code; `codes` names the code set of the openHAB channel the item is linked to (`boards/nspanel/nextion/weather-icons.json`): `openweathermap` (default; OpenWeatherMap and Open-Meteo `icon-id`: `01d`, `10n`, ...), `openweathermap-condition` (OpenWeatherMap `condition-id`, 200 ... 804), `weathercompany` (`iconCode` 0 ... 47), `meteoblue` (`condition`, pictocode 1 ... 17), `smhi` (`symbol_code`), `fmi` (FMI `weather-id`), `wmo` (Open-Meteo `weather-code`), `darksky` (Pirate Weather `icon-id`, Visual Crossing `icon`). The icons are [Meteocons](https://github.com/basmilius/weather-icons) (fill). |
| `weather_page` | page id | opened by tapping the weather icon |
| `status_icons` | up to 10 indicators | the small icons in the top row |
| `shortcuts` | up to 7 indicators | the large icons; each may `open` a page |
| `info` | up to 2 lines | two text lines below the value rows, see below |

A **relay** is `{"label", "item"?, "relay"?}` and describes one of the two
hardware buttons with the relay next to it:

- without `item`, the button switches the relay directly;
- with `item` (a Switch), the relay is exposed to openHAB through that item:
  the button switches the relay at once and sends `ON`/`OFF` to the item, and
  the item switching (UI, rules, voice) switches the relay. A press while
  openHAB is unreachable still switches the relay; after the reconnect the
  panel sends its state to the item instead of taking the stale one. The item
  needs no binding or Thing; a plain `Switch` item is enough, e.g.
  `Switch Office_Panel_Relay1 "Office light (panel)"`;
- `"relay": false` leaves the relay alone: the button only acts on the item
  (e.g. a smart bulb on a permanently powered circuit).

The relays keep their state through a restart or power cut; with an item,
openHAB's state takes over once the panel is connected. An item that is NULL
or UNDEF (openHAB restarting, before its persistence restores it) switches
nothing: the relay stays as it is and the panel tells openHAB its state.

An **info line** has three fields, `left`, `main` and `right` (e.g. tram line,
destination, departure), each `{"item", "format"?, "factor"?}` or `{"text"}`, the left
one also `{"icon"}`. Each may have a text `color`; the left one may be a
badge, a rounded box in the colour `bg` (e.g. the line number in the line's
colour; a palette colour, another one becomes the nearest of them). A text too
long for its field ends in "...".

Without an item, a value row, the outdoor temperature and the weather icon do
not show.

An **indicator** is `{"icon", "interval"?, "blink"?, "color"?, "item"?, "show"?, "on"?, "open"?, "value_style"?}`
(`value_style`: rules on the item's state that set another icon, colour or blinking, see the entities page).
`blink` (200 to 2000 ms) makes one icon blink: shown and hidden that long each
(the web interface offers 0.25, 0.5, 1, 1.5 and 2 s); not with several icons.
`icon` may be a list of 2 to 8 icons, shown in turn every `interval` ms (200 to
5000, default 500) while the indicator shows: `["equalizer", "poll"]` for a
playing speaker, `["ohpanel-speaker-wave-1", "ohpanel-speaker-wave-2",
"ohpanel-speaker-wave-3"]` (series whose icons keep still what does not move;
`volume-low/-medium/-high` jump, MDI centres each on its own). `show` is
`"always"` (default without an item) or `"when_on"` (default with an item:
visible while the item is on). The item is on when its state is ON, OPEN, PLAY
or a number above 0, or, with `on`, when it is the state given there or one
of a list (`"on": "PLAYING"` for a player's String item, `["PLAYING",
"BUFFERING"]`), which then replace the usual ones. The editor offers both, the
states as chips, and shows how the item's state counts right now. A shortcut with an item opens its page only while the item is
on.

## Pages

Every page has `id`, `type` and `title` (optional for `system`). The types:

### `buttons`: up to 8 buttons

`buttons` is an array of up to 8 entries; `null` leaves a slot empty, so the
others keep their place.

| Member | | |
|---|---|---|
| `label` | string | |
| `icon`, `color` | | |
| `item` | item | the item the button shows and acts on |
| `action` | `toggle`, `command`, `open`, `none` | default: `open` if `open` is set, else `toggle` if there is an item, else `none` |
| `command` | string | sent to the item for `action: command`, e.g. `dock`, `ON`, `50` |
| `open` | page id | opened by the button (cover, light, media, entities, qr ...) |
| `value` | `on_off`, `percent`, `none` | what the state line above the name shows (`percent` with a bar); default by the item, whatever the button does: `percent` for one in % (a dimmer, a rollershutter), `on_off` for a switch or a contact, else `none` |
| `always_active` | bool | draw the button active regardless of the item's state |
| `value_style` | rules (see below) | by the item's state: `"active": true / false` draws the button active or not (else its state decides: `on`, else ON / OPEN / above 0), `"layout": "corner" / "center"` places its icon (else its own `layout`), `color` the icon's colour, `blink`, `icon` another icon, e.g. `[{"above": 65, "active": true, "color": "red"}]`; `always_active` still wins |
| `layout` | string | `corner` (default): the icon top left, the name on the bottom line, the state (`value`) above it and, for a value in %, a bar below; `center`: the icon in the middle of the tile, large (48 px), the name centred under it, no state |

### `entities`: a list of states

Either fixed `rows` or a `group`:

| Member | | |
|---|---|---|
| `rows` | array of rows (`null` = empty line) | |
| `group` | item | an openHAB group; its members become the rows, with their labels from openHAB, and new members appear by themselves |
| `metadata` | string | with `group`: a namespace of the members' item metadata (e.g. `plant`) that names, sorts and hides them and gives the rules their thresholds, see below |
| `label` | string | with `metadata`: the row's label as a template of it, e.g. `{room} > {short_name}` (`{value}` the metadata's value, `{key}` a key of its config, `{label}` the item's label); default: the metadata's value |
| `icon`, `color`, `value_color`, `value_style`, `format`, `factor` | | with `group`: apply to every row |
| `style` | `list`, `cards`, `rows` | how the rows look (default `list`): `list` an icon, the label and the value right-aligned, a thin line between rows (7 a sheet); `cards` two columns of cards, the label small above the value (8 a sheet); `rows` each row on a card of its own (7 a sheet) |
| `wide` | bool | the wide layout (longer values, no labels column; with `cards`: the value without its label) |
| `scroll` | bool | a label too long for its field scrolls instead of ending in "..." (`list` and `rows`; values and wide rows still end in "...": the display scrolls 10 texts a page at most) |

A **row** is `{"item", "label"?, "icon"?, "color"?, "value_color"?, "value_style"?, "format"?, "factor"?, "wide"?}`;
`color` is the icon's, `value_color` the value's (a matching rule's colour comes first);
`"wide": true` puts this row alone in the wide layout (a day's date above its lessons, say);
`"scroll"` decides for this row whether a label too long for its field scrolls (else the page's `scroll` counts).
Without a label the item's openHAB label is used.

`format` and `factor` write the value as [Formatting values](#formatting-values)
says. More rows than fit on the display are shown on further sheets of the
same page: swiping left brings the next, right the one before, and dots at
the bottom show which.

With `group` the rows are the group's direct members, sorted by label.
The panel asks openHAB for them each time the page opens (openHAB tells
nobody when a group gains a member), and their states come like any other
item's.

With `metadata` as well, each member's metadata in that namespace counts:
a namespace of its own rather than one of the panel's, so that other clients
(an app, a rule) can read the same values:

- `order` (a number) sorts the rows; members without come last, by label;
- `hidden=true` leaves the member out (it stays in the group for others);
  without it, the member is shown;
- any other key is there for the `label` template and for the rules'
  thresholds: `"at_most": "@critical"` compares with the member's `critical`;
- a member without metadata there shows its item label, and rules with a
  `@` threshold do not match it.

Plant sensors, for instance, in a `.items` file:

```
Group gPlantsIndoor "Plants indoor"
Number:Dimensionless Plant_Ficus_Moisture "Soil moisture" (gPlantsIndoor)
    { plant="Ficus" [room="Living room", short_name="Ficus", warn=25, critical=20, order=4] }
```

```json
{"id": "plants-indoor", "type": "entities", "title": "Plants indoor", "style": "rows",
 "group": "gPlantsIndoor", "metadata": "plant", "label": "{room} > {short_name}", "icon": "sprout",
 "value_style": [{"at_most": "@critical", "color": "red"}, {"at_most": "@warn", "color": "yellow"},
                 {"color": "green"}]}
```

A rule's `color` is the value's; the row's icon takes a rule's colour only
from its `icon_color`.

`value_style` is a list of **rules**, checked in order, the first match wins:

| Rule | matches when |
|---|---|
| `{"below": n, ...}` | the numeric state is below `n` |
| `{"above": n, ...}` | the numeric state is above `n` |
| `{"at_most": n, ...}` | the numeric state is `n` or below |
| `{"at_least": n, ...}` | the numeric state is `n` or above |
| `{"equals": "s", ...}` | the raw state equals `s` (e.g. `"ON"`, `"OPEN"`) |
| `{"within_days": n, ...}` | a date (DateTime state or Unix timestamp) is at most `n` days from today (0 = today); with `"from": "HH:MM"` only from that time on the first of those days, with `"until": "HH:MM"` only before that time on the date's own day |
| `{...}` | always: the fallback, as last rule |

A bin to put out the evening before and to bring in the morning of: `{"within_days": 1, "from": "17:00", "until": "08:00", "color": "yellow"}` matches from 17:00 the day before until 08:00 on the day itself. The panel looks at such rules again every minute.

`below`, `above`, `at_most`, `at_least` and `within_days` compare numbers: a
rule with a text there is refused when the configuration is saved, except
`"@key"` in the first four, a group member's metadata (see `metadata` above). A state that is no number
(or NULL) matches none of them.

A rule sets what shows while it matches, at least one of:

- `color`: the text's or icon's colour;
- `blink`: 200 to 2000 ms, shown and hidden that long each;
- `icon`: another icon (where the field shows one: a row's icon, a value
  row's icon on home, a status icon or shortcut);
- `icon_color`: the colour of the icon beside the value: a value row's on
  home, an entity page's row (its `color` is the value's);
- `format`: another [format](#formatting-values) for the value (where one
  shows), with the rule's own `factor` (default 1): the field's `format` and
  `factor` do not count while the rule matches.

`value_style` works on an entity row (or the page, with `group`), on a value
row on home (the colour is the value's, number and unit; it blinks with its
icon), on an
info line's field with an `item`, and on a status icon or shortcut, e.g.

```json
{"item": "Window_Office", "icon": "window-closed-variant", "show": "always",
 "value_style": [{"equals": "OPEN", "icon": "window-open-variant", "color": "red", "blink": 500}]}
```

A power in W, as kW from 1000 W on and red from 3000 W on:

```json
{"item": "PV_Power", "icon": "solar-power", "format": "%.0f W",
 "value_style": [{"above": 3000, "format": "%.1f kW", "factor": 0.001, "color": "red"},
                 {"above": 999, "format": "%.1f kW", "factor": 0.001}]}
```

### `cover`

A roller shutter, a blind, an awning, a curtain, a garage door, a gate: one
(`item`) or a group's members as rows (`group`).

| Key | Value | |
|---|---|---|
| `item` | a Rollershutter (or Dimmer) item | the cover |
| `title` | text | optional with `item`: without it the item's metadata's `label` |
| `group` | a Group item | instead of `item`: its members as rows |
| `style` | `shutter` (default), `screen`, `awning`, `curtain`, `garage`, `gate` | how it is drawn, its words and its arrows |
| `reverse` | bool | the item's 0 % is closed / extended (most are the other way round: 0 % open, retracted) |
| `from` | `right` (default), `left` | a gate: the side it comes in from |
| `presets` | up to 3 `{"label", "position"}` | pills at the bottom; a tap sends the position (as shown) |
| `moving` | `{item}` | an item saying `UP`, `DOWN` or `STOP` while it moves |
| `metadata` | namespace | where the items' metadata is read (default `cover`) |
| `commands` | `{"item", "stop", "open", "close"}` | what the tiles send (default `STOP`/`UP`/`DOWN`), and optionally to which item |
| `live` | bool | the position sent while the finger moves on the drawing (default: when let go) |
| `hide_slider` | bool | no drawing (a cover without a position) |

On the left the firmware draws the cover in its style, over the page's
height; on the right the position large, the state in words under it (open /
closed, retracted / extended, ...) and, while it moves, which way ("closing",
lighting its button too; told by `moving`, else by the position changing),
then up / stop / down in a row and the presets under them. The drawing: a window
with its shutter (`shutter`) or a screen with its bar (`screen`) coming down,
an awning seen from above going out over the terrace (`awning`), a curtain
from both sides (`curtain`), a garage door (`garage`), a sliding gate coming
in from the right or the left (`gate`, `from`). Dragging on the drawing sets
the position; a grip in the accent colour marks the moving edge; a jump of
more than 10 points (a binding echoing the command, a device telling its
target first) shows only once it has lasted 2.5 s, and 1 % / 99 % count as the
end.

**Metadata.** What the page leaves out comes from the item's metadata in the
namespace `cover` (or `metadata`; the web interface's form shows what it finds there): `label` (the
title), `style`, `reverse`, `from`, `moving` (an item's name, for a group's members; a page of its own takes `"moving"`),
`command_item` (likewise: a page of its own takes `"commands": {"item"}`), `presets` (as text,
`"Sun=70, Shade=100"`) and, for a group's rows, `label`:

```
Rollershutter Awning_Terrace "Terrace awning" { cover=" " [style="awning", presets="Sun=60, Shade=100"] }
```

**A separate command item.** Some covers take their position on one item
(a Dimmer) and open / stop / close on another (a String item taking `open`,
`stop`, `close`). `commands.item` names that one: up / stop / down go there,
with the words of `commands` as they read (up sends `open`, also with
`reverse`), while dragging and the presets send the position to `item`.

```json
{"id": "curtain", "type": "cover", "item": "Curtain_Position", "style": "curtain",
 "commands": {"item": "Curtain_Command", "open": "open", "stop": "stop", "close": "close"}}
```

On a group page each member names its own in its metadata
(`cover=" " [command_item="Curtain_Command"]`), the words coming from the
page's `commands`; up / stop / down on the whole group then go to each
member on its own instead of to the group.

**A group** (`"group"`) shows the whole group at the top (taller, larger
buttons) and a row per member under it, four on a sheet (swipe for more),
or three when the group has presets (the page's `presets`, else the group item's
metadata, `cover=" " [presets="50 %=50, 75 %=75"]`): their pills take the first
row's place, a tap sends the position to the whole group (lit while the group
is there, if it has a state, e.g. `Group:Rollershutter:AVG`): a small drawing in the member's style (its
metadata's, else the page's `style`), its name (its metadata's `label`, else
its label), the position, and up / stop / down. A tap on a member's row
opens it as a cover page of its own; the close cross comes back.

```json
{"id": "shutters-attic", "type": "cover", "title": "Attic", "group": "gRollershutterAttic"}
```

### `light`

`icon`, `brightness` (`{item}`, Dimmer), optional `color` (`{item}`, Color) and
`color_temperature` (`{item}`) with `unit`, either `kelvin` (by default with an
item in K), `mired` (1,000,000 / K, as Zigbee lamps count: high is warm) or
`percent` (by default otherwise), and `min` / `max`, its ends in that unit:
by default the item's minimum and maximum in openHAB (its state description,
as many bindings set it from the lamp), else 2000 / 6500 K, 153 / 500 mired,
0 / 100 %; set them to narrow the range. The slider runs from warm (bottom) to cold whatever the unit;
`reverse` (bool) turns a percentage round (bindings differ: by default 0 % is
warm). A value in mired shows as Kelvin.

The page shows the brightness large on the left and a slider upright in the
middle, and its on / off as a tile bottom left; it switches the light's
`switch` (`{item}`, a Switch) if it has one, else the brightness item gets
`ON` / `OFF`; while the light is off the tile is dark and the slider grey. In
the colour view the colour chosen shows in the tile's place. With a colour temperature or a colour, tiles on the right choose the
view: the brightness, the colour temperature (a slider on a warm-to-cold
track; an item in K (a Number:Temperature, or above 100) goes from `min`
to `max`, any other is a percentage) or the colour (a colour wheel: a touch
sends hue and saturation as HSB, the brightness kept; it follows the finger
as it is dragged, a ring marks the colour; a light that is off is switched
on first and gets the colour once it reports back on, at its own brightness). Sliders send while they
move (every 300 ms at most) and when let go; `"live": false` only when let go.

### `thermostat`

A room's heating (or cooling): its target temperature set on a ring.

| Key | Value | |
|---|---|---|
| `item` | a Number (Number:Temperature) item | the target temperature: what the ring and minus / plus set |
| `current` | `{item}` | the temperature now, shown large in the middle |
| `heating` | `{item}` | ON (OPEN, above 0) while it heats: "heating" under the target, in orange |
| `cooling` | `{item}` | ... while it cools: "cooling", in blue |
| `mode` | `{item, value_style?}` | its mode (a String, say), chosen with the pills under the ring; `value_style`: rules on its state with `"locked": true`: the target is not to be set in that mode (no minus and plus, the ring not dragged), e.g. `[{"equals": "schedule", "locked": true}]` |
| `modes` | up to 4 `{"label", "value"}` | the pills: what each is called and what it sends to `mode`; without them the mode item's options in openHAB |
| `min`, `max`, `step` | numbers | the target's range and step; by default the item's in openHAB (its state description), else 5, 30, 0.5 |
| `style` | `accent` (default), `gradient` | the ring filled in the theme's accent, or from blue to red |
| `curve` | `0.25`, `0.5` (default), `0.75`, `1`, `1.5`, `2` | the gradient: the colour at a point is its share of the ring to this power: below 1 red comes the sooner, 1 evenly, above 1 the later |

In the middle a ring of beads over three quarters of a circle, open at the
bottom, filled as far as the target with a white knob where it ends; inside
it the temperature now, the target under it ("Target 21.0 °C", as many
decimals as the step) and whether it heats or cools. Dragging on the ring sets
the target (shown at once, sent when let go); minus and plus beside it move it
a step. Under the ring the modes as pills, the one the mode item is in lit in
the accent; with no `mode` the row stays empty. A target sent shows until the
item reports it (6 s at most); a command carries the item's unit ("21.5 °C").

```json
{"id": "thermostat-kitchen", "type": "thermostat", "title": "Kitchen",
 "item": "Thermostat_Kitchen__SetpointTemperature",
 "current": {"item": "Thermostat_Kitchen__Temperature"},
 "heating": {"item": "Thermostat_Kitchen__Heating"},
 "mode": {"item": "Thermostat_Kitchen__OperatingMode"},
 "modes": [{"label": "Auto", "value": "auto"}, {"label": "Manual", "value": "manual"}, {"label": "Off", "value": "off"}]}
```

### `keypad`

A code typed on the panel and sent with a button: an alarm system's modes, a door's "Open".

| Key | Value | |
|---|---|---|
| `buttons` | 1 to 4 buttons, see below | required; left of the keys (EU) or under them (US), larger the fewer there are |
| `item` | an item | where the buttons send to, unless one names its own |
| `state` | `{item, value_style?, format?, icon?, color?}` | shown over the code; its rules give the words (`format`), `icon`, `color` and `blink` |
| `text`, `icon`, `color` | | without a `state`: shown over the code instead, e.g. `"Door code"` with `lock` |
| `length` | 1 to 8 | the code's places (without: one per digit typed, up to 8) |
| `keys` | `digits` (default), `phone` | the bottom row: backspace, 0, C (clears); or `*`, 0, `#` with backspace at the end of the code's places |
| `result` | `{item}` | "Reply from openHAB": a String item a rule writes a text to, e.g. "Wrong code". If it changes within 10 s after sending, its text shows for a few seconds instead of waiting for the state. Never interpreted |

A **button** is `{"label", "icon"?, "item"?, "command"?, "code"?, "when"?, "show_when"?}`:

| Key | |
|---|---|
| `label`, `icon` | on the button (the icon before the label) |
| `item` | where it sends to (default: the page's `item`) |
| `command` | what it sends; `{code}` is the code typed (default `"{code}"`: the code alone) |
| `code` | `false`: works without a code too (else the panel asks for one) |
| `when` | the state it leads to: lit while the state is that; after a press, "Not accepted" if the state does not get there within 6 s |
| `show_when` | a state or a list of them: the button shows only then, e.g. "Off" only while armed. The buttons that show are arranged as if there were no others. Needs `state` |

The code stays on the display until a button sends it, and shows only as
places filling, never as digits. It is forgotten after sending, after 20 s
without a key and when the page closes. The panel checks no code itself;
openHAB (a rule, an alarm binding) decides.

openHAB writes every command to its event log, a code too. A log filter keeps
it out (in `log4j2.xml`, a `RegexFilter` on the item's name in the `event`
logger).

```json
{"id": "alarm", "type": "keypad", "title": "Alarm", "item": "Alarm_Command", "length": 4,
 "state": {"item": "Alarm_State", "value_style": [
   {"equals": "DISARMED", "format": "Disarmed", "icon": "shield-off", "color": "green"},
   {"equals": "ARMED_AWAY", "format": "Armed away", "icon": "shield-lock", "color": "red"},
   {"equals": "ALARM", "format": "Alarm!", "icon": "shield-alert", "color": "red", "blink": 500}]},
 "buttons": [{"label": "Away", "icon": "shield-lock", "command": "ARM_AWAY {code}", "show_when": "DISARMED"},
             {"label": "Home", "icon": "shield-home", "command": "ARM_HOME {code}", "show_when": "DISARMED"},
             {"label": "Off", "icon": "shield-off", "command": "DISARM {code}",
              "show_when": ["ARMED_AWAY", "ARMED_HOME", "ALARM"]}]}
```

A door: one button, the code alone to a String item a rule checks.

```json
{"id": "door", "type": "keypad", "title": "Front door", "text": "Door code", "icon": "lock",
 "buttons": [{"label": "Open", "icon": "lock-open-variant", "item": "FrontDoor_Code"}]}
```

### `media`

A speaker: what plays, its cover, previous / play-pause / next, the volume,
and a star that opens a page of favourites.

| Member | |
|---|---|
| `control` | `{item}`, a Player item: play/pause, previous/next (`PREVIOUS`, `NEXT`) |
| `volume`, `mute` | `{item}` each: the volume button, which opens a card with a slider, minus, plus and the mute; the slider sends while it moves, `"live": false` only when let go |
| `track`, `artist`, `album` | `{item}` each: what plays, in three lines (the album, or the playlist) |
| `art` | `{item}`: the cover, see below |
| `favorites` | the id of a [`media_favorites`](#media_favorites-stations-to-choose-from) page: the star bottom left opens it |
| `play` | `{item}`, with `favorites`: the item a favourite's stream URI goes to, for Sonos one linked to the `playuri` channel |
| `uri` | `{item}`: what plays, as an address, for Sonos `currenttrackuri`. After a favourite's tap the panel learns the stream it plays and shows the station's name whenever that stream plays without a title worth showing: after a pause, another time. A stream is known by its address without the server and the parameters (both change from play to play). Without `uri` the panel goes by the file name a stream tells as its title. Some broadcasters name all their streams alike (`stream.mp3`), which then show no name; `uri` tells them apart |
| `icon` | what the page shows as its picture while nothing plays (default `speaker`) |

A title that is one long word, or one with `?`, `/` or `=` is not shown: a station
without metadata reports its stream's file name (`stream.mp3?cid=...`).

`art` names an item holding the cover: its address (`http://`, `https://`, or
a path on the openHAB server, `/...`, fetched with the panel's token; for
Sonos a String item linked to `currentalbumarturl`) or the picture itself
as a `data:image/jpeg;base64,...` address (an Image item linked to
`currentalbumart`). The address is the lighter choice: openHAB sends an
Image item's every new picture through the state stream, 50 to 150 KB as
text, whether a panel shows it or not. The panel loads the picture when it
changes, decodes it (JPEG; not progressive), takes the square in its middle
and draws it 70 x 70 points large, each point 2 x 2 pixels, with rounded
corners: a few thousand rectangles, after everything else on the page, from
the middle outwards, in under a second. Until then, and without a cover, the
icon shows.

```json
{"id": "speaker", "type": "media", "title": "Kitchen",
 "control": {"item": "kitchenSpeakerControl"}, "volume": {"item": "kitchenSpeakerVolume"},
 "mute": {"item": "kitchenSpeakerMute"}, "track": {"item": "kitchenSpeakerCurrentTitle"},
 "artist": {"item": "kitchenSpeakerCurrentArtist"}, "album": {"item": "kitchenSpeakerCurrentAlbum"},
 "art": {"item": "kitchenSpeakerCurrentAlbumArtUrl"}, "favorites": "radio",
 "play": {"item": "kitchenSpeakerPlayUri"}}
```

### `media_favorites`: stations to choose from

A station catalogue as tiles, on the button pages' design: eight on a sheet,
more on further sheets (swiped), each with an icon (or the station's logo)
above its short name. A tap sends the station to the play item (`play`)
of a player and shows that player: the player whose star opened the page,
else the page's `player`, else the one media page linking it. So players in
several rooms share one catalogue, each playing on its own speaker. The
player shows the station's name as its title until the speaker tells a title
worth showing; a stream often tells none, or only its address, so the name
may stay. A button page can open the page too (`"open"`).

| Member | |
|---|---|
| `stations` | `{"item"}`: an item whose options (its commandDescription, or stateDescription) are the catalogue, `stream URI=key`, a key per station, `a-z 0-9` |
| `player` | the id of the media page a tap plays on when the page was not opened from a player |

The panel fetches the catalogue once openHAB is connected and keeps it;
openHAB tells nobody when options or metadata change, so the panel asks
again each time the page opens, showing what it has at once and redrawing
only when something changed.

What the panel shows of a station comes from the catalogue item's `ohpanel`
metadata, `stations`: a JSON object, per station's key any of

| Key | |
|---|---|
| `name` | its name, the player's title until the speaker tells its own (default: the key) |
| `short` | the name on its tile (default: `name`), about 10 characters fit |
| `icon` | an icon (default `radio`) |
| `logo` | a logo, shown instead of the icon (below) |

In an `.items` file the JSON is a string like any other: its quotes escaped
(`\"`), lines may break inside it:

```
// openHAB: the catalogue (no channel, no rule)
String RadioStations "Radio stations" {
    commandDescription=" " [options="x-rincon-mp3radio://radio.example/one.m3u=one,x-rincon-mp3radio://radio.example/news.m3u=news"],
    ohpanel=" " [stations="{
        \"one\": {\"name\": \"Radio One\", \"short\": \"One\", \"logo\": \"ATg4DybkQMPrgIHr...\"},
        \"news\": {\"name\": \"News Radio\", \"short\": \"News\", \"icon\": \"newspaper-variant-outline\"}
    }"]}
```

In the Main UI's code view of the item's `ohpanel` metadata it may be YAML
(`config: stations: one: name: Radio One ...`); the panel takes both.

A logo comes ready to draw, as a code made from a picture in the web
interface (the favourites page's form, "Stations: names, icons and logos…",
where stations are also added, removed, ordered and their keys and URIs
changed: a picture dropped in, shown as the panel will draw it, and the
command options and the metadata to copy into openHAB, as an `.items` file
writes them or for the Main UI's code view), or with `tools/station_logo.py` for scripts. Either cuts the
logo out (a plain background taken away, or `--circle` for a round logo, or
`--disc "#rrggbb"` to put a one-colour logo in white on a disc), scales it
to 56 px and reduces its colours; the panel only draws its rectangles,
laying translucent edges on the tile's colour. A code is a few KB; a logo
with fewer colours (`--colours 8`) is shorter and drawn faster.
`--preview out.png` shows how it will look on the tiles.

```
tools/station_logo.py logo.jpg --circle --preview logo-preview.png
```

```json
{"id": "radio", "type": "media_favorites", "title": "Radio",
 "stations": {"item": "RadioStations"}, "player": "speaker"}
```

### `weather`

The days of the forecast, a sheet each: swiped left and right, dots at the
bottom. A sheet shows the day and its date as the title, the condition's icon,
the high large with the low under it, and up to six cards of an extra (an
icon, the value, a small label: wind, rain, UV, ...).

`days` is a list of 1 to 7 days, each:

| Key | Value | |
|---|---|---|
| `day` | `0` (today) to `6` | which day it is: its date |
| `title` | text, up to 24 characters | default: Today, Tomorrow, then the weekday (in the display's language) |
| `icon` | `{item, codes}` | the condition code, as for the home screen |
| `max`, `min` | `{item}` | the high and the low, shown as degrees ("13,4°") |
| `condition` | `{item}` | the condition in words ("light rain"), under the temperatures |
| `extras` | up to 6 `{"icon", "item", "label"?, "color"?}` | the cards; `color` is the icon's |

```json
{"id": "weather", "type": "weather", "title": "Weather", "days": [
  {"day": 0, "icon": {"item": "weatherTodayConditionIconId", "codes": "openweathermap"},
   "max": {"item": "weatherTodayMaxTemperature"}, "min": {"item": "weatherTodayMinTemperature"},
   "extras": [{"icon": "weather-windy-variant", "item": "weatherTodayWindSpeed", "label": "Wind"}]},
  {"day": 1, "icon": {"item": "weatherTomorrowConditionIconId", "codes": "openweathermap"}}]}
```

A page of one day in the old form (`day`, `icon`, `min`, `max`, `condition`, `extras` on
the page itself) still works; the web interface turns it into `days` on
its first change. The home screen's `weather_page` opens the first day.

### `qr`

A QR code on a white card, drawn by the panel itself, with a caption of up to two lines under it.

| Key | Value | |
|---|---|---|
| `text` | up to 200 characters | what the code holds: a link, a text, or a Wi-Fi login (`WIFI:T:WPA;S:guest;P:secret;;`; the web interface writes it from the network's name and password) |
| `caption` | text, or a list of 2 texts | under the code, one line or two, e.g. `["Wi-Fi: {ssid}", "Password: {password}"]`; `{ssid}` and `{password}` show the network and password of a Wi-Fi login |
| `color` | a colour (see above) | the code's colour (default: the theme's accent; dark colours scan best) |
| `style` | `solid` (default), `dots` | `dots`: the modules apart (the three corner squares stay whole) |
| `ecc` | `low`, `medium` (default), `quartile`, `high` | error correction: higher survives more damage, with smaller modules |

```json
{"id": "guests", "type": "qr", "title": "Guest Wi-Fi", "caption": ["Wi-Fi: {ssid}", "Password: {password}"],
 "text": "WIFI:T:WPA;S:guest;P:secret;;", "style": "dots"}
```

### `system`

The info page is built in, like the settings page: always there with the id
`info`, never deleted; a page `{"id": "info", "type": "system", "title"}` in
the configuration only gives it another title (default "Panel info" /
"Panel-Info"). Reach it from a tile or a shortcut (the swipe row takes
button pages only). Eight cards, refreshed every second while it is shown:
an icon, a small label and the value each (a value too long for its line in
two smaller lines): the firmware, the display design (or that it is
outdated), the uptime, the free memory, the PSRAM, the WiFi network (its
signal as a small icon), the IP address and openHAB's host (protocol and port
with the label). The labels follow the language and can be replaced with
`display.texts.system` (8 strings, in this order).

With an IPv6 address the IP card shows the IPv4 address above it and the IPv6
address below (shortened in the middle when it does not fit; on an IPv6-only
network the IPv6 address alone). Tapping the IP card opens the network page
over the info page: the IPv4 address with its prefix, the gateway, the IPv6
addresses (global, unique local, link-local), the IPv6 router on a network
without IPv4, the DNS servers in the order the panel asks them, the time
server and the panel's .local name. The back arrow in its title, or the close
cross, goes back to the info page.

### The settings page

Built in: always there with the id `settings`, never configured (no page may
use that id); reach it from a button or a shortcut's `open` (the swipe row
takes button pages only). The screen's settings on the panel itself: sliders for the brightness in use
and when idle and for the time until idle (`display.screen`), and a restart
button that asks for a second tap. What is set there is kept like a change in
the web interface, and sent to a linked item. Its title is "Settings" /
"Einstellungen" (`display.texts.settings_title`).
