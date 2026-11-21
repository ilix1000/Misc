/*
 * emu_sms.c - Sega Master System, Game Gear and SG-1000, on SMS Plus
 * (GPLv2, Charles MacDonald; ESP32 version from Retro-Go).
 *
 * Controls: A = button 2, B = button 1, START = Pause (Master System) or
 * Start (Game Gear). Their pads only have a d-pad and two buttons, so they
 * fit this handheld exactly.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "emulators.h"
#include "romscan.h"
#include "system.h"
#include "bridge.h"
#include "display.h"
#include "input.h"
#include "unzip.h"
#include "video.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

/* Last: SMS Plus defines macros called input/option/bitmap/cart. */
#define RETRO_GO 1            /* use rg_system.h (our stand-in), not its fallbacks */
#include "shared.h"

#define SMS_FRAME_US_NTSC 16683   /* 59.94 Hz */
#define SMS_FRAME_US_PAL  20000
#define MAX_SKIP 3

static uint8_t *s_buf[2];
static uint16_t s_pal[256];

static bool state_io(const char *rom_path, bool save)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "sms", ".state");
    FILE *f = fopen(path, save ? "wb" : "rb");
    if (!f) return false;
    if (save) system_save_state(f);
    else system_load_state(f);
    fclose(f);
    return true;
}

static void sram_io(const char *rom_path, bool save)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "sms", ".sav");
    if (save) {
        if (sms.save) file_save(path, cart.sram, 0x8000);   /* only if the game used it */
    } else {
        file_read_into(path, cart.sram, 0x8000);
    }
}

void emu_sms_run(const char *path)
{
    size_t size;
    uint8_t *raw = rom_load(path, &size);
    if (!raw) { message_box("Can't read ROM", NULL, 0); return; }

    /* The core wants whole 16 KB pages; skip a 512-byte copier header. */
    size_t skip = (size > 0x4000 && ((size / 512) & 1)) ? 512 : 0;
    size_t data_size = size - skip;
    size_t buf_size = (data_size + 0x3FFF) & ~0x3FFF;
    uint8_t *rom = heap_caps_calloc(1, buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom) { free(raw); message_box("Out of memory", NULL, 0); return; }
    memcpy(rom, raw + skip, data_size);
    free(raw);

    for (int i = 0; i < 2; i++)
        if (!s_buf[i]) s_buf[i] = heap_caps_malloc(SMS_WIDTH * SMS_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf[0] || !s_buf[1]) { free(rom); message_box("Out of memory", NULL, 0); return; }

    system_reset_config();
    option.sndrate = 8000;      /* no speaker: lowest rate the core allows */
    option.overscan = 0;
    option.extra_gg = 0;
    option.tms_pal = 0;
    option.console = has_ext(path, ".sg") ? 5 : 0;

    if (!load_rom(rom, buf_size, data_size)) {
        free(rom);
        message_box("Can't load this ROM", NULL, 0);
        return;
    }
    /* Game Gear files without the standard header would be detected as
     * Master System games. */
    if (has_ext(path, ".gg") && sms.console < CONSOLE_GG) sms.console = CONSOLE_GG;

    int cur = 0;
    bitmap.width = SMS_WIDTH;
    bitmap.height = SMS_HEIGHT;
    bitmap.pitch = SMS_WIDTH;
    bitmap.data = s_buf[cur];

    system_poweron();
    sram_io(path, false);

    int vx = -1, vy = -1, vw = -1, vh = -1;
    const char *title = IS_GG ? "Game Gear" : "Master System";
    const int frame_us = sms.display == DISPLAY_NTSC ? SMS_FRAME_US_NTSC : SMS_FRAME_US_PAL;
    pacer_t pacer;
    pacer_start(&pacer);
    int skipped = 0;
    bool skip_draw = false;

    for (;;) {
        uint32_t k = input_poll();
        if (bridge_quit_requested()) break;
        if (input_pressed() & BTN_MENU) {
            video_wait();
            ingame_action_t a = ingame_menu(title, true);
            if (a == MENU_QUIT) break;
            if (a == MENU_RESET) system_reset();
            if (a == MENU_SAVE_STATE)
                message_box(state_io(path, true) ? "State saved" : "Save failed", NULL, 700);
            if (a == MENU_LOAD_STATE)
                message_box(state_io(path, false) ? "State loaded" : "No saved state", NULL, 700);
            pacer_start(&pacer);
            vw = -1;                 /* screen was used by the menu: redo layout */
            continue;
        }

        input.pad[0] = input.pad[1] = input.system = 0;
        if (k & BTN_UP)    input.pad[0] |= INPUT_UP;
        if (k & BTN_DOWN)  input.pad[0] |= INPUT_DOWN;
        if (k & BTN_LEFT)  input.pad[0] |= INPUT_LEFT;
        if (k & BTN_RIGHT) input.pad[0] |= INPUT_RIGHT;
        if (k & BTN_A)     input.pad[0] |= INPUT_BUTTON2;
        if (k & BTN_B)     input.pad[0] |= INPUT_BUTTON1;
        if (IS_GG) {
            if (k & BTN_START)  input.system |= INPUT_START;
            if (k & BTN_SELECT) input.system |= INPUT_PAUSE;
        } else {
            if (k & BTN_START)  input.system |= INPUT_PAUSE;
            if (k & BTN_SELECT) input.system |= INPUT_START;
        }

        system_frame(skip_draw);

        if (!skip_draw) {
            if (bitmap.viewport.x != vx || bitmap.viewport.y != vy ||
                bitmap.viewport.w != vw || bitmap.viewport.h != vh) {
                video_wait();
                vx = bitmap.viewport.x; vy = bitmap.viewport.y;
                vw = bitmap.viewport.w; vh = bitmap.viewport.h;
                video_setup(vw, vh);
            }
            render_copy_palette(s_pal);
            const uint8_t *frame = s_buf[cur] + vy * bitmap.pitch + vx;
            if (video_submit(frame, bitmap.pitch, s_pal)) {
                cur ^= 1;            /* draw the next frame into the other buffer */
                bitmap.data = s_buf[cur];
            }
        }
        bool behind = pacer_wait(&pacer, frame_us);
        skip_draw = behind && skipped < MAX_SKIP;
        skipped = skip_draw ? skipped + 1 : 0;
    }

    video_wait();
    sram_io(path, true);
    system_shutdown();
    free(rom);
    cart.rom = NULL;
    cart.loaded = 0;
}
