#!/bin/sh
# Builds a release and refreshes firmware/ so devices can update over Wi-Fi.
#   tools/release.sh            (needs ESP-IDF exported)
# Bump FW_VERSION in main/version.h first; devices only update to a higher version.
# Also builds the "joy3-14" variant (joystick on GPIO 3 / 14), which updates
# from its own folder firmware/ota-joy3-14.
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/#define FW_VERSION "\(.*\)"/\1/p' main/version.h)
idf.py build
mkdir -p firmware/ota
cp build/retro_pocket.bin firmware/ota/retro-pocket.bin
echo "$VER" > firmware/ota/version.txt
(cd build && esptool.py --chip esp32s3 merge_bin -o ../firmware/retro-pocket-esp32s3.bin @flash_args)

idf.py -B build-joy3-14 -DJOY_ALT=1 build
mkdir -p firmware/ota-joy3-14
cp build-joy3-14/retro_pocket.bin firmware/ota-joy3-14/retro-pocket.bin
echo "$VER" > firmware/ota-joy3-14/version.txt
(cd build-joy3-14 && esptool.py --chip esp32s3 merge_bin -o ../firmware/retro-pocket-esp32s3-joy3-14.bin @flash_args)
echo "Release $VER ready (standard + joy3-14). Commit and push firmware/ to publish it."
