/*
 * launcher.c - the boot menu.
 *
 * At boot the whole SD card is scanned (see romscan.c) and every game found,
 * zipped or not, is listed under its system. Joystick left/right switches
 * tabs, up/down picks a game, A starts it, SELECT jumps to the next letter.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher.h"
#include "emulators.h"
#include "romscan.h"
#include "system.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ROWS        10
#define ROW_H       10
#define LIST_Y      16

typedef enum { TAB_ROMS, TAB_BUILTIN, TAB_SETTINGS } tab_kind_t;

typedef struct {
    const char *title;
    tab_kind_t kind;
    sys_t sys;
    void (*run)(const char *path);
    uint16_t color;
    int sel, top;
} tab_t;

static tab_t tabs[] = {
    { .title = "NES",      .kind = TAB_ROMS, .sys = SYS_NES, .run = emu_nes_run },
    { .title = "Game Boy", .kind = TAB_ROMS, .sys = SYS_GB,  .run = emu_gb_run },
    { .title = "Built-in", .kind = TAB_BUILTIN },
    { .title = "Settings", .kind = TAB_SETTINGS },
};
#define TAB_COUNT ((int)(sizeof(tabs) / sizeof(tabs[0])))

static const struct { const char *name; const char *desc; void (*run)(const char *); } builtins[] = {
    { "Maze Runner 3D", "First-person maze. Find the green exit!", game_maze_run },
};
#define BUILTIN_COUNT ((int)(sizeof(builtins) / sizeof(builtins[0])))

#define SETTINGS_COUNT 5

static int s_tab;
static int64_t s_sel_since;   /* for scrolling long names */

static int item_count(const tab_t *t)
{
    switch (t->kind) {
        case TAB_ROMS: return games_count(t->sys);
        case TAB_BUILTIN: return BUILTIN_COUNT;
        default: return SETTINGS_COUNT;
    }
}

/* ---------------- scanning ---------------- */

static void scan_progress(int files, int games, const char *current)
{
    uint16_t *fb = display_fb();
    char buf[40];
    gfx_fill(fb, C_BG);
    gfx_text_center(fb, 34, "Looking for games...", C_TEXT);
    snprintf(buf, sizeof(buf), "%d found", games);
    gfx_text_center(fb, 54, buf, C_ACCENT);
    snprintf(buf, sizeof(buf), "%d files checked", files);
    gfx_text_center(fb, 66, buf, C_DIM);
    gfx_text(fb, 4, 100, current, C_DIM, (LCD_WIDTH - 8) / CHAR_W);
    display_present();
}

static void rescan(void)
{
    scan_progress(0, 0, "");
    games_scan(scan_progress);
    for (int i = 0; i < TAB_COUNT; i++) {
        int n = item_count(&tabs[i]);
        if (tabs[i].sel >= n) tabs[i].sel = n ? n - 1 : 0;
        if (tabs[i].top > tabs[i].sel) tabs[i].top = tabs[i].sel;
    }
}

/* ---------------- settings ---------------- */

static void setting_label(int i, char *out, size_t n)
{
    switch (i) {
        case 0: snprintf(out, n, "Brightness   %d%%", g_settings.brightness); break;
        case 1: snprintf(out, n, "GB colors    %s", gb_palette_names[g_settings.gb_palette]); break;
        case 2: snprintf(out, n, "NES view     %s", g_settings.nes_scale ? "Trim edges" : "Full"); break;
        case 3: snprintf(out, n, "Show FPS     %s", g_settings.show_fps ? "On" : "Off"); break;
        case 4: snprintf(out, n, "Rescan SD card"); break;
    }
}

static void setting_change(int i, int dir)
{
    switch (i) {
        case 0:
            g_settings.brightness += dir * 10;
            if (g_settings.brightness > 100) g_settings.brightness = 10;
            if (g_settings.brightness < 10) g_settings.brightness = 100;
            display_set_backlight(g_settings.brightness);
            break;
        case 1: g_settings.gb_palette = (g_settings.gb_palette + dir + GB_PALETTE_COUNT) % GB_PALETTE_COUNT; break;
        case 2: g_settings.nes_scale ^= 1; break;
        case 3: g_settings.show_fps ^= 1; break;
        case 4: if (dir > 0) rescan(); return;
    }
    settings_save();
}

/* ---------------- drawing ---------------- */

static void draw_header(uint16_t *fb, const tab_t *t)
{
    gfx_rect(fb, 0, 0, LCD_WIDTH, 13, t->color);
    gfx_text(fb, 3, 3, GLYPH_ARROW_LEFT, C_TEXT, 0);
    gfx_text(fb, LCD_WIDTH - 8, 3, GLYPH_ARROW_RIGHT, C_TEXT, 0);
    gfx_text_center(fb, 3, t->title, C_TEXT);
}

static void draw_footer(uint16_t *fb, const char *left, const char *right)
{
    gfx_rect(fb, 0, LCD_HEIGHT - 10, LCD_WIDTH, 10, C_PANEL);
    gfx_text(fb, 2, LCD_HEIGHT - 8, left, C_DIM, 0);
    if (right) gfx_text(fb, LCD_WIDTH - 2 - (int)strlen(right) * CHAR_W, LCD_HEIGHT - 8, right, C_DIM, 0);
}

static void draw_row(uint16_t *fb, int row, const char *text, bool selected)
{
    int y = LIST_Y + row * ROW_H;
    int x = 4, max = (LCD_WIDTH - 8) / CHAR_W;
    if (selected) gfx_rect(fb, 0, y - 1, LCD_WIDTH, ROW_H, C_SEL);
    int len = strlen(text), off = 0;
    if (selected && len > max) {
        /* marquee: pause, scroll, pause */
        int steps = len - max;
        int t = (int)((esp_timer_get_time() - s_sel_since) / 180000) - 5;
        t = t < 0 ? 0 : t % (steps + 10);
        off = t > steps ? steps : t;
    }
    gfx_text(fb, x, y, text + off, selected ? C_TEXT : C_DIM, max);
}

static void draw(const tab_t *t)
{
    uint16_t *fb = display_fb();
    gfx_fill(fb, C_BG);
    draw_header(fb, t);
    char buf[64];
    int n = item_count(t);

    if (t->kind == TAB_ROMS && !sd_ok()) {
        gfx_text_center(fb, 46, "No SD card found", C_RED);
        gfx_text_center(fb, 60, "Insert a FAT32 card", C_DIM);
        gfx_text_center(fb, 70, "and restart", C_DIM);
    } else if (t->kind == TAB_ROMS && n == 0) {
        gfx_text_center(fb, 36, "No games found", C_TEXT);
        gfx_text_center(fb, 54, "Copy .zip .nes .gb", C_DIM);
        gfx_text_center(fb, 64, "files anywhere on", C_DIM);
        gfx_text_center(fb, 74, "the SD card", C_DIM);
    } else {
        for (int r = 0; r < ROWS && t->top + r < n; r++) {
            int i = t->top + r;
            const char *label = buf;
            switch (t->kind) {
                case TAB_ROMS: label = games_get(t->sys, i)->name; break;
                case TAB_BUILTIN: label = builtins[i].name; break;
                default: setting_label(i, buf, sizeof(buf)); break;
            }
            draw_row(fb, r, label, i == t->sel);
        }
        if (n > ROWS) { /* scrollbar */
            int h = (ROWS * ROW_H) * ROWS / n, y = LIST_Y + (ROWS * ROW_H - h) * t->top / (n - ROWS);
            gfx_rect(fb, LCD_WIDTH - 2, y, 2, h < 3 ? 3 : h, C_ACCENT);
        }
        if (t->kind == TAB_BUILTIN)
            gfx_text_wrap(fb, 4, LCD_HEIGHT - 32, LCD_WIDTH - 8, builtins[t->sel].desc, C_DIM);
    }

    if (t->kind == TAB_SETTINGS) {
        draw_footer(fb, "A:+  B:-", "v1.1");
    } else {
        snprintf(buf, sizeof(buf), "%d/%d", n ? t->sel + 1 : 0, n);
        draw_footer(fb, t->kind == TAB_ROMS && n > ROWS ? "A:Play SEL:A-Z" : "A:Play", buf);
    }
    display_present();
}

/* ---------------- main loop ---------------- */

static void splash(void)
{
    for (int f = 0; f <= 30; f++) {
        uint16_t *fb = display_fb();
        gfx_fill(fb, C_BG);
        int w = f * 4 > 120 ? 120 : f * 4;
        gfx_rect(fb, (LCD_WIDTH - w) / 2, 70, w, 2, C_ACCENT);
        gfx_text_center(fb, 52, "RETRO POCKET", C_TEXT);
        if (f > 15) gfx_text_center(fb, 80, sd_ok() ? "SD card OK" : "No SD card", sd_ok() ? C_GREEN : C_RED);
        display_present();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void launch(tab_t *t)
{
    if (t->kind == TAB_BUILTIN) {
        builtins[t->sel].run(NULL);
        return;
    }
    const game_t *g = t->kind == TAB_ROMS ? games_get(t->sys, t->sel) : NULL;
    if (!g) return;
    uint16_t *fb = display_fb();
    gfx_fill(fb, C_BG);
    gfx_text_center(fb, 52, "Loading...", C_TEXT);
    gfx_text(fb, 4, 70, g->name, C_DIM, (LCD_WIDTH - 8) / CHAR_W);
    display_present();
    display_wait();
    t->run(g->path);
}

/* SELECT: jump to the first game starting with the next letter. */
static void jump_letter(tab_t *t)
{
    int n = item_count(t);
    if (t->kind != TAB_ROMS || n < 2) return;
    int cur = toupper((unsigned char)games_get(t->sys, t->sel)->name[0]);
    for (int k = 1; k < n; k++) {
        int i = (t->sel + k) % n;
        int c = toupper((unsigned char)games_get(t->sys, i)->name[0]);
        if (c != cur) { t->sel = i; t->top = i; return; }
    }
}

void launcher_run(void)
{
    tabs[0].color = RGB(170, 40, 50);
    tabs[1].color = RGB(60, 120, 60);
    tabs[2].color = RGB(60, 70, 170);
    tabs[3].color = RGB(80, 80, 96);

    splash();
    rescan();
    /* open on the first tab that actually has games */
    for (int i = 0; i < TAB_COUNT; i++)
        if (tabs[i].kind != TAB_ROMS || item_count(&tabs[i])) { s_tab = i; break; }
    s_sel_since = esp_timer_get_time();

    for (;;) {
        tab_t *t = &tabs[s_tab];
        input_poll();
        uint32_t r = input_repeat(), p = input_pressed();
        int n = item_count(t), old_sel = t->sel;

        if (r & (BTN_LEFT | BTN_RIGHT)) {
            s_tab = (s_tab + (r & BTN_LEFT ? TAB_COUNT - 1 : 1)) % TAB_COUNT;
            s_sel_since = esp_timer_get_time();
            continue;
        }
        if (n) {
            if (r & BTN_UP)   t->sel = (t->sel + n - 1) % n;
            if (r & BTN_DOWN) t->sel = (t->sel + 1) % n;
        }
        if (t->kind == TAB_SETTINGS) {
            if (p & BTN_A) setting_change(t->sel, +1);
            if (p & BTN_B) setting_change(t->sel, -1);
        } else {
            if (p & (BTN_A | BTN_START)) launch(t);
            if (p & BTN_SELECT) jump_letter(t);
        }
        if (t->sel != old_sel) s_sel_since = esp_timer_get_time();
        if (t->sel < t->top) t->top = t->sel;
        if (t->sel >= t->top + ROWS) t->top = t->sel - ROWS + 1;
        if (n > ROWS && t->top > n - ROWS) t->top = n - ROWS;

        draw(t);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
