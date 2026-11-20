/*
 * emu_nes.c - NES front-end around agnes (MIT, cores/agnes.c).
 * Supports mappers 0 (NROM), 1 (MMC1), 2 (UxROM) and 4 (MMC3), which covers
 * a large part of the library.
 *
 * 256x240 is scaled to 160x128 with 2x2 averaging so details don't vanish
 * when pixels are dropped.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "emulators.h"
#include "system.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

/* Pull the core in directly so we can read the PPU's index buffer instead of
 * calling agnes_get_screen_pixel() 61,440 times per frame. */
/* ...and keep the emulator state (~95 KB, touched every PPU dot) in fast
 * internal RAM instead of PSRAM. */
static void *agnes_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
#define malloc(n) agnes_alloc(n)
#include "cores/agnes.c"
#undef malloc

#define NES_FRAME_US 16639 /* 60.1 Hz */

static uint16_t s_pal[64];      /* unswapped RGB565, for blending */
static uint16_t s_xmap[LCD_WIDTH];
static uint16_t s_ymap[LCD_HEIGHT];

static void build_maps(int trim)
{
    /* trim = 1 hides the 8 px overscan border TVs never showed. */
    int sx0 = trim ? 8 : 0, sw = trim ? 240 : 256;
    int sy0 = trim ? 8 : 0, sh = trim ? 224 : 240;
    for (int x = 0; x < LCD_WIDTH; x++) s_xmap[x] = sx0 + x * sw / LCD_WIDTH;
    for (int y = 0; y < LCD_HEIGHT; y++) s_ymap[y] = sy0 + y * sh / LCD_HEIGHT;
    for (int i = 0; i < 64; i++)
        s_pal[i] = RGB565(g_colors[i].r, g_colors[i].g, g_colors[i].b);
}

#define BLEND(a, b) ((uint16_t)((((a) & 0xF7DE) >> 1) + (((b) & 0xF7DE) >> 1)))

static void IRAM_ATTR blit(const agnes_t *nes, uint16_t *fb)
{
    const uint8_t *src = nes->ppu.screen_buffer;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        const uint8_t *r0 = src + s_ymap[y] * AGNES_SCREEN_WIDTH;
        const uint8_t *r1 = r0 + (s_ymap[y] < 239 ? AGNES_SCREEN_WIDTH : 0);
        uint16_t *dst = fb + y * LCD_WIDTH;
        for (int x = 0; x < LCD_WIDTH; x++) {
            int sx = s_xmap[x], sx1 = sx < 255 ? sx + 1 : sx;
            uint16_t top = BLEND(s_pal[r0[sx] & 63], s_pal[r0[sx1] & 63]);
            uint16_t bot = BLEND(s_pal[r1[sx] & 63], s_pal[r1[sx1] & 63]);
            dst[x] = SWAP16(BLEND(top, bot));
        }
    }
}

/* Battery-backed PRG RAM (Zelda-style saves) lives in the MMC1/MMC3 mapper. */
static uint8_t *battery_ram(agnes_t *nes, const uint8_t *rom)
{
    if (!(rom[6] & 0x02)) return NULL;
    if (nes->gamepack.mapper == 1) return nes->mapper.m1.prg_ram;
    if (nes->gamepack.mapper == 4) return nes->mapper.m4.prg_ram;
    return NULL;
}
#define BATTERY_SIZE (8 * 1024)

static bool state_io(agnes_t *nes, const char *rom_path, bool save)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "nes", ".state");
    size_t sz = agnes_state_size();
    agnes_state_t *st = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!st) return false;
    bool ok;
    if (save) {
        agnes_dump_state(nes, st);
        ok = file_save(path, st, sz);
    } else {
        ok = file_read_into(path, st, sz) && agnes_restore_state(nes, st);
    }
    free(st);
    return ok;
}

void emu_nes_run(const char *path)
{
    size_t size;
    uint8_t *rom = file_load(path, &size);
    if (!rom) { message_box("Can't read ROM", NULL, 0); return; }

    agnes_t *nes = agnes_make();
    if (!nes) { free(rom); message_box("Out of memory", NULL, 0); return; }
    if (!agnes_load_ines_data(nes, rom, size)) {
        char m[32];
        snprintf(m, sizeof(m), "Mapper %d", size >= 8 ? ((rom[6] >> 4) | (rom[7] & 0xF0)) : -1);
        agnes_destroy(nes);
        free(rom);
        message_box("Unsupported ROM", m, 0);
        return;
    }

    char sav[300];
    save_path(sav, sizeof(sav), path, "nes", ".sav");
    uint8_t *bram = battery_ram(nes, rom);
    if (bram) file_read_into(sav, bram, BATTERY_SIZE);

    build_maps(g_settings.nes_scale);
    pacer_t pacer;
    pacer_start(&pacer);
    bool skip = false;

    for (;;) {
        uint32_t k = input_poll();
        if (input_pressed() & BTN_MENU) {
            ingame_action_t a = ingame_menu("NES", true);
            if (a == MENU_QUIT) break;
            if (a == MENU_RESET) {
                /* agnes has no reset(): reload the cartridge, keep battery RAM */
                static uint8_t keep[BATTERY_SIZE];
                if (bram) memcpy(keep, bram, BATTERY_SIZE);
                memset(nes, 0, sizeof(*nes));
                memset(nes->ram, 0xff, sizeof(nes->ram));
                agnes_load_ines_data(nes, rom, size);
                bram = battery_ram(nes, rom);
                if (bram) memcpy(bram, keep, BATTERY_SIZE);
            }
            if (a == MENU_SAVE_STATE)
                message_box(state_io(nes, path, true) ? "State saved" : "Save failed", NULL, 700);
            if (a == MENU_LOAD_STATE)
                message_box(state_io(nes, path, false) ? "State loaded" : "No saved state", NULL, 700);
            pacer_start(&pacer);
            continue;
        }

        agnes_input_t in = {
            .a = k & BTN_A, .b = k & BTN_B, .start = k & BTN_START, .select = k & BTN_SELECT,
            .up = k & BTN_UP, .down = k & BTN_DOWN, .left = k & BTN_LEFT, .right = k & BTN_RIGHT,
        };
        agnes_set_input(nes, &in, NULL);
        if (!agnes_next_frame(nes)) {
            message_box("CPU crashed", "(bad opcode)", 0);
            break;
        }
        if (!skip) {
            uint16_t *fb = display_fb();
            blit(nes, fb);
            if (g_settings.show_fps) draw_fps(fb, pacer.fps);
            display_present();
        }
        skip = pacer_wait(&pacer, NES_FRAME_US) && !skip;
    }

    if (bram) file_save(sav, bram, BATTERY_SIZE);
    display_wait();
    agnes_destroy(nes);
    free(rom);
}
