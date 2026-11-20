#!/bin/sh
# Builds the PC simulator: ./build.sh && SIM_SCRIPT=demo.txt ./retro-pocket-sim
cd "$(dirname "$0")"
M=../main
cc -O2 -std=gnu11 -DSD_MOUNT='"simsd"' -Iinclude -I$M -o retro-pocket-sim sim.c \
   $M/launcher.c $M/system.c $M/gfx.c $M/emu_gb.c $M/emu_nes.c $M/game_maze.c -lm "$@"
