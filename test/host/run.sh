#!/bin/sh
# Builds and runs the host tests. cJSON comes from the managed component the
# firmware build fetched, so run a firmware build once first.
set -e
cd "$(dirname "$0")/../.."
CJSON=managed_components/espressif__cjson/cJSON
FLAGS="-std=gnu++20 -Wall -Wextra -g -fsanitize=address,undefined -I$CJSON"
mkdir -p test/host/build

gcc -c -g -fsanitize=address,undefined "$CJSON/cJSON.c" -o test/host/build/cJSON.o
gcc -c -g -fsanitize=address,undefined components/qrcodegen/src/qrcodegen.c -Icomponents/qrcodegen/include \
    -o test/host/build/qrcodegen.o

g++ $FLAGS -Icomponents/app_openhab/src -Icomponents/app_openhab/include -Icomponents/app_config/include \
    test/host/openhab_test.cpp components/app_openhab/src/States.cpp test/host/build/cJSON.o \
    -o test/host/build/openhab_test
g++ $FLAGS -Icomponents/panel/include \
    test/host/schema_test.cpp components/panel/src/Schema.cpp test/host/build/cJSON.o \
    -o test/host/build/schema_test

g++ $FLAGS -Icomponents/nextion/include \
    test/host/nextion_test.cpp components/nextion/src/Protocol.cpp -o test/host/build/nextion_test

g++ $FLAGS -Icomponents/app_display/include -Icomponents/app_openhab/include -Icomponents/app_config/include -Iboards/nspanel/board \
    test/host/format_test.cpp components/app_display/src/Format.cpp components/app_display/src/Icons.cpp \
    test/host/build/cJSON.o -o test/host/build/format_test
python3 tools/gen_display_assets.py --raw test/host/build/icons.bin
g++ $FLAGS -Icomponents/app_display/include -Icomponents/app_openhab/include -Icomponents/app_config/include \
    -Icomponents/nextion/include -Icomponents/qrcodegen/include \
    test/host/engine_test.cpp components/app_display/src/Engine.cpp components/app_display/src/Engine[A-Z]*.cpp components/app_display/src/Format.cpp \
    components/app_display/src/Logo.cpp components/app_display/src/Cover.cpp \
    components/app_display/src/Icons.cpp components/nextion/src/Protocol.cpp test/host/build/cJSON.o test/host/build/qrcodegen.o \
    -o test/host/build/engine_test

test/host/build/openhab_test > /dev/null && echo "openhab: all checks passed"
test/host/build/format_test test/host/build/icons.bin
test/host/build/engine_test test/host/build/icons.bin
test/host/build/nextion_test
test/host/build/schema_test
python3 tools/check_names.py
