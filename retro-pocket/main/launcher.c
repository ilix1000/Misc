/*
 * launcher.c - the boot menu.
 *
 * Joystick left/right switches tabs (NES / Game Boy / Built-in / Settings),
 * up/down picks a game, A starts it, B goes up a folder.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "launcher.h"
#include "emulators.h"
#include "system.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_ENTRIES 1024
#define NAME_LEN    96
#define ROWS        10
#define ROW_H       10
#define LIST_Y      16

typedef enum { TAB_ROMS, TAB_BUILTIN, TAB_SETTINGS } tab_kind_t;

typedef struct {
    const char *title;
    tab_kind_t kind;
    const char *root;          /* ROM folder on the SD card */
    const char *exts;          /* space separated, lowercase */
    void (*run)(const char *path);
    uint16_t color;
    char cwd[256];
    int sel, top;
} tab_t;

typedef struct {
    char name[NAME_LEN];
    bool dir;
} entry_t;

static tab_t tabs[] = {
    { .title = "NES",      .kind = TAB_ROMS, .root = DIR_ROMS "/nes", .exts = ".nes", .run = emu_nes_run },
    { .title = "Game Boy", .kind = TAB_ROMS, .root = DIR_ROMS "/gb", .exts = ".gb .gbc .dmg", .run = emu_gb_run },
    { .title = "Built-in", .kind = TAB_BUILTIN },
    { .title = "Settings", .kind = TAB_SETTINGS },
};
#define TAB_COUNT ((int)(sizeof(tabs) / sizeof(tabs[0])))

static const struct { const char *name; const char *desc; void (*run)(const char *); } builtins[] = {
    { "Maze Runner 3D", "First-person maze. Find the green exit!", game_maze_run },
};
#define BUILTIN_COUNT ((int)(sizeof(builtins) / sizeof(builtins[0])))

static entry_t *s_entries;
static int s_count;
static int s_tab;
static int64_t s_sel_since;   /* for scrolling long names */

/* ---------------- directory listing ---------------- */

static bool has_ext(const char *name, const char *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[12];
    int n = 0;
    for (const char *p = dot; *p && n < 10; p++) ext[n++] = tolower((unsigned char)*p);
    ext[n] = 0;
    for (const char *p = exts; *p; ) {
        const char *sp = strchr(p, ' ');
        int len = sp ? (int)(sp - p) : (int)strlen(p);
        if (len == n && strncmp(p, ext, n) == 0) return true;
        p += len + (sp ? 1 : 0);
    }
    return false;
}

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan(tab_t *t)
{
    s_count = 0;
    if (t->kind != TAB_ROMS || !sd_ok()) return;
    if (!t->cwd[0]) strlcpy(t->cwd, t->root, sizeof(t->cwd));
    DIR *d = opendir(t->cwd);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_count < MAX_ENTRIES) {
        if (e->d_name[0] == '.') continue;
        bool dir = e->d_type == DT_DIR;
        if (!dir && !has_ext(e->d_name, t->exts)) continue;
        if (strlen(e->d_name) >= NAME_LEN) continue;
        strcpy(s_entries[s_count].name, e->d_name);
        s_entries[s_count].dir = dir;
        s_count++;
    }
    closedir(d);
    qsort(s_entries, s_count, sizeof(entry_t), cmp_entry);
    if (t->sel >= s_count) t->sel = s_count ? s_count - 1 : 0;
}

static int item_count(const tab_t *t)
{
    switch (t->kind) {
        case TAB_ROMS: return s_count;
        case TAB_BUILTIN: return BUILTIN_COUNT;
        default: return 4;
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
    }
    settings_save();
}

/* ---------------- drawing ---------------- */

static void display_name(const char *in, bool dir, char *out, size_t n)
{
    strlcpy(out, in, n);
    if (!dir) {
        char *dot = strrchr(out, '.');
        if (dot) *dot = 0;
    }
}

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

static void draw_row(uint16_t *fb, int row, const char *text, bool selected, bool folder)
{
    int y = LIST_Y + row * ROW_H;
    int x = 4, max = (LCD_WIDTH - 8) / CHAR_W;
    if (selected) gfx_rect(fb, 0, y - 1, LCD_WIDTH, ROW_H, C_SEL);
    if (folder) {
        gfx_text(fb, x, y, GLYPH_FOLDER, C_ACCENT, 0);
        x += CHAR_W + 2;
        max -= 2;
    }
    int len = strlen(text), off = 0;
    if (selected && len > max) {
        /* marquee: pause, scroll, pause */
        int steps = len - max;
        int t = (int)((esp_timer_get_time() - s_sel_since) / 180000) - 5;
        int cycle = steps + 10;
        t = t < 0 ? 0 : t % cycle;
        off = t > steps ? steps : t;
    }
    gfx_text(fb, x, y, text + off, selected ? C_TEXT : C_DIM, max);
}

static void draw(const tab_t *t)
{
    uint16_t *fb = display_fb();
    gfx_fill(fb, C_BG);
    draw_header(fb, t);
    char buf[NAME_LEN + 16];
    int n = item_count(t);

    if (t->kind == TAB_ROMS && !sd_ok()) {
        gfx_text_center(fb, 46, "No SD card found", C_RED);
        gfx_text_center(fb, 60, "Insert a FAT32 card", C_DIM);
        gfx_text_center(fb, 70, "and restart", C_DIM);
    } else if (t->kind == TAB_ROMS && n == 0) {
        gfx_text_center(fb, 46, "No games here yet", C_TEXT);
        snprintf(buf, sizeof(buf), "Copy ROMs to");
        gfx_text_center(fb, 60, buf, C_DIM);
        gfx_text_center(fb, 70, t->cwd + strlen(SD_MOUNT), C_ACCENT);
    } else {
        for (int r = 0; r < ROWS && t->top + r < n; r++) {
            int i = t->top + r;
            bool sel = i == t->sel, folder = false;
            switch (t->kind) {
                case TAB_ROMS:
                    folder = s_entries[i].dir;
                    display_name(s_entries[i].name, folder, buf, sizeof(buf));
                    break;
                case TAB_BUILTIN: strlcpy(buf, builtins[i].name, sizeof(buf)); break;
                default: setting_label(i, buf, sizeof(buf)); break;
            }
            draw_row(fb, r, buf, sel, folder);
        }
        if (n > ROWS) { /* scrollbar */
            int h = (ROWS * ROW_H) * ROWS / n, y = LIST_Y + (ROWS * ROW_H - h) * t->top / (n - ROWS);
            gfx_rect(fb, LCD_WIDTH - 2, y, 2, h, C_ACCENT);
        }
        if (t->kind == TAB_BUILTIN)
            gfx_text_wrap(fb, 4, LCD_HEIGHT - 32, LCD_WIDTH - 8, builtins[t->sel].desc, C_DIM);
    }

    if (t->kind == TAB_SETTINGS) {
        draw_footer(fb, "A:+  B:-", "v1.0");
    } else {
        snprintf(buf, sizeof(buf), "%d/%d", n ? t->sel + 1 : 0, n);
        draw_footer(fb, t->kind == TAB_ROMS && strcmp(t->cwd, t->root) ? "A:Play  B:Back" : "A:Play", buf);
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
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void launch(tab_t *t)
{
    if (t->kind == TAB_BUILTIN) {
        builtins[t->sel].run(NULL);
        return;
    }
    if (t->kind != TAB_ROMS || s_count == 0) return;
    entry_t *e = &s_entries[t->sel];
    char path[sizeof(t->cwd) + NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", t->cwd, e->name);
    if (e->dir) {
        if (strlen(path) < sizeof(t->cwd)) {
            strcpy(t->cwd, path);
            t->sel = t->top = 0;
            scan(t);
        }
        return;
    }
    display_wait();
    t->run(path);
    scan(t); /* the list buffer is reused, refresh it after a game */
}

void launcher_run(void)
{
    s_entries = heap_caps_malloc(sizeof(entry_t) * MAX_ENTRIES, MALLOC_CAP_SPIRAM);
    if (!s_entries) s_entries = malloc(sizeof(entry_t) * 64);
    tabs[0].color = RGB(170, 40, 50);
    tabs[1].color = RGB(60, 120, 60);
    tabs[2].color = RGB(60, 70, 170);
    tabs[3].color = RGB(80, 80, 96);

    splash();
    scan(&tabs[s_tab]);
    s_sel_since = esp_timer_get_time();

    for (;;) {
        tab_t *t = &tabs[s_tab];
        input_poll();
        uint32_t r = input_repeat(), p = input_pressed();
        int n = item_count(t), old_sel = t->sel;

        if (r & BTN_LEFT)  { s_tab = (s_tab + TAB_COUNT - 1) % TAB_COUNT; scan(&tabs[s_tab]); }
        if (r & BTN_RIGHT) { s_tab = (s_tab + 1) % TAB_COUNT; scan(&tabs[s_tab]); }
        if (s_tab != (int)(t - tabs)) { s_sel_since = esp_timer_get_time(); continue; }

        if (n) {
            if (r & BTN_UP)   t->sel = (t->sel + n - 1) % n;
            if (r & BTN_DOWN) t->sel = (t->sel + 1) % n;
        }
        if (t->kind == TAB_SETTINGS) {
            if (p & BTN_A) setting_change(t->sel, +1);
            if (p & BTN_B) setting_change(t->sel, -1);
        } else {
            if (p & (BTN_A | BTN_START)) launch(t);
            if ((p & BTN_B) && t->kind == TAB_ROMS && strcmp(t->cwd, t->root)) {
                char *slash = strrchr(t->cwd, '/');
                if (slash) *slash = 0;
                t->sel = t->top = 0;
                scan(t);
            }
        }
        if (t->sel != old_sel) s_sel_since = esp_timer_get_time();
        if (t->sel < t->top) t->top = t->sel;
        if (t->sel >= t->top + ROWS) t->top = t->sel - ROWS + 1;

        draw(t);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
