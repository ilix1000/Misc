#!/bin/sh
# Builds the PC simulator: ./build.sh && SIM_SCRIPT=demo.txt ./retro-pocket-sim
cd "$(dirname "$0")"
M=../main
cc -O2 -std=gnu11 -DSIM -pthread -DSD_MOUNT='"simsd"' -Iinclude -I$M -o retro-pocket-sim sim.c \
   $M/launcher.c $M/romscan.c $M/unzip.c $M/ui.c $M/bridge.c $M/cores/qrcodegen.c sim_net.c $M/system.c $M/gfx.c $M/emu_gb.c $M/emu_nes.c $M/game_maze.c -lm "$@"
