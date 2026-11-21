#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "i18n.h"
#include "board.h"
#include "gfx.h"
#include "input.h"
#include "ui.h"
#include "bridge.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---------------- files ---------------- */

uint8_t *file_load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!buf) buf = n > 0 ? malloc(n) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf && size) *size = n;
    return buf;
}

bool file_save(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool file_read_into(const char *path, void *data, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

void save_path(char *out, size_t n, const char *rom_path, const char *sys, const char *ext)
{
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    snprintf(out, n, "%s/%s/%.*s%s", DIR_SAVES, sys, len, base, ext);
}

/* ---------------- menus ---------------- */

static uint16_t *s_snapshot;

void snapshot_take(void)
{
    if (!s_snapshot) s_snapshot = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!s_snapshot) return;
    display_wait();
    memcpy(s_snapshot, display_front(), LCD_WIDTH * LCD_HEIGHT * 2);
    gfx_dim(s_snapshot);
}

void snapshot_draw(uint16_t *fb)
{
    if (s_snapshot) memcpy(fb, s_snapshot, LCD_WIDTH * LCD_HEIGHT * 2);
    else gfx_fill(fb, C_BG);
}

static void wait_release(void)
{
    while (input_poll()) vTaskDelay(pdMS_TO_TICKS(10));
}

ingame_action_t ingame_menu(const char *title, bool has_states)
{
    static const struct { const char *label; ingame_action_t act; bool state; } items[] = {
        { "Resume",     MENU_RESUME,     false },
        { "Save state", MENU_SAVE_STATE, true  },
        { "Load state", MENU_LOAD_STATE, true  },
        { "Reset",      MENU_RESET,      false },
        { "Quit",       MENU_QUIT,       false },
    };
    int idx[5], n = 0;
    for (int i = 0; i < 5; i++)
        if (!items[i].state || has_states) idx[n++] = i;

    snapshot_take();
    wait_release();
    int sel = 0;
    float hl = 0, open = ui_anim_on() ? 0 : 1;
    ui_dt();
    const int w = 104, h = 20 + n * 13, x = (LCD_WIDTH - w) / 2, y = (LCD_HEIGHT - h) / 2;
    for (;;) {
        float dt = ui_dt();
        input_poll();
        uint32_t r = input_repeat(), p = input_pressed();
        if (r & BTN_UP) sel = (sel + n - 1) % n;
        if (r & BTN_DOWN) sel = (sel + 1) % n;
        bool quit = bridge_quit_requested();
        if (p & (BTN_B | BTN_MENU)) sel = -1;
        if (quit || sel < 0 || (p & (BTN_A | BTN_START))) {
            /* leave the plain (dimmed) game frame on screen behind any message */
            gfx_set_origin(0, 0);
            snapshot_draw(display_fb());
            display_present();
            wait_release();
            return quit ? MENU_QUIT : sel < 0 ? MENU_RESUME : items[idx[sel]].act;
        }
        open = ui_approach(open, 1, 9, dt);
        hl = ui_approach(hl, sel * 13, 22, dt);
        int oy = (int)((1 - ui_ease_out_back(open)) * (LCD_HEIGHT - y));

        uint16_t *fb = display_fb();
        gfx_set_origin(0, 0);
        snapshot_draw(fb);
        gfx_set_origin(0, oy);
        gfx_round_rect(fb, x - 1, y - 1, w + 2, h + 2, C_ACCENT);
        gfx_round_rect(fb, x, y, w, h, RGB(24, 27, 46));
        gfx_rect(fb, x + 1, y + 1, w - 2, 13, RGB(44, 40, 30));
        gfx_text(fb, x + 6, y + 4, title, C_ACCENT, (w - 12) / CHAR_W);
        gfx_round_rect(fb, x + 4, y + 17 + (int)hl, w - 8, 12, C_SEL);
        for (int i = 0; i < n; i++)
            gfx_text(fb, x + 12, y + 19 + i * 13, T(items[idx[i]].label), i == sel ? C_TEXT : C_DIM, 0);
        gfx_set_origin(0, 0);
        ui_draw_toast(fb, dt);
        display_present();
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

void message_box(const char *line1, const char *line2, int ms)
{
    snapshot_take();
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    wait_release();
    float open = ui_anim_on() ? 0 : 1;
    ui_dt();
    for (;;) {
        float dt = ui_dt();
        open = ui_approach(open, 1, 10, dt);
        float k = ui_ease_out_back(open);
        uint16_t *fb = display_fb();
        gfx_set_origin(0, 0);
        snapshot_draw(fb);
        int bw = (int)((LCD_WIDTH - 8) * (0.6f + 0.4f * k)), bh = 40;
        int bx = (LCD_WIDTH - bw) / 2, by = (LCD_HEIGHT - bh) / 2;
        gfx_round_rect(fb, bx - 1, by - 1, bw + 2, bh + 2, C_ACCENT);
        gfx_round_rect(fb, bx, by, bw, bh, RGB(24, 27, 46));
        if (open > 0.5f) {
            gfx_text_center(fb, by + 10, T(line1), C_TEXT);
            if (line2) gfx_text_center(fb, by + 22, T(line2), C_DIM);
        }
        display_present();
        input_poll();
        if (input_pressed()) break;
        if (ms > 0 && esp_timer_get_time() >= end) break;
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    wait_release();
}

/* ---------------- frame pacing ---------------- */

void pacer_start(pacer_t *p)
{
    p->next = esp_timer_get_time();
    p->fps_t0 = p->next;
    p->frames = 0;
    p->fps = 0;
}

bool pacer_wait(pacer_t *p, int frame_us)
{
    p->next += frame_us;
    int64_t now = esp_timer_get_time();
    p->frames++;
    if (now - p->fps_t0 >= 1000000) {
        p->fps = p->frames;
        p->frames = 0;
        p->fps_t0 = now;
    }
    if (p->next > now) {
        int64_t wait = p->next - now;
        if (wait > 2000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
        while (esp_timer_get_time() < p->next) { }
        return false;
    }
    if (now - p->next > 100000) p->next = now; /* way behind: don't try to catch up */
    return true;
}

void draw_fps(uint16_t *fb, int fps)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", fps);
    gfx_rect(fb, 0, 0, (int)strlen(buf) * CHAR_W + 2, 9, C_BLACK);
    gfx_text(fb, 1, 1, buf, C_GREEN, 0);
}
