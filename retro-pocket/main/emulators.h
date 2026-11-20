#pragma once

/* Every "system" the launcher knows about. To add one, write a
 * void my_run(const char *path) and add a row in launcher.c. */
void emu_gb_run(const char *path);
void emu_nes_run(const char *path);
void game_maze_run(const char *unused);

#define GB_PALETTE_COUNT 5
extern const char *const gb_palette_names[GB_PALETTE_COUNT];
