/*
 * emu_pce.c - PC Engine / TurboGrafx-16, on PCE-GO (GPLv2, from Retro-Go).
 *
 * The PC Engine pad is a d-pad, buttons I and II, Select and Run, so it maps
 * one-to-one onto this handheld: A = I, B = II, START = Run, SELECT = Select.
 *
 * PCE-GO owns the main loop (RunPCE) and calls back into the three osd_*
 * functions below every frame.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "emulators.h"
#include "romscan.h"
#include "system.h"
#include "bridge.h"
#include "display.h"
#include "input.h"
#include "video.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define RETRO_GO 1
#include "pce-go.h"

#define PCE_FRAME_US 16667
#define MAX_SKIP 3

static uint8_t *s_buf[2];       /* XBUF_WIDTH x XBUF_HEIGHT each, +16 scratch */
static int s_cur;
static uint16_t s_pal[256];     /* panel byte order */
static int s_w, s_h, s_setup_w, s_setup_h;
static bool s_draw = true, s_skip;
static int s_skipped;
static pacer_t s_pacer;
static const char *s_path;

static void state_path(char *out, size_t n) { save_path(out, n, s_path, "pce", ".state"); }

uint8_t *osd_gfx_framebuffer(int width, int height)
{
    if (width > 0 && height > 0) { s_w = width; s_h = height; }
    /* until a game programs the video size registers, assume the usual mode */
    if (s_w <= 0 || s_h <= 0) { s_w = 256; s_h = 224; }
    if (s_w > XBUF_WIDTH - 16) s_w = XBUF_WIDTH - 16;
    if (s_h > XBUF_HEIGHT) s_h = XBUF_HEIGHT;
    return s_draw ? s_buf[s_cur] + 16 : NULL;   /* NULL = skip drawing this frame */
}

void osd_vsync(void)
{
    if (s_draw && s_w > 0 && s_h > 0) {
        if (s_w != s_setup_w || s_h != s_setup_h) {
            video_wait();
            video_setup(s_w, s_h);
            s_setup_w = s_w;
            s_setup_h = s_h;
        }
        if (video_submit(s_buf[s_cur] + 16, XBUF_WIDTH, s_pal)) s_cur ^= 1;
    }
    bool behind = pacer_wait(&s_pacer, PCE_FRAME_US);
    s_skip = behind && s_skipped < MAX_SKIP;
    s_skipped = s_skip ? s_skipped + 1 : 0;
    s_draw = !s_skip;
}

void osd_input_read(uint8_t joypads[8])
{
    uint32_t k = input_poll();
    if (bridge_quit_requested()) { StopPCE(); return; }
    if (input_pressed() & BTN_MENU) {
        video_wait();
        ingame_action_t a = ingame_menu("PC Engine", true);
        char path[300];
        state_path(path, sizeof(path));
        if (a == MENU_QUIT) StopPCE();
        if (a == MENU_RESET) ResetPCE(false);
        if (a == MENU_SAVE_STATE)
            message_box(SaveState(path) == 0 ? "State saved" : "Save failed", NULL, 700);
        if (a == MENU_LOAD_STATE)
            message_box(LoadState(path) == 0 ? "State loaded" : "No saved state", NULL, 700);
        pacer_start(&s_pacer);
        s_setup_w = 0;               /* menu drew over the screen: redo layout */
        k = 0;
    }
    uint8_t b = 0;
    if (k & BTN_LEFT)   b |= JOY_LEFT;
    if (k & BTN_RIGHT)  b |= JOY_RIGHT;
    if (k & BTN_UP)     b |= JOY_UP;
    if (k & BTN_DOWN)   b |= JOY_DOWN;
    if (k & BTN_A)      b |= JOY_A;
    if (k & BTN_B)      b |= JOY_B;
    if (k & BTN_START)  b |= JOY_RUN;
    if (k & BTN_SELECT) b |= JOY_SELECT;
    joypads[0] = b;
}

void emu_pce_run(const char *path)
{
    size_t size;
    uint8_t *raw = rom_load(path, &size);
    if (!raw) { message_box("Can't read ROM", NULL, 0); return; }
    if (size < 0x2000 || size > 0x1000000) { free(raw); message_box("Not a PC Engine ROM", NULL, 0); return; }

    /* PCE-GO maps banks with a power-of-two mask; give it zero-filled room up
     * to that size so odd-sized ROMs never read past the buffer. */
    size_t offset = size & 0x1fff, banks = (size - offset + 0x1fff) / 0x2000, pow2 = 1;
    while (pow2 < banks) pow2 <<= 1;
    uint8_t *rom = heap_caps_calloc(1, offset + pow2 * 0x2000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom) { free(raw); message_box("Out of memory", NULL, 0); return; }
    memcpy(rom, raw, size);
    free(raw);

    for (int i = 0; i < 2; i++)
        if (!s_buf[i]) s_buf[i] = heap_caps_calloc(1, XBUF_WIDTH * XBUF_HEIGHT + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf[0] || !s_buf[1]) { free(rom); message_box("Out of memory", NULL, 0); return; }

    uint16_t *pal = PalettePCE(16);
    for (int i = 0; i < 256; i++) s_pal[i] = pal ? SWAP16(pal[i]) : 0;
    free(pal);

    s_path = path;
    s_cur = 0;
    s_w = s_h = s_setup_w = s_setup_h = 0;
    s_draw = true;
    s_skipped = 0;

    if (InitPCE(22050, false) != 0) { free(rom); message_box("PC Engine init failed", NULL, 0); return; }
    if (LoadCard(rom, size) != 0) {    /* PCE-GO owns rom from here (frees it) */
        ShutdownPCE();
        message_box("Can't load this ROM", NULL, 0);
        return;
    }
    pacer_start(&s_pacer);
    RunPCE();                          /* returns after StopPCE() */
    video_wait();
    ShutdownPCE();
}
