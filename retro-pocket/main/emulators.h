#pragma once

/* Every "system" the launcher knows about. To add one, write a
 * void my_run(const char *path) and add a row in launcher.c. */
void emu_gb_run(const char *path);
/* internal: the Game Boy Color core (emu_gbc.c); takes ownership of rom */
#include <stddef.h>
#include <stdint.h>
void emu_gbc_run(const char *path, uint8_t *rom, size_t size);
size_t gb_cart_ram_size(const uint8_t *rom, size_t size);
void emu_nes_run(const char *path);
void emu_sms_run(const char *path);   /* Master System, Game Gear, SG-1000 */
void emu_pce_run(const char *path);   /* PC Engine / TurboGrafx-16 */
void game_maze_run(const char *unused);

#define GB_PALETTE_COUNT 5
extern const char *const gb_palette_names[GB_PALETTE_COUNT];
