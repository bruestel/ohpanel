# Crash analysis

When the firmware crashes (panic, watchdog), the ESP32 writes a core dump to
its `coredump` partition before it restarts: the state of the crashed task and
seven more at the moment of the crash (the partition is 32 KB; the crashed task
always comes first). It stays there until it is erased, through any number of
reboots.

## On the panel

**Diagnostics** in the web interface shows the crash in one line,
read from the dump on the device:

    Crash in task "display": LoadProhibited (EXCCAUSE 28) at 0x400d6f2e,
    backtrace 0x400d6f2b 0x400d71c4 0x400e0a11. Firmware: the running one;
    decode with build/elf/*-74068cc91.elf.

The same data is in the raw JSON below it (`crash`), and in the serial log at
boot. The addresses mean nothing without the firmware's `.elf`. The hash at
the end names it.

**Download core dump** saves `coredump.bin`; **Erase core dump** makes room
for the next one (a new crash overwrites an old dump anyway).

## The .elf

A dump can only be decoded with the `.elf` of exactly the build that crashed.
Every build keeps it (`tools/archive_elf.py`):

    build/elf/<version>-<elf sha256>.elf

The version is the one the web interface shows. The hash is the start of the
ELF's SHA-256, which the firmware carries in its app descriptor and the core
dump records, so `crash.elf_sha256` points at exactly one file. Keep the files
of every build you put on a panel. `build/` is not under version control, so
they are lost with the checkout.

## Decoding

The backtrace alone, from the line above:

    ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line \
        -pfiaC -e build/elf/<version>-74068cc91.elf 0x400d6f2b 0x400d71c4 0x400e0a11

The whole dump (all tasks, their stacks, registers and variables):

    ~/.platformio/penv/bin/esp-coredump info_corefile \
        --gdb ~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32-elf-gdb \
        --core coredump.bin build/elf/<version>-74068cc91.elf

`dbg_corefile` with the same arguments instead of `info_corefile` opens gdb
on the dump.
