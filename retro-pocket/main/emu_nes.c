/*
 * emu_nes.c - NES front-end around agnes (MIT, cores/agnes.c).
 * Supports mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM) and 4 (MMC3), which covers
 * a large part of the library.
 *
 * 256x240 is scaled to 160x128 with 2x2 averaging so details don't vanish
 * when pixels are dropped.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "emulators.h"
#include "i18n.h"
#include "romscan.h"
#include "system.h"
#include "bridge.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"

/* Pull the core in directly so we can read the PPU's index buffer instead of
 * calling agnes_get_screen_pixel() 61,440 times per frame, keep its hot
 * code in IRAM and its state (~23 KB) in fast internal RAM. */
#define AGNES_HOT IRAM_ATTR
static void *agnes_alloc(size_t n)
{
    /* internal RAM only if Wi-Fi and the web server keep enough of it */
    void *p = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > n + 48 * 1024)
        p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void *screen_alloc(size_t n)
{
    /* written once per line, read once per frame: PSRAM is fine */
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
#define AGNES_SCREEN_ALLOC(n) screen_alloc(n)
#define AGNES_SCREEN_FREE(p) free(p)
#define malloc(n) agnes_alloc(n)
#include "cores/agnes.c"
#undef malloc

#define NES_FRAME_US 16639 /* 60.1 Hz */
#define MAX_SKIP 3          /* show at least every 4th frame */

static void blit(const uint8_t *src, uint16_t *fb);

/* ---------------- display on the other core ----------------
 * While core 1 emulates the next frame, core 0 scales the finished one and
 * sends it to the panel. The two NES screen buffers are swapped, not copied. */

static int s_shown, s_shown_fps;
static int64_t s_fps_t0;

static void show_frame(const uint8_t *src)
{
    uint16_t *fb = display_fb();
    blit(src, fb);
    s_shown++;
    int64_t now = esp_timer_get_time();
    if (now - s_fps_t0 >= 1000000) { s_shown_fps = s_shown; s_shown = 0; s_fps_t0 = now; }
    if (g_settings.show_fps) draw_fps(fb, s_shown_fps);
    display_present();
}

#ifdef SIM
static bool present_async(const uint8_t *frame) { show_frame(frame); return true; }
static void present_wait(void) {}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_work, s_idle;
static const uint8_t *volatile s_job;

static void blit_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_work, portMAX_DELAY);
        show_frame(s_job);
        xSemaphoreGive(s_idle);
    }
}

/* Hands a finished frame to core 0 if it's free (then the caller swaps
 * buffers). Returns false, dropping the frame, if core 0 is still busy. */
static bool present_async(const uint8_t *frame)
{
    if (!s_work) {
        s_work = xSemaphoreCreateBinary();
        s_idle = xSemaphoreCreateBinary();
        xSemaphoreGive(s_idle);
        xTaskCreatePinnedToCore(blit_task, "nes_blit", 4096, NULL, 6, NULL, 0);
    }
    if (xSemaphoreTake(s_idle, 0) != pdTRUE) return false;
    s_job = frame;
    xSemaphoreGive(s_work);
    return true;
}

/* Before anything else draws (menus, exit): let core 0 finish. */
static void present_wait(void)
{
    if (!s_idle) return;
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
    display_wait();
}
#endif

static uint16_t s_pal[64];      /* unswapped RGB565, for blending */
static uint16_t s_xmap[LCD_MAX_DIM];
static uint16_t s_ymap[LCD_MAX_DIM];
static int s_ox, s_oy, s_dw, s_dh; /* where the picture goes on screen */

static void build_maps(int trim)
{
    /* trim = 1 hides the 8 px overscan border TVs never showed. */
    int sx0 = trim ? 8 : 0, sw = trim ? 240 : 256;
    int sy0 = trim ? 8 : 0, sh = trim ? 224 : 240;
    if (LCD_WIDTH >= LCD_HEIGHT) {
        /* landscape: fill the screen */
        s_dw = LCD_WIDTH;
        s_dh = LCD_HEIGHT;
    } else {
        /* portrait: full width, keep the aspect ratio, black bars */
        s_dw = LCD_WIDTH;
        s_dh = sh * s_dw / sw;
        if (s_dh > LCD_HEIGHT) s_dh = LCD_HEIGHT;
    }
    s_ox = (LCD_WIDTH - s_dw) / 2;
    s_oy = (LCD_HEIGHT - s_dh) / 2;
    for (int x = 0; x < s_dw; x++) s_xmap[x] = sx0 + x * sw / s_dw;
    for (int y = 0; y < s_dh; y++) s_ymap[y] = sy0 + y * sh / s_dh;
    for (int i = 0; i < 64; i++)
        s_pal[i] = RGB565(g_colors[i].r, g_colors[i].g, g_colors[i].b);
}

#define BLEND(a, b) ((uint16_t)((((a) & 0xF7DE) >> 1) + (((b) & 0xF7DE) >> 1)))

static void IRAM_ATTR blit(const uint8_t *src, uint16_t *fb)
{
    if (s_oy > 0) {
        gfx_rect(fb, 0, 0, LCD_WIDTH, s_oy, C_BLACK);
        gfx_rect(fb, 0, s_oy + s_dh, LCD_WIDTH, LCD_HEIGHT - s_oy - s_dh, C_BLACK);
    }
    for (int y = 0; y < s_dh; y++) {
        const uint8_t *r0 = src + s_ymap[y] * AGNES_SCREEN_WIDTH;
        const uint8_t *r1 = r0 + (s_ymap[y] < 239 ? AGNES_SCREEN_WIDTH : 0);
        uint16_t *dst = fb + (s_oy + y) * LCD_WIDTH + s_ox;
        for (int x = 0; x < s_dw; x++) {
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
    uint8_t *rom = rom_load(path, &size);
    if (!rom) { message_box("Can't read ROM", NULL, 0); return; }

    agnes_t *nes = agnes_make();
    if (!nes) { free(rom); message_box("Out of memory", NULL, 0); return; }
    if (!agnes_load_ines_data(nes, rom, size)) {
        char m[32];
        snprintf(m, sizeof(m), T("Mapper %d"), size >= 8 ? ((rom[6] >> 4) | (rom[7] & 0xF0)) : -1);
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
    /* second screen buffer, so core 0 can show one while we draw the next */
    uint8_t *bufs[2] = { nes->ppu.screen_buffer, heap_caps_malloc(AGNES_SCREEN_WIDTH * AGNES_SCREEN_HEIGHT, MALLOC_CAP_SPIRAM) };
    int cur = 0;
    pacer_t pacer;
    pacer_start(&pacer);
    s_fps_t0 = esp_timer_get_time();
    int skipped = 0;
    bool skip = false;

    for (;;) {
        uint32_t k = input_poll();
        if (bridge_quit_requested()) break;
        if (input_pressed() & BTN_MENU) {
            present_wait();
            ingame_action_t a = ingame_menu("NES", true);
            if (a == MENU_QUIT) break;
            if (a == MENU_RESET) {
                /* agnes has no reset(): reload the cartridge, keep battery RAM */
                static uint8_t keep[BATTERY_SIZE];
                if (bram) memcpy(keep, bram, BATTERY_SIZE);
                uint8_t *screen = nes->ppu.screen_buffer;
                memset(nes, 0, sizeof(*nes));     /* (keeps our screen buffer) */
                nes->ppu.screen_buffer = screen;
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
        agnes_set_skip_render(skip);
        if (!agnes_next_frame_fast(nes)) {
            present_wait();
            message_box("CPU crashed", "(bad opcode)", 0);
            break;
        }
        if (!skip && bufs[1] && present_async(bufs[cur])) {
            cur ^= 1;
            nes->ppu.screen_buffer = bufs[cur];
        } else if (!skip && !bufs[1]) {
            show_frame(bufs[0]);            /* no PSRAM for a 2nd buffer */
        }
        /* Game logic always runs at 60 Hz; if we fall behind, the next
         * frames aren't drawn (up to MAX_SKIP in a row). */
        bool behind = pacer_wait(&pacer, NES_FRAME_US);
        skip = behind && skipped < MAX_SKIP;
        skipped = skip ? skipped + 1 : 0;
    }

    present_wait();
    agnes_set_skip_render(false);
    if (bram) file_save(sav, bram, BATTERY_SIZE);
    nes->ppu.screen_buffer = bufs[0];   /* agnes_destroy frees this one */
    free(bufs[1]);
    agnes_destroy(nes);
    free(rom);
}
