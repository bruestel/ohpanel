# Third-party notices

This project is licensed under the Apache License 2.0; see [LICENSE](LICENSE).

The **firmware images** are not only this source. Building links in the ESP-IDF
framework and the components listed below, so a released `.bin` carries other
people's code, under their licences. This file records what and whose.

One component is vendored into the repository: Nayuki's QR code generator, in
`components/qrcodegen` with its licence. The others are fetched at build time by
the IDF component manager, pinned by `dependencies.lock`, and the framework comes
from the PlatformIO platform pinned in `platformio.ini`.

## What the images contain

| Component | Version | Licence | Source |
| :--- | :--- | :--- | :--- |
| ESP-IDF | 6.0.1 | Apache-2.0 | [espressif/esp-idf](https://github.com/espressif/esp-idf) |
| FreeRTOS kernel | 10.5.1 | MIT | bundled with ESP-IDF |
| lwIP | 2.2.0 | BSD-3-Clause | bundled with ESP-IDF |
| Mbed TLS | 4.0.0 | Apache-2.0 | bundled with ESP-IDF |
| Wi-Fi and PHY libraries | with ESP-IDF 6.0.1 | Apache-2.0 (see below) | [espressif/esp32-wifi-lib](https://github.com/espressif/esp32-wifi-lib) |
| cJSON | 1.7.19~2 | MIT | [espressif/cjson](https://components.espressif.com/components/espressif/cjson) |
| mDNS | 1.14.0 | Apache-2.0 | [espressif/mdns](https://components.espressif.com/components/espressif/mdns) |
| QR Code generator library (C), Copyright Project Nayuki | vendored (`components/qrcodegen`) | MIT (`components/qrcodegen/LICENSE`) | [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) |

**Mbed TLS** is offered under Apache-2.0 *or* GPL-2.0-or-later; the Apache-2.0
option applies here.

**The Wi-Fi and PHY libraries** are shipped by Espressif as pre-compiled
binaries under Apache-2.0. Their source is not published, so they cannot be
rebuilt from source the way everything else here can.

## Tooling

`boards/nspanel/nextion/tools/nhw/` contains two modules of
[newmatik/nextion-hmi-writer](https://github.com/newmatik/nextion-hmi-writer)
(MIT, Copyright 2026 Newmatik GmbH, licence in that directory). They are build
tools for the display design and not part of the firmware.

## Display firmware

The display image (`build/ohpanel-nspanel-<variant>-display-<version>.tft`, built by
`boards/nspanel/nextion/build.sh`) is compiled by the Nextion Editor and contains, besides this project's pages
and pictures:

| Part | Licence | Source |
| :--- | :--- | :--- |
| Nextion display runtime (operating code of the display, added by the Nextion Editor 1.68.1 when compiling) | proprietary, ITEAD Studio | [Nextion Editor](https://nextion.tech/nextion-editor/) |
| Fonts 16 .. 72 px, named "Ubuntu derivative OHpanel" as the licence asks of a converted font, rendered from Ubuntu Regular 0.83, Copyright 2011 Canonical Ltd. (`boards/nspanel/nextion/assets/fonts/Ubuntu-R.ttf`) | Ubuntu Font Licence 1.0 (`boards/nspanel/nextion/assets/fonts/UFL.txt`) | [Ubuntu font](https://design.ubuntu.com/font) |
| Icon fonts `icons24` and `icons32`: Material Design Icons, and Latin-1 from Noto Sans Regular 2.015, Copyright 2022 The Noto Project Authors (`boards/nspanel/nextion/assets/fonts/NotoSans-Regular.ttf`) | Apache 2.0 (Material Design Icons), SIL Open Font License 1.1 (Noto Sans, `boards/nspanel/nextion/assets/fonts/OFL.txt`) | [Material Design Icons](https://pictogrammers.com/library/mdi/), [Noto](https://github.com/notofonts/latin-greek-cyrillic) |
| Weather icons: Meteocons by Bas Milius, "fill" style of release 2.0.0 (the static SVGs), rendered per background (`boards/nspanel/nextion/assets/meteocons`) | MIT (`boards/nspanel/nextion/assets/meteocons/LICENSE`) | [basmilius/weather-icons](https://github.com/basmilius/weather-icons) |
| Own pictures: backgrounds, button tiles, badges and their variants per background (`boards/nspanel/nextion/assets`) | Apache 2.0 (this project) | |

The fonts are made
by `boards/nspanel/nextion/fonts.py` from the TrueType fonts in `boards/nspanel/nextion/assets/fonts` and
compiled into the display image as bitmaps.

## Display design

The display design is this project's own: built by
`boards/nspanel/nextion/ohpanel_hmi.py` on an empty project of the Nextion Editor
(`boards/nspanel/nextion/seed`, made by `boards/nspanel/nextion/tools/make-seed.sh`), with
the pictures of `boards/nspanel/nextion/assets`. Its pages were inspired by
[Blackymas/NSPanel_HA_Blueprint](https://github.com/Blackymas/NSPanel_HA_Blueprint).

## Icons

[Material Design Icons](https://pictogrammers.com/library/mdi/) by
Pictogrammers (Austin Andrews and contributors), Apache License 2.0:

- the web interface embeds the paths of its menu icons (`www/index.html`; the
  openHAB entry's is the openHAB logo, see `ohpanel-openhab` below);
- the panel's icon set (embedded in the firmware by
  `tools/gen_display_assets.py`) carries the table of icon names and code
  points (`boards/nspanel/nextion/icons.json`) and a 24 px picture of every icon, drawn from
  the MDI font in `boards/nspanel/nextion/assets/fonts/materialdesignicons-webfont.ttf`
  (Apache License 2.0, `boards/nspanel/nextion/assets/fonts/MDI-LICENSE.txt`), and the
  icons' categories and aliases (`boards/nspanel/nextion/icon-meta.json`, from MDI's
  `meta.json`); the web interface draws its icons from it;
- the display design's icon fonts are drawn from the same MDI font
  (`boards/nspanel/nextion/fonts.py`).

## OHpanel's own icons

The icons named `ohpanel-...` (`boards/nspanel/nextion/own_icons.py`, `boards/nspanel/nextion/assets/icons/`)
are this project's, under its Apache License 2.0, with two exceptions:

- `ohpanel-speaker-wave-1` .. `-3` are MDI's `volume-high`
  (Material Design Icons, Apache License 2.0), split into its speaker and
  waves;
- `ohpanel-openhab` is the openHAB logo (from
  [openhab-docs](https://github.com/openhab/openhab-docs), `openhab-logo-square.svg`),
  in one colour so the display can draw it as an icon. "openHAB" and the
  openHAB logo are trademarks of the openHAB Foundation e.V.; the icon only
  refers to openHAB, the software the panel works with. OHpanel is not
  affiliated with or endorsed by the openHAB Foundation (see below).

## The demo and the flasher (GitHub Pages)

The demo (`tools/build_demo.py`) serves the web interface and the firmware's icon set (Material Design Icons,
Apache License 2.0, see above) with invented data; the flasher serves the factory image of the latest release.
Both publish the licences of what they serve under `licences/`. The flasher loads
[ESP Web Tools](https://github.com/esphome/esp-web-tools) (Apache License 2.0) from unpkg.com in the browser;
it is not part of this repository or of any image.

## Logo typeface

The wordmark of the logo (`logo/`) is set in
[Sora](https://github.com/sora-xor/sora-font), Copyright 2019 The Sora Project
Authors, under the SIL Open Font License 1.1. The font file and the licence are
in `boards/nspanel/nextion/assets/fonts/`; the build renders it into pictures and outlines, so
no firmware image contains the font itself.

## Name and colours

OHpanel is a community project and is not affiliated with or endorsed by the
openHAB project or the openHAB Foundation. "openHAB" is a trademark of the
openHAB Foundation e.V. The logo borrows only openHAB's brand colour; its
shapes are our own.
