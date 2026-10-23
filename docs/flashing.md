# Flashing

The first installation goes over the serial port; afterwards everything comes
over WiFi from the web interface (Settings > Firmware): the firmware (with the
icon set embedded) and the display firmware.

A serial flash is needed again only when the partition table changes
(`boards/nspanel/partitions.csv`). That last happened on 17 October 2026, when the panel
configuration moved from a LittleFS partition into an NVS partition of its own and the
firmware slots grew to 1.94 MB (see below).

## What goes on the flash

| Offset | Part | From |
|---|---|---|
| `0x1000` | bootloader | ESP-IDF |
| `0x8000` | partition table | `boards/nspanel/partitions.csv` |
| `0xe000` | OTA data (boot the first slot) | ESP-IDF |
| `0x10000` | firmware, the icon set embedded | the build |

The panel configuration lives in `panel` (`0x3F0000`, 32 KB, deflated), WiFi, passwords,
openHAB and the https keys in `nvs` (`0x9000`); the core dump in `coredump` (`0x3F8000`,
32 KB). Written one by one, these parts leave `nvs` and `panel` as they are; the merged file
below does not: it fills the gaps between the parts with `0xFF`, `nvs` included.

## Into the bootloader

Connect a 3.3 V USB serial adapter to the panel's programming pads, hold GPIO0
to GND and switch the power on; then GPIO0 may be released. The bootloader
waits for the flash; it does not start the firmware by itself afterwards, so
switch the power off and on again.

## With PlatformIO

    pio run -e nspanel -t upload        # the NSPanel EU
    pio run -e nspanel-us -t upload     # the NSPanel US

writes every part of the table. The US firmware's files are in `.pio/build/nspanel-us/` and are named
`ohpanel-nspanel-us-...`; the rest of this page names the EU's.

## With esptool alone

Every build also writes the parts merged into one file,
`.pio/build/nspanel/ohpanel-nspanel-eu-factory-<version>.bin`, to be written at offset 0:

    esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
        --before no_reset --after no_reset \
        write_flash 0x0 ohpanel-nspanel-eu-factory-<version>.bin

Coming from other firmware (Tasmota, ESPHome, the Arduino firmware), erase the
flash first (`esptool ... erase_flash`): settings in a different format are
not taken over.

## From an older partition table

OTA cannot change the partition table. Do not update a panel with an older table
over WiFi: the new firmware would run with the default configuration and could not
save one (the log says "No panel partition"). Once over serial:

1. Maintenance > Backup: download a backup with the panel configuration (and the
   passwords, to be safe).
2. Flash the parts one by one, not the merged file, which erases `nvs` too
   (WiFi, passwords, openHAB, the https keys):

        cd .pio/build/nspanel
        esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
            --before no_reset --after no_reset write_flash \
            0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 ota_data_initial.bin 0x10000 firmware.bin

3. The panel comes back on its network (`nvs` is where it was) with the default
   configuration: Maintenance > Backup > Restore brings the panel configuration
   back. (Was `nvs` erased after all, the panel opens its hotspot, an open
   network named like the panel (also in the serial log, "Access point"), and
   the backup with the passwords restores WiFi and openHAB as well.)

The tables before:

- **until 17 October 2026:** the configuration on a 256 KB LittleFS partition
  (`storage`), 1.81 MB firmware slots, a 64 KB core dump;
- **until October 2026:** the icon set and a recovery display firmware in
  partitions of their own and 1.5 MB firmware slots, too small for this
  firmware: an update over WiFi is refused ("image larger than the OTA slot").

## After the first start

Without stored WiFi the panel opens its hotspot, an open network named like
the panel (`ohpanel-` and the end of its MAC address); the display shows its
name and a QR code to join it. The web interface at `http://192.168.4.1` sets
up WiFi and openHAB, behind the web password if one is set.

The hotspot also opens when the stored network stays out of reach for about
half a minute, and by hand: hold the right button for 15 seconds. The panel
then leaves its network for 10 minutes and the display shows the hotspot;
joined again, the hotspot closes two minutes later, or at once with a short
press on either button (which switches its relay as always). A press in the
hotspot that opened without a network switches the relay and restarts the
panel. Holding both buttons for
15 seconds is the factory reset, the same as the web interface's: everything
stored goes (WiFi, web password, device name and settings, openHAB, the
panel configuration) and the panel restarts into the hotspot.

The display itself still runs whatever design it had. Write the panel's
(`ohpanel-nspanel-eu-display-<version>.tft`, built by `boards/nspanel/nextion/build.sh`) from
Settings > Firmware > Display firmware. The version in the name is the design's: while the display runs
another one (or a design that is not OHpanel's), every page of the web interface says so and names the file
it needs; not in the hotspot.

## Updates

A firmware update over WiFi (Settings > Firmware,
`ohpanel-nspanel-eu-firmware-<version>.bin`, next to the factory image in
`.pio/build/nspanel/`) replaces only the firmware (the icon set is part of it). If the new firmware was built for another display design, the web
interface says so on every page and names the file: write
`ohpanel-nspanel-eu-display-<version>.tft` of the same release under Settings >
Firmware > Display firmware.

A panel takes only its own variant's firmware: an NSPanel EU refuses `ohpanel-nspanel-us-firmware-...` and the other
way round. Both have the same hardware, so to try one variant's design on the other panel the API takes it all the
same with `any_variant=1` (not in the web interface; firmware that is not OHpanel's is still refused):

    curl -u <user>:<password> --data-binary @ohpanel-nspanel-us-firmware-<version>.bin \
        "https://<panel>/api/v1/ota?any_variant=1"

then write that variant's display design. Back the same way.
