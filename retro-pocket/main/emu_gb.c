/*
 * emu_gb.c - Game Boy front-end around Peanut-GB (MIT, cores/peanut_gb.h).
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
#include "system.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define GB_FRAME_US 16743 /* 59.73 Hz */

typedef struct {
    uint8_t *rom;
    size_t rom_size;
    uint8_t *cart_ram;
    size_t cart_ram_size;
    bool ram_dirty;
    uint16_t *fb;
    uint16_t pal[3][4]; /* [OBJ0, OBJ1, BG][shade] */
} gb_priv_t;

const char *const gb_palette_names[GB_PALETTE_COUNT] = {
    "Green", "Grey", "Pocket", "Warm", "Color",
};

/* RGB888 values: OBJ0, OBJ1, BG, lightest to darkest. */
static const uint32_t palettes[GB_PALETTE_COUNT][3][4] = {
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

static gb_priv_t s;
static uint8_t s_line_dst[LCD_HEIGHT_GB];

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    return addr < s.rom_size ? s.rom[addr] : 0xFF;
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    return addr < s.cart_ram_size ? s.cart_ram[addr] : 0xFF;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    if (addr < s.cart_ram_size && s.cart_ram[addr] != val) {
        s.cart_ram[addr] = val;
        s.ram_dirty = true;
    }
}

static void gb_err(struct gb_s *gb, const enum gb_error_e e, const uint16_t addr)
{
    /* Bad opcode / bus access: keep running, most games recover. */
}

static void IRAM_ATTR draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    if (line >= LCD_HEIGHT_GB) return;
    int y = s_line_dst[line];
    if (y == 0xFF) return;
    uint16_t *dst = s.fb + y * LCD_WIDTH;
    for (int x = 0; x < 160; x++) {
        uint8_t p = pixels[x];
        int sub = (p >> 4) & 3;
        if (sub > 2) sub = 2;
        dst[x] = s.pal[sub][p & 3];
    }
}

static void load_palette(int idx)
{
    if (idx < 0 || idx >= GB_PALETTE_COUNT) idx = 0;
    for (int sub = 0; sub < 3; sub++)
        for (int i = 0; i < 4; i++) {
            uint32_t c = palettes[idx][sub][i];
            s.pal[sub][i] = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        }
}

static void save_sram(const char *rom_path)
{
    if (!s.cart_ram_size || !s.ram_dirty) return;
    char path[300];
    save_path(path, sizeof(path), rom_path, "gb", ".sav");
    if (file_save(path, s.cart_ram, s.cart_ram_size)) s.ram_dirty = false;
}

/* Save states are the whole emulator struct + cart RAM. Function pointers are
 * restored from the live struct so they stay valid across firmware builds. */
static bool state_save(struct gb_s *gb, const char *rom_path)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "gb", ".state");
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t magic = 0x47425331; /* "GBS1" */
    bool ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(gb, sizeof(*gb), 1, f) == 1;
    if (ok && s.cart_ram_size) ok = fwrite(s.cart_ram, s.cart_ram_size, 1, f) == 1;
    fclose(f);
    return ok;
}

static bool state_load(struct gb_s *gb, const char *rom_path)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "gb", ".state");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct gb_s *tmp = heap_caps_malloc(sizeof(*tmp), MALLOC_CAP_SPIRAM);
    uint32_t magic = 0;
    bool ok = tmp && fread(&magic, 4, 1, f) == 1 && magic == 0x47425331 &&
              fread(tmp, sizeof(*tmp), 1, f) == 1;
    if (ok && s.cart_ram_size) ok = fread(s.cart_ram, s.cart_ram_size, 1, f) == 1;
    fclose(f);
    if (ok) {
        tmp->gb_rom_read = gb->gb_rom_read;
        tmp->gb_cart_ram_read = gb->gb_cart_ram_read;
        tmp->gb_cart_ram_write = gb->gb_cart_ram_write;
        tmp->gb_error = gb->gb_error;
        tmp->gb_serial_tx = gb->gb_serial_tx;
        tmp->gb_serial_rx = gb->gb_serial_rx;
        tmp->gb_bootrom_read = gb->gb_bootrom_read;
        tmp->display.lcd_draw_line = gb->display.lcd_draw_line;
        tmp->direct.priv = gb->direct.priv;
        memcpy(gb, tmp, sizeof(*gb));
        s.ram_dirty = true;
    }
    free(tmp);
    return ok;
}

void emu_gb_run(const char *path)
{
    memset(&s, 0, sizeof(s));
    s.rom = file_load(path, &s.rom_size);
    if (!s.rom) { message_box("Can't read ROM", NULL, 0); return; }

    struct gb_s *gb = heap_caps_malloc(sizeof(struct gb_s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gb) { free(s.rom); message_box("Out of memory", NULL, 0); return; }

    enum gb_init_error_e err = gb_init(gb, rom_read, cart_ram_read, cart_ram_write, gb_err, &s);
    if (err != GB_INIT_NO_ERROR) {
        free(gb);
        free(s.rom);
        message_box(err == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Unsupported cartridge" : "Bad ROM checksum",
                    "(GB Color-only games won't run)", 0);
        return;
    }

    size_t ram_size = 0;
    if (gb_get_save_size_s(gb, &ram_size) == 0 && ram_size > 0) {
        s.cart_ram = heap_caps_calloc(1, ram_size, MALLOC_CAP_SPIRAM);
        s.cart_ram_size = s.cart_ram ? ram_size : 0;
        char sp[300];
        save_path(sp, sizeof(sp), path, "gb", ".sav");
        if (s.cart_ram) file_read_into(sp, s.cart_ram, s.cart_ram_size);
    }

    gb_init_lcd(gb, draw_line);
    load_palette(g_settings.gb_palette);
    for (int i = 0; i < LCD_HEIGHT_GB; i++) s_line_dst[i] = 0xFF;
    for (int y = 0; y < LCD_HEIGHT; y++) s_line_dst[y * LCD_HEIGHT_GB / LCD_HEIGHT] = y;

    char title[17];
    gb_get_rom_name(gb, title);

    pacer_t pacer;
    pacer_start(&pacer);
    int64_t sram_t = esp_timer_get_time();
    bool skip = false;

    for (;;) {
        uint32_t keys = input_poll();
        if (input_pressed() & BTN_MENU) {
            ingame_action_t a = ingame_menu(title[0] ? title : "Game Boy", true);
            if (a == MENU_QUIT) break;
            if (a == MENU_RESET) gb_reset(gb);
            if (a == MENU_SAVE_STATE)
                message_box(state_save(gb, path) ? "State saved" : "Save failed", NULL, 700);
            if (a == MENU_LOAD_STATE)
                message_box(state_load(gb, path) ? "State loaded" : "No saved state", NULL, 700);
            load_palette(g_settings.gb_palette);
            pacer_start(&pacer);
            continue;
        }

        uint8_t pad = 0xFF; /* active low */
        if (keys & BTN_A) pad &= ~JOYPAD_A;
        if (keys & BTN_B) pad &= ~JOYPAD_B;
        if (keys & BTN_START) pad &= ~JOYPAD_START;
        if (keys & BTN_SELECT) pad &= ~JOYPAD_SELECT;
        if (keys & BTN_UP) pad &= ~JOYPAD_UP;
        if (keys & BTN_DOWN) pad &= ~JOYPAD_DOWN;
        if (keys & BTN_LEFT) pad &= ~JOYPAD_LEFT;
        if (keys & BTN_RIGHT) pad &= ~JOYPAD_RIGHT;
        gb->direct.joypad = pad;

        s.fb = display_fb();
        gb_run_frame(gb);
        if (!skip) {
            if (g_settings.show_fps) draw_fps(s.fb, pacer.fps);
            display_present();
        }

        int64_t now = esp_timer_get_time();
        if (s.ram_dirty && now - sram_t >= 30000000) { save_sram(path); sram_t = now; }

        skip = pacer_wait(&pacer, GB_FRAME_US) && !skip; /* never skip 2 in a row */
    }

    save_sram(path);
    display_wait();
    free(s.cart_ram);
    free(gb);
    free(s.rom);
}
