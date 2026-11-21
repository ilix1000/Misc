#!/bin/sh
# Builds the PC simulator: ./build.sh && SIM_SCRIPT=demo.txt ./retro-pocket-sim
cd "$(dirname "$0")"
M=../main
C=../components
CORES="$C/smsplus/*.c $C/smsplus/cpu/*.c $C/smsplus/sound/*.c $C/pcego/*.c"
cc -O2 -std=gnu11 -DSIM -DRETRO_GO=1 -include esp_attr.h -pthread -DSD_MOUNT='"simsd"' -Iinclude -I$M -I$C/smsplus -I$C/smsplus/cpu -I$C/smsplus/sound -I$C/pcego -I$C/rg_compat/include -o retro-pocket-sim sim.c \
   $M/launcher.c $M/romscan.c $M/unzip.c $M/ui.c $M/bridge.c $M/cores/qrcodegen.c sim_net.c $M/system.c $M/gfx.c $M/emu_gb.c $M/emu_gbc.c $M/emu_nes.c $M/emu_sms.c $M/emu_pce.c $M/video.c $CORES $M/game_maze.c -lm "$@"
