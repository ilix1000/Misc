/*
 * ui.c - animation helpers, toasts, status bar, background, keyboard.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "i18n.h"
#include "gfx.h"
#include "icons.h"
#include "input.h"
#include "bridge.h"
#include "system.h"
#include "wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---------------- timing & easing ---------------- */

float ui_dt(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    float dt = last ? (now - last) / 1e6f : 0.016f;
    last = now;
    return dt > 0.1f ? 0.1f : dt;
}

float ui_time(void) { return esp_timer_get_time() / 1e6f; }
bool ui_anim_on(void) { return g_settings.ui_anim != 0; }

float ui_approach(float cur, float target, float speed, float dt)
{
    if (!ui_anim_on()) return target;
    float v = cur + (target - cur) * (1.0f - expf(-speed * dt));
    return fabsf(target - v) < 0.002f ? target : v;
}

float ui_ease_out_cubic(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float u = 1 - t;
    return 1 - u * u * u;
}

float ui_ease_out_back(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float c1 = 1.70158f, c3 = c1 + 1;
    float u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}

/* ---------------- toasts ---------------- */

static char s_toast[64];
static float s_toast_t = -1;   /* seconds shown, <0 = none */

void ui_toast(const char *msg)
{
    snprintf(s_toast, sizeof(s_toast), "%s", T(msg));
    s_toast_t = 0;
}

void ui_draw_toast(uint16_t *fb, float dt)
{
    if (s_toast_t < 0) {
        char m[64];
        if (!bridge_take_toast(m, sizeof(m))) return;
        ui_toast(m);
    }
    s_toast_t += dt;
    const float in = 0.3f, hold = 2.2f, out = 0.3f;
    float t = s_toast_t, y;
    if (t < in) y = -16 + 16 * ui_ease_out_back(t / in);
    else if (t < in + hold) y = 0;
    else if (t < in + hold + out) y = -16 * ui_ease_out_cubic((t - in - hold) / out);
    else { s_toast_t = -1; return; }
    if (!ui_anim_on()) y = t < in + hold + out ? 0 : -16;
    int max = (LCD_WIDTH - 12) / CHAR_W;
    int len = strlen(s_toast);
    if (len > max) len = max;
    int w = len * CHAR_W + 10, x = (LCD_WIDTH - w) / 2;
    gfx_set_origin(0, 0);
    gfx_round_rect(fb, x, (int)y + 2, w, 13, RGB(255, 196, 40));
    gfx_round_rect(fb, x + 1, (int)y + 3, w - 2, 11, RGB(30, 33, 56));
    gfx_text(fb, x + 5, (int)y + 5, s_toast, C_TEXT, max);
}

/* ---------------- status bar ---------------- */

static void wifi_glyph(uint16_t *fb, int x, int y, int bars, uint16_t on, uint16_t off)
{
    for (int i = 0; i < 4; i++)
        gfx_rect(fb, x + i * 3, y + 6 - i * 2, 2, 2 + i * 2, i < bars ? on : off);
}

void ui_draw_statusbar(uint16_t *fb, const char *title, uint16_t color)
{
    gfx_rect_alpha(fb, 0, 0, LCD_WIDTH, UI_BAR_H, C_BLACK, 110);
    gfx_rect(fb, 0, UI_BAR_H - 1, LCD_WIDTH, 1, color);
    gfx_text(fb, 3, 2, title, C_TEXT, (LCD_WIDTH - 40) / CHAR_W);
    int x = LCD_WIDTH - 14;
    switch (wifi_state()) {
        case WIFI_CONNECTED:
            wifi_glyph(fb, x, 2, wifi_bars(), C_TEXT, RGB(70, 74, 100));
            break;
        case WIFI_CONNECTING: {
            int blink = ((int)(ui_time() * 3)) & 1;
            wifi_glyph(fb, x, 2, 4, blink ? C_DIM : RGB(70, 74, 100), RGB(70, 74, 100));
            break;
        }
        case WIFI_HOTSPOT:
            break;
        default:
            break;
    }
    if (wifi_hotspot_on()) {
        x -= wifi_state() == WIFI_CONNECTED || wifi_state() == WIFI_CONNECTING ? 16 : 2;
        gfx_text(fb, x, 2, "AP", C_ACCENT, 0);
    }
}

/* ---------------- background ---------------- */

void ui_draw_background(uint16_t *fb, uint32_t tint, float t)
{
    /* gradient from a dark version of the tint to near-black */
    uint32_t top = ((((tint >> 16) & 255) * 45 / 100) << 16) | ((((tint >> 8) & 255) * 45 / 100) << 8) | ((tint & 255) * 45 / 100);
    gfx_vgradient(fb, 0, 0, LCD_WIDTH, LCD_HEIGHT, top, 0x080910);
    /* drifting diagonal light streaks */
    for (int k = 0; k < 3; k++) {
        float off = fmodf(t * (8 + k * 5) + k * 70, LCD_WIDTH + LCD_HEIGHT + 60) - 40;
        for (int y = 0; y < LCD_HEIGHT; y += 1) {
            int x = (int)(off - y * 0.6f);
            gfx_rect_alpha(fb, x, y, 6 + k * 4, 1, RGB(255, 255, 255), 10);
        }
    }
    /* twinkling dots */
    for (int i = 0; i < 18; i++) {
        uint32_t h = i * 2654435761u;
        int x = h % LCD_WIDTH, y = (h >> 12) % LCD_HEIGHT;
        float b = 0.5f + 0.5f * sinf(t * (1.5f + (h & 7) * 0.3f) + i);
        gfx_rect_alpha(fb, x, y, 1, 1, RGB(255, 255, 255), (int)(40 + 120 * b));
    }
}

void ui_draw_spinner(uint16_t *fb, int cx, int cy, float t, uint16_t c)
{
    for (int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4 + t * 5;
        int x = cx + (int)(cosf(a) * 8), y = cy + (int)(sinf(a) * 8);
        int alpha = 255 - ((i * 32 + (int)(t * 300)) % 256);
        gfx_rect_alpha(fb, x - 1, y - 1, 3, 3, c, alpha);
    }
}

/* ---------------- on-screen keyboard ---------------- */

static const char *const kb_layouts[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm.-_" },
    { "!?#$%&*()=", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM,'\"" },
    { "1234567890", "+-*/\\|<>[]", "{}~^`;:,.'", "\"!?#$%&=_ " },
};
static const char *const kb_special[] = { "abc", "ABC", "#+=", "Space", "Del", "OK" };

bool ui_keyboard(const char *title, char *buf, size_t n)
{
    int layout = 0, row = 1, col = 0;
    float hx = 0, hy = 0;
    size_t len = strlen(buf);
    while (input_poll()) vTaskDelay(pdMS_TO_TICKS(10));
    float open = ui_anim_on() ? 0 : 1;

    for (;;) {
        float dt = ui_dt();
        input_poll();
        uint32_t r = input_repeat(), p = input_pressed();
        const int rows = 5, cols = row == 4 ? 4 : 10;
        if (r & BTN_UP) row = (row + rows - 1) % rows;
        if (r & BTN_DOWN) row = (row + 1) % rows;
        int c2 = row == 4 ? 4 : 10;
        if (col >= c2) col = c2 - 1;
        if (r & BTN_LEFT) col = (col + c2 - 1) % c2;
        if (r & BTN_RIGHT) col = (col + 1) % c2;
        (void)cols;

        bool done = false;
        if (p & BTN_MENU) { while (input_poll()) vTaskDelay(pdMS_TO_TICKS(10)); return false; }
        if (p & BTN_START) done = true;
        if (p & BTN_SELECT) layout = layout == 0 ? 1 : 0;
        if ((p & BTN_B) && len > 0) buf[--len] = 0;
        if (p & BTN_A) {
            if (row < 4) {
                if (len + 1 < n) { buf[len++] = kb_layouts[layout][row][col]; buf[len] = 0; }
                if (layout == 1 && row > 0) layout = 0;   /* shift is one-shot */
            } else switch (col) {
                case 0: layout = layout == 2 ? 0 : 2; break;
                case 1: if (len + 1 < n) { buf[len++] = ' '; buf[len] = 0; } break;
                case 2: if (len > 0) buf[--len] = 0; break;
                case 3: done = true; break;
            }
        }
        if (done) { while (input_poll()) vTaskDelay(pdMS_TO_TICKS(10)); return true; }

        /* layout */
        int W = LCD_WIDTH, H = LCD_HEIGHT;
        int kw = (W - 4) / 10, kh = 13;
        int ky = H - 5 * (kh + 1) - 3;
        static const int sp_w[4] = { 2, 4, 2, 2 }; /* bottom row widths in key units */
        float tx, ty, tw;
        if (row < 4) { tx = 2 + col * kw; ty = ky + row * (kh + 1); tw = kw; }
        else {
            int ux = 0;
            for (int i = 0; i < col; i++) ux += sp_w[i];
            tx = 2 + ux * kw * 10 / 10.0f; ty = ky + 4 * (kh + 1); tw = sp_w[col] * kw;
        }
        hx = ui_approach(hx ? hx : tx, tx, 22, dt);
        hy = ui_approach(hy ? hy : ty, ty, 22, dt);
        open = ui_approach(open, 1, 12, dt);
        int slide = (int)((1 - open) * H);

        uint16_t *fb = display_fb();
        gfx_set_origin(0, 0);
        gfx_vgradient(fb, 0, 0, W, H, 0x1c2040, 0x090a14);
        gfx_set_origin(0, slide);
        gfx_text(fb, 4, 3, title, C_DIM, (W - 8) / CHAR_W);
        /* text field */
        gfx_round_rect(fb, 2, 13, W - 4, 15, RGB(10, 11, 20));
        gfx_frame(fb, 2, 13, W - 4, 15, RGB(60, 66, 110));
        int maxc = (W - 14) / CHAR_W, start = (int)len > maxc ? (int)len - maxc : 0;
        int cx = gfx_text(fb, 6, 17, buf + start, C_TEXT, 0);
        if (((int)(ui_time() * 2)) & 1) gfx_rect(fb, cx, 16, 1, 9, C_ACCENT);
        /* keys */
        gfx_round_rect(fb, (int)hx, (int)hy, (int)tw - 1, kh, C_SEL);
        for (int rr = 0; rr < 4; rr++)
            for (int cc = 0; cc < 10; cc++) {
                int x = 2 + cc * kw, y = ky + rr * (kh + 1);
                bool sel = rr == row && cc == col;
                if (!sel) gfx_round_rect(fb, x, y, kw - 1, kh, RGB(36, 40, 68));
                char s[2] = { kb_layouts[layout][rr][cc], 0 };
                gfx_text(fb, x + (kw - 6) / 2, y + 3, s, sel ? C_ACCENT : C_TEXT, 0);
            }
        int ux = 0;
        const char *labels[4] = { layout == 2 ? kb_special[0] : kb_special[2], T(kb_special[3]), T(kb_special[4]), kb_special[5] };
        for (int i = 0; i < 4; i++) {
            int x = 2 + ux * kw, y = ky + 4 * (kh + 1), w = sp_w[i] * kw;
            bool sel = row == 4 && col == i;
            if (!sel) gfx_round_rect(fb, x, y, w - 1, kh, i == 3 ? RGB(40, 100, 60) : RGB(50, 54, 86));
            gfx_text(fb, x + (w - gfx_text_width(labels[i], 1)) / 2, y + 3, labels[i], sel ? C_ACCENT : C_TEXT, 0);
            ux += sp_w[i];
        }
        gfx_set_origin(0, 0);
        display_present();
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}
