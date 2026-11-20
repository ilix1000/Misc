#!/bin/sh
# Builds a release and refreshes firmware/ so devices can update over Wi-Fi.
#   tools/release.sh            (needs ESP-IDF exported)
# Bump FW_VERSION in main/version.h first; devices only update to a higher version.
set -e
cd "$(dirname "$0")/.."
idf.py build
mkdir -p firmware/ota
cp build/retro_pocket.bin firmware/ota/retro-pocket.bin
sed -n 's/#define FW_VERSION "\(.*\)"/\1/p' main/version.h > firmware/ota/version.txt
(cd build && esptool.py --chip esp32s3 merge_bin -o ../firmware/retro-pocket-esp32s3.bin @flash_args)
echo "Release $(cat firmware/ota/version.txt) ready. Commit and push firmware/ to publish it."
