/*
 * emu_gb.c - Game Boy / Game Boy Color.
 *
 * Original Game Boy games run on Peanut-GB (MIT, cores/peanut_gb.h), the most
 * accurate core. Games marked as Color (header byte 0x143 bit 7, e.g. GTA 2)
 * go to the Color-capable fork in emu_gbc.c.
 *
 * 160x144 is squeezed to 160x128 by dropping 1 line in 9 (nearest
 * neighbour), so the full picture stays visible.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#include "cores/peanut_gb.h"
/* Peanut-GB's LCD_* names clash with ours (board.h). */
#undef LCD_WIDTH
#undef LCD_HEIGHT
#define LCD_HEIGHT_GB 144

#include "emulators.h"
#include "romscan.h"
#include "system.h"
#include "bridge.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define GB_FRAME_US 16743 /* 59.73 Hz */
#define GB_CGB 0
#define GB_STATE_MAGIC 0x47425331 /* "GBS1" */
#define GB_RUN_DECL static void gb_dmg_run

const char *const gb_palette_names[GB_PALETTE_COUNT] = {
    "Green", "Grey", "Pocket", "Warm", "Color",
};

/* RGB888 values: OBJ0, OBJ1, BG, lightest to darkest. */
const uint32_t gb_palettes[GB_PALETTE_COUNT][3][4] = {
#define SAME(a, b, c, d) { { a, b, c, d }, { a, b, c, d }, { a, b, c, d } }
    SAME(0xC4D49C, 0x8CA45C, 0x4C6C3C, 0x1C2C1C), /* Classic green */
    SAME(0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000), /* Grey */
    SAME(0xE8E8D8, 0xA8A898, 0x686858, 0x282820), /* Pocket */
    SAME(0xFFF0D0, 0xE0A060, 0x905040, 0x302020), /* Warm */
#undef SAME
    { { 0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000 },  /* Color pop: sprites 0 red */
      { 0xFFFFFF, 0x7BFF31, 0x008400, 0x000000 },  /* sprites 1 green */
      { 0xFFFFFF, 0x63A5FF, 0x0000FF, 0x000000 } },/* background blue */
};


/* Cartridge RAM size from the ROM header (works for both cores). */
size_t gb_cart_ram_size(const uint8_t *rom, size_t size)
{
    static const uint32_t sizes[] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };
    if (size <= 0x149) return 0;
    uint8_t type = rom[0x147];
    if (type == 0x05 || type == 0x06) return 0x200;   /* MBC2 built-in RAM */
    return rom[0x149] < 6 ? sizes[rom[0x149]] : 0;
}

#include "emu_gb_impl.h"

void emu_gb_run(const char *path)
{
    size_t size;
    uint8_t *rom = rom_load(path, &size);
    if (!rom) { message_box("Can't read ROM", NULL, 0); return; }
    /* 0x80 = Color-enhanced, 0xC0 = Color-only: both play in colour */
    if (size > 0x143 && (rom[0x143] & 0x80)) emu_gbc_run(path, rom, size);
    else gb_dmg_run(path, rom, size);
}
