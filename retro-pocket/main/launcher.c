/*
 * launcher.c - the boot menu.
 *
 * Home: a carousel of cards (Recent, Favorites, NES, Game Boy, Built-in,
 * Wi-Fi, Settings). A opens a card; screens slide in and out; lists scroll
 * smoothly. Requests from the web UI (launch a game, rescan, settings...)
 * are picked up here every frame.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher.h"
#include "bridge.h"
#include "emulators.h"
#include "icons.h"
#include "ota.h"
#include "romscan.h"
#include "system.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "ui.h"
#include "version.h"
#include "wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define W LCD_WIDTH
#define H LCD_HEIGHT
#define ROW_H 11
#define HEAD_H 18
#define FOOT_H 10

/* ---------------- screens ---------------- */

typedef enum { SCR_HOME, SCR_LIST, SCR_SETTINGS, SCR_WIFI, SCR_NETS, SCR_QR } scr_t;

typedef enum { CARD_RECENT, CARD_FAVS, CARD_NES, CARD_GB, CARD_BUILTIN, CARD_WIFI, CARD_SETTINGS, CARD_COUNT } card_t;

static const struct { const char *title; const uint16_t *icon; uint32_t color; } cards[CARD_COUNT] = {
    [CARD_RECENT]   = { "Recent",    icon_clock, 0x3a7bd5 },
    [CARD_FAVS]     = { "Favorites", icon_star,  0xd99a1e },
    [CARD_NES]      = { "NES",       icon_nes,   0xc8303c },
    [CARD_GB]       = { "Game Boy",  icon_gb,    0x4f9a3c },
    [CARD_BUILTIN]  = { "Built-in",  icon_cube,  0x7a4fd6 },
    [CARD_WIFI]     = { "Wi-Fi",     icon_wifi,  0x2aa6c9 },
    [CARD_SETTINGS] = { "Settings",  icon_gear,  0x6b7090 },
};

static const struct { const char *name; const char *desc; void (*run)(const char *); } builtins[] = {
    { "Maze Runner 3D", "First-person maze. Find the green exit!", game_maze_run },
};
#define BUILTIN_COUNT ((int)(sizeof(builtins) / sizeof(builtins[0])))

/* A scrolling list (games, settings, Wi-Fi menu, networks). */
typedef struct {
    int sel;
    float sel_y, scroll;   /* animated, in pixels */
    int64_t sel_since;     /* for the long-name marquee */
} list_t;

static struct {
    scr_t stack[8];
    int depth;
    /* transition */
    float trans;           /* 0..1, 1 = done */
    int trans_dir;         /* +1 push, -1 pop */
    scr_t trans_from;
    /* home */
    int home_sel;
    float home_cam;
    float dots_x;
    /* lists */
    card_t list_card;
    list_t list, settings, wifi, nets;
    /* QR page */
    int qr_page;
    float t;
} S;

static scr_t top(void) { return S.stack[S.depth - 1]; }

static void push(scr_t s)
{
    if (S.depth >= 8) return;
    S.trans_from = top();
    S.stack[S.depth++] = s;
    S.trans = ui_anim_on() ? 0 : 1;
    S.trans_dir = 1;
}

static void pop(void)
{
    if (S.depth <= 1) return;
    S.trans_from = top();
    S.depth--;
    S.trans = ui_anim_on() ? 0 : 1;
    S.trans_dir = -1;
}

static void list_reset(list_t *l)
{
    l->sel = 0;
    l->sel_y = l->scroll = 0;
    l->sel_since = esp_timer_get_time();
}

/* ---------------- list contents ---------------- */

static int game_list_count(card_t c)
{
    switch (c) {
        case CARD_RECENT: return lib_recent_count();
        case CARD_FAVS: return lib_fav_count();
        case CARD_NES: return games_count(SYS_NES);
        case CARD_GB: return games_count(SYS_GB);
        case CARD_BUILTIN: return BUILTIN_COUNT;
        default: return 0;
    }
}

static const game_t *game_list_get(card_t c, int i)
{
    switch (c) {
        case CARD_RECENT: return lib_recent_get(i);
        case CARD_FAVS: return lib_fav_get(i);
        case CARD_NES: return games_get(SYS_NES, i);
        case CARD_GB: return games_get(SYS_GB, i);
        default: return NULL;
    }
}

/* ---------------- settings ---------------- */

enum { SET_BRIGHT, SET_SCREEN, SET_STICK, SET_SWAP, SET_PAL, SET_NES, SET_FPS, SET_ANIM,
       SET_RESCAN, SET_UPDATE, SET_ABOUT, SET_COUNT };
static const char *const rot_names[ROT_COUNT] = { "Landscape", "Portrait", "Land 180", "Port 180" };

static void setting_text(int i, char *name, char *val, size_t n)
{
    val[0] = 0;
    switch (i) {
        case SET_BRIGHT: strcpy(name, "Brightness"); snprintf(val, n, "%d%%", g_settings.brightness); break;
        case SET_SCREEN: strcpy(name, "Screen"); strcpy(val, rot_names[g_settings.rotation & 3]); break;
        case SET_STICK:  strcpy(name, "Stick"); snprintf(val, n, "%d deg", g_settings.stick_rot * 90); break;
        case SET_SWAP:   strcpy(name, "Swap red/blue"); strcpy(val, g_settings.swap_rb ? "On" : "Off"); break;
        case SET_PAL:    strcpy(name, "GB colors"); strcpy(val, gb_palette_names[g_settings.gb_palette]); break;
        case SET_NES:    strcpy(name, "NES view"); strcpy(val, g_settings.nes_scale ? "Trim" : "Full"); break;
        case SET_FPS:    strcpy(name, "Show FPS"); strcpy(val, g_settings.show_fps ? "On" : "Off"); break;
        case SET_ANIM:   strcpy(name, "Animations"); strcpy(val, g_settings.ui_anim ? "On" : "Off"); break;
        case SET_RESCAN: strcpy(name, "Rescan SD card"); break;
        case SET_UPDATE: {
            strcpy(name, "Update");
            ota_status_t st;
            ota_status(&st);
            if (st.phase == OTA_CHECKING) strcpy(val, "Checking");
            else if (ota_update_available() == 1) snprintf(val, n, "Get v%s", st.latest);
            else if (ota_update_available() == 0) strcpy(val, "Up to date");
            else if (st.phase == OTA_FAILED) strcpy(val, "Failed");
            else strcpy(val, "Check");
            break;
        }
        case SET_ABOUT:  strcpy(name, "Version"); strcpy(val, FW_VERSION); break;
    }
}

static void apply_display(void)
{
    display_set_rotation(g_settings.rotation, g_settings.swap_rb);
    display_set_backlight(g_settings.brightness);
}

static void rescan(void);

static void setting_change(int i, int dir)
{
    switch (i) {
        case SET_BRIGHT:
            g_settings.brightness += dir * 10;
            if (g_settings.brightness > 100) g_settings.brightness = dir > 0 ? 10 : 100;
            if (g_settings.brightness < 10) g_settings.brightness = 100;
            break;
        case SET_SCREEN: g_settings.rotation = (g_settings.rotation + dir + ROT_COUNT) % ROT_COUNT; break;
        case SET_STICK:  g_settings.stick_rot = (g_settings.stick_rot + dir + 4) % 4; break;
        case SET_SWAP:   g_settings.swap_rb ^= 1; break;
        case SET_PAL:    g_settings.gb_palette = (g_settings.gb_palette + dir + GB_PALETTE_COUNT) % GB_PALETTE_COUNT; break;
        case SET_NES:    g_settings.nes_scale ^= 1; break;
        case SET_FPS:    g_settings.show_fps ^= 1; break;
        case SET_ANIM:   g_settings.ui_anim ^= 1; break;
        case SET_RESCAN: rescan(); return;
        case SET_UPDATE:
            if (wifi_state() != WIFI_CONNECTED) { ui_toast("Connect to Wi-Fi first"); return; }
            ota_online_start(ota_update_available() == 1);
            return;
        default: return;
    }
    apply_display();
    settings_save();
}

/* ---------------- Wi-Fi menu ---------------- */

enum { WM_SCAN, WM_HOTSPOT, WM_QR, WM_POWER, WM_FORGET, WM_COUNT };

static void wifi_text(int i, char *name, char *val, size_t n)
{
    val[0] = 0;
    switch (i) {
        case WM_SCAN:    strcpy(name, "Find networks"); break;
        case WM_HOTSPOT: strcpy(name, "Hotspot"); strcpy(val, wifi_hotspot_on() ? "On" : "Off"); break;
        case WM_QR:      strcpy(name, "Show QR code"); break;
        case WM_POWER:   strcpy(name, "Wi-Fi"); strcpy(val, wifi_enabled() ? "On" : "Off"); break;
        case WM_FORGET:  strcpy(name, "Forget network"); break;
    }
}

static wifi_ap_t s_nets[24];
static int s_net_count;

/* ---------------- drawing helpers ---------------- */

static void draw_header(uint16_t *fb, const uint16_t *icon, const char *title, const char *right, uint32_t color)
{
    gfx_vgradient(fb, 0, 0, W, HEAD_H, color, ((color >> 1) & 0x7f7f7f));
    gfx_icon(fb, 2, 1, icon, 16, 1);
    gfx_text(fb, 21, 6, title, C_TEXT, (W - 50) / CHAR_W);
    if (right) gfx_text(fb, W - 3 - gfx_text_width(right, 1), 6, right, RGB(230, 230, 240), 0);
    gfx_rect(fb, 0, HEAD_H, W, 1, RGB(0, 0, 0));
}

static void draw_footer(uint16_t *fb, const char *hint)
{
    gfx_rect(fb, 0, H - FOOT_H, W, FOOT_H, RGB(22, 24, 40));
    gfx_text(fb, 3, H - 8, hint, C_DIM, (W - 6) / CHAR_W);
}

typedef void (*row_fn)(int i, char *label, char *value, size_t n, const uint16_t **icon);

/* Draws a list with a sliding highlight, smooth scrolling and marquee. */
static void draw_list(uint16_t *fb, list_t *l, int count, row_fn fn, int y0, int y1, float dt, uint32_t color)
{
    int area = y1 - y0, visible = area / ROW_H;
    float target_scroll = l->scroll;
    float sel_px = l->sel * ROW_H;
    if (sel_px - target_scroll < ROW_H) target_scroll = sel_px - ROW_H;
    if (sel_px - target_scroll > area - 2 * ROW_H) target_scroll = sel_px - area + 2 * ROW_H;
    float max_scroll = count * ROW_H - area;
    if (target_scroll > max_scroll) target_scroll = max_scroll;
    if (target_scroll < 0) target_scroll = 0;
    l->scroll = ui_approach(l->scroll, target_scroll, 18, dt);
    l->sel_y = ui_approach(l->sel_y, sel_px, 24, dt);

    int first = (int)(l->scroll / ROW_H);
    char label[128], value[40];
    for (int i = first; i < count && i <= first + visible + 1; i++) {
        int y = y0 + i * ROW_H - (int)l->scroll;
        if (y + ROW_H <= y0 || y >= y1) continue;
        bool sel = i == l->sel;
        if (sel) {
            int hy = y0 + (int)(l->sel_y - l->scroll);
            gfx_round_rect(fb, 1, hy, W - 2, ROW_H, gfx_lerp_color(color, 0x2a3060, 0.45f));
            gfx_rect(fb, 1, hy + 2, 2, ROW_H - 4, C_ACCENT);
        }
        const uint16_t *icon = NULL;
        fn(i, label, value, sizeof(value), &icon);
        int x = 6, max = (W - 12) / CHAR_W;
        bool bars = value[0] == '\x01';
        int vw = bars ? 16 : value[0] ? gfx_text_width(value, 1) + 6 : 0;
        if (icon) vw += 10;
        max -= (vw + CHAR_W - 1) / CHAR_W;
        int len = strlen(label), off = 0;
        if (sel && len > max) {
            int steps = len - max;
            int t = (int)((esp_timer_get_time() - l->sel_since) / 150000) - 6;
            t = t < 0 ? 0 : t % (steps + 12);
            off = t > steps ? steps : t;
        }
        if (y < y0) continue;   /* header/panel above stays clean */
        gfx_text(fb, x, y + 2, label + off, sel ? C_TEXT : RGB(170, 175, 200), max);
        int rx = W - 5;
        if (bars) {
            rx -= 11;
            for (int b = 0; b < 4; b++)
                gfx_rect(fb, rx + b * 3, y + 7 - b * 2, 2, 2 + b * 2, b < value[1] - '0' ? (sel ? C_ACCENT : C_TEXT) : RGB(60, 64, 90));
            rx -= 4;
        } else if (value[0]) {
            rx -= gfx_text_width(value, 1);
            gfx_text(fb, rx, y + 2, value, sel ? C_ACCENT : C_DIM, 0);
            rx -= 4;
        }
        if (icon) gfx_icon(fb, rx - 8, y + 1, icon, 8, 1);
    }
    if (count > visible) {
        int bh = area * visible / count;
        if (bh < 6) bh = 6;
        int by = y0 + (int)((area - bh) * (l->scroll / (max_scroll > 0 ? max_scroll : 1)));
        gfx_rect(fb, W - 2, by, 2, bh, RGB(90, 96, 140));
    }
}

/* ---------------- HOME ---------------- */

static void card_subtitle(card_t c, char *out, size_t n)
{
    int k;
    switch (c) {
        case CARD_RECENT: k = lib_recent_count(); snprintf(out, n, k ? "%d played" : "Nothing yet", k); break;
        case CARD_FAVS:   k = lib_fav_count(); snprintf(out, n, k ? "%d games" : "START to add", k); break;
        case CARD_NES:    k = games_count(SYS_NES); snprintf(out, n, "%d game%s", k, k == 1 ? "" : "s"); break;
        case CARD_GB:     k = games_count(SYS_GB); snprintf(out, n, "%d game%s", k, k == 1 ? "" : "s"); break;
        case CARD_BUILTIN: snprintf(out, n, "%d game", BUILTIN_COUNT); break;
        case CARD_WIFI:
            switch (wifi_state()) {
                case WIFI_CONNECTED: { char ip[16]; wifi_ip(ip, sizeof(ip)); snprintf(out, n, "%s", ip); break; }
                case WIFI_CONNECTING: snprintf(out, n, "Connecting..."); break;
                case WIFI_HOTSPOT: snprintf(out, n, "Hotspot on"); break;
                default: snprintf(out, n, "Off"); break;
            }
            break;
        default: snprintf(out, n, "v" FW_VERSION); break;
    }
}

static void draw_home(uint16_t *fb, float dt)
{
    /* background tint follows the camera between two cards */
    int a = (int)floorf(S.home_cam), b = a + 1;
    float f = S.home_cam - a;
    if (a < 0) a = 0;
    if (b >= CARD_COUNT) b = CARD_COUNT - 1;
    uint32_t ca = cards[a].color, cb = cards[b].color;
    uint32_t tint = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int x = (ca >> sh) & 255, y = (cb >> sh) & 255;
        tint |= (uint32_t)(x + (int)((y - x) * f)) << sh;
    }
    ui_draw_background(fb, tint, S.t);
    ui_draw_statusbar(fb, "RETRO POCKET", RGB((tint >> 16) & 255, (tint >> 8) & 255, tint & 255));

    float spacing = W * 0.46f;
    int cy = UI_BAR_H + (H - UI_BAR_H) * 40 / 100;
    for (int pass = 0; pass < 2; pass++) {          /* far cards first, selected on top */
        for (int i = 0; i < CARD_COUNT; i++) {
            float d = i - S.home_cam;
            if (fabsf(d) > 2.2f) continue;
            bool center = fabsf(d) < 0.5f;
            if (center != (pass == 1)) continue;
            int scale = center ? 3 : 2;
            int size = 16 * scale;
            int cx = W / 2 + (int)(d * spacing);
            int bob = center ? (int)(sinf(S.t * 3) * 2) : 0;
            int x = cx - size / 2, y = cy - size / 2 + bob + (center ? 0 : 6);
            if (center) {
                int pw = size + 14, ph = size + 14;
                uint16_t glow = RGB((cards[i].color >> 16) & 255, (cards[i].color >> 8) & 255, cards[i].color & 255);
                gfx_rect_alpha(fb, cx - pw / 2 - 2, cy - ph / 2 - 2 + bob, pw + 4, ph + 4, glow, 60);
                gfx_round_rect(fb, cx - pw / 2, cy - ph / 2 + bob, pw, ph, RGB(24, 26, 44));
                gfx_frame(fb, cx - pw / 2 + 1, cy - ph / 2 + bob + 1, pw - 2, ph - 2, glow);
            }
            gfx_icon(fb, x, y, cards[i].icon, 16, scale);
            if (!center) gfx_rect_alpha(fb, x, y, size, size, RGB(8, 9, 16), 130);
        }
    }

    /* title + subtitle of the nearest card, sliding with the camera */
    int near = (int)lroundf(S.home_cam);
    float off = (near - S.home_cam) * W * 0.6f;
    float fade = 1.0f - fabsf(near - S.home_cam) * 2;
    if (fade < 0) fade = 0;
    char sub[40];
    card_subtitle(near, sub, sizeof(sub));
    int ty = cy + 34;
    int tw = gfx_text_width(cards[near].title, 2);
    int scale = tw > W - 8 ? 1 : 2;
    gfx_text_center_scaled(fb, W / 2 + (int)off, ty, cards[near].title,
                           gfx_lerp_color(0x202230, 0xf0f0f6, fade), scale);
    gfx_text_center_scaled(fb, W / 2 + (int)off, ty + 18, sub, gfx_lerp_color(0x202230, 0x9aa0c0, fade), 1);

    /* page dots */
    int dw = CARD_COUNT * 7 + 6, dx = (W - dw) / 2, dy = H - 7;
    S.dots_x = ui_approach(S.dots_x, S.home_sel * 7, 16, dt);
    for (int i = 0; i < CARD_COUNT; i++) gfx_rect(fb, dx + i * 7 + (i > S.home_sel ? 6 : 0), dy, 3, 3, RGB(70, 74, 100));
    gfx_round_rect(fb, dx + (int)S.dots_x, dy - 1, 9, 5, C_ACCENT);
}

/* ---------------- LIST ---------------- */

static void game_row(int i, char *label, char *value, size_t n, const uint16_t **icon)
{
    value[0] = 0;
    if (S.list_card == CARD_BUILTIN) { snprintf(label, 128, "%s", builtins[i].name); return; }
    const game_t *g = game_list_get(S.list_card, i);
    if (!g) { label[0] = 0; return; }
    snprintf(label, 128, "%s", g->name);
    if (S.list_card == CARD_RECENT || S.list_card == CARD_FAVS)
        snprintf(value, n, "%s", g->sys == SYS_NES ? "NES" : "GB");
    if (lib_is_fav(g->path)) *icon = icon_smallstar;
}

static void draw_game_list(uint16_t *fb, float dt)
{
    card_t c = S.list_card;
    uint32_t col = cards[c].color;
    gfx_vgradient(fb, 0, 0, W, H, 0x141729, 0x090a12);
    int n = game_list_count(c);
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d", n);
    if (n == 0) {
        gfx_icon(fb, W / 2 - 16, H / 2 - 30 + (int)(sinf(S.t * 2) * 2), cards[c].icon, 16, 2);
        const char *l1 = "No games here yet", *l2 = "Copy .zip .nes .gb files";
        const char *l3 = "to the SD card or web UI";
        if (c == CARD_RECENT) { l1 = "Nothing played yet"; l2 = "Games you play"; l3 = "show up here"; }
        if (c == CARD_FAVS)   { l1 = "No favorites yet"; l2 = "Press START on a game"; l3 = "to add it here"; }
        gfx_text_center(fb, H / 2 + 8, l1, C_TEXT);
        gfx_text_center(fb, H / 2 + 20, l2, C_DIM);
        gfx_text_center(fb, H / 2 + 30, l3, C_DIM);
    } else {
        draw_list(fb, &S.list, n, game_row, HEAD_H + 2, H - FOOT_H - 1, dt, col);
        if (c == CARD_BUILTIN)
            gfx_text_wrap(fb, 4, H - FOOT_H - 22, W - 8, builtins[S.list.sel].desc, C_DIM);
    }
    draw_header(fb, cards[c].icon, cards[c].title, cnt, col);
    draw_footer(fb, c == CARD_BUILTIN ? "A Play  B Back" : W < 150 ? "A Play  START Fav" : "A Play B Back START Fav");
}

/* ---------------- SETTINGS / WIFI / NETS / QR ---------------- */

static void settings_row(int i, char *label, char *value, size_t n, const uint16_t **icon)
{
    setting_text(i, label, value, n);
}

static void draw_settings(uint16_t *fb, float dt)
{
    gfx_vgradient(fb, 0, 0, W, H, 0x141729, 0x090a12);
    draw_list(fb, &S.settings, SET_COUNT, settings_row, HEAD_H + 2, H - FOOT_H - 1, dt, cards[CARD_SETTINGS].color);
    draw_header(fb, icon_gear, "Settings", NULL, cards[CARD_SETTINGS].color);
    draw_footer(fb, "</> Change  B Back");
}

static void wifi_row(int i, char *label, char *value, size_t n, const uint16_t **icon)
{
    wifi_text(i, label, value, n);
}

static void draw_wifi(uint16_t *fb, float dt)
{
    gfx_vgradient(fb, 0, 0, W, H, 0x141729, 0x090a12);
    /* status panel */
    int py = HEAD_H + 3;
    gfx_round_rect(fb, 3, py, W - 6, 38, RGB(26, 30, 52));
    char l1[48], l2[48], l3[48], ip[16], ssid[33];
    wifi_ip(ip, sizeof(ip));
    wifi_ssid(ssid, sizeof(ssid));
    uint16_t c1 = C_TEXT;
    l2[0] = l3[0] = 0;
    switch (wifi_state()) {
        case WIFI_CONNECTED:
            snprintf(l1, sizeof(l1), "%s", ssid);
            snprintf(l2, sizeof(l2), "Web UI:");
            snprintf(l3, sizeof(l3), "http://%s", ip);
            c1 = C_GREEN;
            break;
        case WIFI_CONNECTING: snprintf(l1, sizeof(l1), "Joining %s", ssid); break;
        case WIFI_HOTSPOT:
            snprintf(l1, sizeof(l1), "%s", wifi_ap_ssid());
            snprintf(l2, sizeof(l2), "Password: %s", wifi_ap_pass());
            snprintf(l3, sizeof(l3), "Open " WIFI_AP_IP);
            c1 = C_ACCENT;
            break;
        default: snprintf(l1, sizeof(l1), "Wi-Fi is off"); break;
    }
    if (wifi_state() == WIFI_CONNECTING) ui_draw_spinner(fb, W - 14, py + 19, S.t, C_ACCENT);
    gfx_text(fb, 7, py + 4, l1, c1, (W - 28) / CHAR_W);
    gfx_text(fb, 7, py + 15, l2, C_DIM, (W - 12) / CHAR_W);
    gfx_text(fb, 7, py + 26, l3, C_ACCENT, (W - 12) / CHAR_W);
    draw_list(fb, &S.wifi, WM_COUNT, wifi_row, py + 41, H - FOOT_H - 1, dt, cards[CARD_WIFI].color);
    draw_header(fb, icon_wifi, "Wi-Fi", NULL, cards[CARD_WIFI].color);
    draw_footer(fb, "A Select  B Back");
}

static void net_row(int i, char *label, char *value, size_t n, const uint16_t **icon)
{
    int r = s_nets[i].rssi, bars = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
    snprintf(label, 128, "%s", s_nets[i].ssid);
    value[0] = '\x01';              /* draw_list: signal bars */
    value[1] = '0' + bars;
    value[2] = 0;
    if (s_nets[i].secure) *icon = icon_lock;
}

static void draw_nets(uint16_t *fb, float dt)
{
    gfx_vgradient(fb, 0, 0, W, H, 0x141729, 0x090a12);
    if (S.nets.sel_since == 0) {   /* results not in yet */
        ui_draw_spinner(fb, W / 2, H / 2 - 6, S.t, C_ACCENT);
        gfx_text_center(fb, H / 2 + 10, "Looking for networks", C_DIM);
    } else if (s_net_count == 0) {
        gfx_text_center(fb, H / 2 - 8, "No networks found", C_TEXT);
        gfx_text_center(fb, H / 2 + 4, "START to scan again", C_DIM);
    } else {
        draw_list(fb, &S.nets, s_net_count, net_row, HEAD_H + 2, H - FOOT_H - 1, dt, cards[CARD_WIFI].color);
    }
    draw_header(fb, icon_wifi, "Networks", NULL, cards[CARD_WIFI].color);
    draw_footer(fb, W < 150 ? "A Join START Rescan" : "A Join START Rescan B Back");
}

static void draw_qr(uint16_t *fb, float dt)
{
    gfx_vgradient(fb, 0, 0, W, H, 0x141729, 0x090a12);
    char text[120], l1[48], l2[48], ip[16];
    wifi_ip(ip, sizeof(ip));
    bool can_ap = wifi_hotspot_on(), can_url = ip[0] != 0;
    int page = S.qr_page;
    if (page == 0 && !can_ap) page = 1;
    if (page == 1 && !can_url) page = 0;
    if (page == 0 && can_ap) {
        snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", wifi_ap_ssid(), wifi_ap_pass());
        snprintf(l1, sizeof(l1), "1. Scan to join hotspot");
        snprintf(l2, sizeof(l2), "2. Open " WIFI_AP_IP);
    } else if (can_url) {
        snprintf(text, sizeof(text), "http://%s", ip);
        snprintf(l1, sizeof(l1), "Scan to open web UI");
        snprintf(l2, sizeof(l2), "http://%s", ip);
    } else {
        gfx_text_center(fb, H / 2 - 4, "Turn on Wi-Fi first", C_DIM);
        draw_header(fb, icon_wifi, "QR code", NULL, cards[CARD_WIFI].color);
        return;
    }
    int avail = (W < H - HEAD_H - 26 ? W : H - HEAD_H - 26) - 4;
    int qy = HEAD_H + 2 + avail / 2 + 1;
    gfx_qr(fb, W / 2, qy, text, avail);
    gfx_text_center(fb, H - 20, l1, C_TEXT);
    gfx_text_center(fb, H - 10, l2, C_ACCENT);
    draw_header(fb, icon_wifi, "QR code", can_ap && can_url ? "</>" : NULL, cards[CARD_WIFI].color);
}

static void draw_screen(scr_t s, uint16_t *fb, float dt)
{
    switch (s) {
        case SCR_HOME: draw_home(fb, dt); break;
        case SCR_LIST: draw_game_list(fb, dt); break;
        case SCR_SETTINGS: draw_settings(fb, dt); break;
        case SCR_WIFI: draw_wifi(fb, dt); break;
        case SCR_NETS: draw_nets(fb, dt); break;
        case SCR_QR: draw_qr(fb, dt); break;
    }
}

/* ---------------- scanning, launching ---------------- */

static void scan_progress(int files, int games, const char *current)
{
    uint16_t *fb = display_fb();
    char buf[40];
    float t = ui_time();
    gfx_set_origin(0, 0);
    gfx_vgradient(fb, 0, 0, W, H, 0x1c2040, 0x090a14);
    int cy = H / 2;
    ui_draw_spinner(fb, W / 2, cy - 22, t, C_ACCENT);
    gfx_text_center(fb, cy - 4, "Looking for games", C_TEXT);
    snprintf(buf, sizeof(buf), "%d found", games);
    gfx_text_center(fb, cy + 10, buf, C_ACCENT);
    snprintf(buf, sizeof(buf), "%d files checked", files);
    gfx_text_center(fb, cy + 20, buf, C_DIM);
    gfx_text(fb, 4, H - 12, current, RGB(80, 84, 110), (W - 8) / CHAR_W);
    display_present();
}

static void rescan(void)
{
    scan_progress(0, 0, "");
    games_scan(scan_progress);
    int n = game_list_count(S.list_card);
    if (S.list.sel >= n) S.list.sel = n ? n - 1 : 0;
}

static void loading_screen(const char *name, const uint16_t *icon)
{
    uint16_t *fb = display_fb();
    gfx_set_origin(0, 0);
    gfx_vgradient(fb, 0, 0, W, H, 0x1c2040, 0x090a14);
    gfx_icon(fb, W / 2 - 24, H / 2 - 40, icon, 16, 3);
    gfx_text_center(fb, H / 2 + 14, "Loading...", C_ACCENT);
    int len = strlen(name), max = (W - 8) / CHAR_W;
    gfx_text(fb, len > max ? 4 : (W - gfx_text_width(name, 1)) / 2, H / 2 + 26, name, C_DIM, max);
    display_present();
    display_wait();
}

static void after_game(void)
{
    bridge_set_now_playing("");
    bridge_clear_quit();
    gfx_set_origin(0, 0);
    ui_dt(); /* don't animate the time spent in-game */
}

static void play_game(const game_t *g)
{
    if (!g) return;
    char path[320], name[96];
    snprintf(path, sizeof(path), "%s", g->path);  /* the library may be rescanned while playing */
    snprintf(name, sizeof(name), "%s", g->name);
    int sys = g->sys;
    lib_add_recent(path);
    bridge_set_now_playing(name);
    bridge_clear_quit();
    loading_screen(name, sys == SYS_NES ? icon_nes : icon_gb);
    if (sys == SYS_NES) emu_nes_run(path);
    else emu_gb_run(path);
    after_game();
}

static void play_builtin(int i)
{
    bridge_set_now_playing(builtins[i].name);
    bridge_clear_quit();
    builtins[i].run(NULL);
    after_game();
}

/* SELECT in a system list: jump to the next starting letter. */
static void jump_letter(void)
{
    int n = game_list_count(S.list_card);
    if (n < 2 || S.list_card == CARD_BUILTIN) return;
    int cur = toupper((unsigned char)game_list_get(S.list_card, S.list.sel)->name[0]);
    for (int k = 1; k < n; k++) {
        int i = (S.list.sel + k) % n;
        if (toupper((unsigned char)game_list_get(S.list_card, i)->name[0]) != cur) { S.list.sel = i; return; }
    }
}

/* ---------------- input per screen ---------------- */

static void list_nav(list_t *l, int n, uint32_t r)
{
    if (!n) return;
    int old = l->sel;
    if (r & BTN_UP) l->sel = (l->sel + n - 1) % n;
    if (r & BTN_DOWN) l->sel = (l->sel + 1) % n;
    if (l->sel != old) l->sel_since = esp_timer_get_time();
}

static void open_card(card_t c)
{
    switch (c) {
        case CARD_WIFI: list_reset(&S.wifi); push(SCR_WIFI); break;
        case CARD_SETTINGS: list_reset(&S.settings); push(SCR_SETTINGS); break;
        default:
            if (S.list_card != c) list_reset(&S.list);
            S.list_card = c;
            push(SCR_LIST);
            break;
    }
}

static void handle_input(void)
{
    input_poll();
    uint32_t r = input_repeat(), p = input_pressed();
    switch (top()) {
        case SCR_HOME:
            if ((r & BTN_LEFT) && S.home_sel > 0) S.home_sel--;
            if ((r & BTN_RIGHT) && S.home_sel < CARD_COUNT - 1) S.home_sel++;
            if (p & (BTN_A | BTN_START)) open_card(S.home_sel);
            break;
        case SCR_LIST: {
            int n = game_list_count(S.list_card);
            list_nav(&S.list, n, r);
            if (p & BTN_B) { pop(); break; }
            if (!n) break;
            if (p & BTN_A) {
                if (S.list_card == CARD_BUILTIN) play_builtin(S.list.sel);
                else play_game(game_list_get(S.list_card, S.list.sel));
            }
            if ((p & BTN_START) && S.list_card != CARD_BUILTIN) {
                const game_t *g = game_list_get(S.list_card, S.list.sel);
                bool fav = !lib_is_fav(g->path);
                char path[320];
                snprintf(path, sizeof(path), "%s", g->path);
                lib_set_fav(path, fav);
                ui_toast(fav ? "Added to favorites" : "Removed from favorites");
                n = game_list_count(S.list_card);
                if (S.list.sel >= n) S.list.sel = n ? n - 1 : 0;
            }
            if (p & BTN_SELECT) jump_letter();
            break;
        }
        case SCR_SETTINGS:
            list_nav(&S.settings, SET_COUNT, r);
            if (p & BTN_B) { pop(); break; }
            if (r & BTN_RIGHT) setting_change(S.settings.sel, +1);
            if (r & BTN_LEFT) setting_change(S.settings.sel, -1);
            if (p & BTN_A) setting_change(S.settings.sel, +1);
            break;
        case SCR_WIFI:
            list_nav(&S.wifi, WM_COUNT, r);
            if (p & BTN_B) { pop(); break; }
            if (p & BTN_A) {
                switch (S.wifi.sel) {
                    case WM_SCAN:
                        if (!wifi_enabled()) { ui_toast("Turn Wi-Fi on first"); break; }
                        s_net_count = 0;
                        list_reset(&S.nets);
                        S.nets.sel_since = 0;
                        wifi_scan_start();
                        push(SCR_NETS);
                        break;
                    case WM_HOTSPOT: wifi_set_hotspot(!wifi_hotspot_on()); break;
                    case WM_QR: S.qr_page = wifi_hotspot_on() ? 0 : 1; push(SCR_QR); break;
                    case WM_POWER: wifi_set_enabled(!wifi_enabled()); break;
                    case WM_FORGET: wifi_forget(); ui_toast("Network forgotten"); break;
                }
            }
            break;
        case SCR_NETS:
            if (S.nets.sel_since == 0 && !wifi_scan_busy()) {
                s_net_count = wifi_scan_results(s_nets, 24);
                S.nets.sel_since = esp_timer_get_time();
            }
            list_nav(&S.nets, s_net_count, r);
            if (p & BTN_B) { pop(); break; }
            if (p & BTN_START) { s_net_count = 0; S.nets.sel_since = 0; wifi_scan_start(); break; }
            if ((p & BTN_A) && s_net_count) {
                wifi_ap_t *ap = &s_nets[S.nets.sel];
                char pass[65] = "", title[48];
                snprintf(title, sizeof(title), "Password: %s", ap->ssid);
                if (!ap->secure || ui_keyboard(title, pass, sizeof(pass))) {
                    wifi_connect(ap->ssid, pass);
                    char m[64];
                    snprintf(m, sizeof(m), "Joining %s...", ap->ssid);
                    ui_toast(m);
                    pop();
                }
                ui_dt();
            }
            break;
        case SCR_QR:
            if (p & BTN_B) pop();
            if (r & (BTN_LEFT | BTN_RIGHT)) S.qr_page ^= 1;
            break;
    }
}

/* ---------------- requests from the web UI ---------------- */

static void handle_requests(void)
{
    uint32_t req = bridge_take_requests();
    if (req & REQ_APPLY_SETTINGS) { apply_display(); settings_save(); }
    if (req & REQ_RESCAN) rescan();
    if (req & REQ_OTA_ONLINE) ota_online_start(true);
    char path[320];
    if (bridge_take_launch(path, sizeof(path))) {
        bridge_clear_quit();
        const game_t *g = games_find(path);
        if (!g) { rescan(); g = games_find(path); }
        if (g) play_game(g);
        else ui_toast("Not a game I can run");
    }
}

/* Full-screen progress while a firmware update installs. */
static bool draw_ota_overlay(uint16_t *fb)
{
    ota_status_t st;
    ota_status(&st);
    if (st.phase != OTA_DOWNLOADING && st.phase != OTA_DONE) return false;
    gfx_set_origin(0, 0);
    gfx_vgradient(fb, 0, 0, W, H, 0x1c2040, 0x090a14);
    gfx_icon(fb, W / 2 - 16, H / 2 - 44, icon_gear, 16, 2);
    gfx_text_center(fb, H / 2 - 6, st.phase == OTA_DONE ? "Update installed!" : "Updating firmware", C_TEXT);
    int bw = W - 30;
    gfx_round_rect(fb, 15, H / 2 + 8, bw, 8, RGB(30, 33, 56));
    gfx_round_rect(fb, 15, H / 2 + 8, bw * st.percent / 100 > 4 ? bw * st.percent / 100 : 4, 8, C_ACCENT);
    char pct[32];
    snprintf(pct, sizeof(pct), st.phase == OTA_DONE ? "Restarting..." : "%d%%  don't turn off", st.percent);
    gfx_text_center(fb, H / 2 + 22, pct, C_DIM);
    return true;
}

/* ---------------- boot ---------------- */

static void splash(void)
{
    const float dur = ui_anim_on() ? 1.5f : 0.4f;
    float t0 = ui_time();
    const char *w1 = "RETRO", *w2 = "POCKET";
    for (;;) {
        float t = ui_time() - t0;
        if (t > dur) break;
        uint16_t *fb = display_fb();
        gfx_set_origin(0, 0);
        ui_draw_background(fb, 0x3040a0, t);
        int cy = H / 2 - 14;
        /* letters drop in one by one */
        for (int line = 0; line < 2; line++) {
            const char *w = line ? w2 : w1;
            int n = strlen(w), x0 = (W - gfx_text_width(w, 2)) / 2;
            for (int i = 0; i < n; i++) {
                float lt = (t - (line * 5 + i) * 0.06f) / 0.35f;
                if (lt <= 0) continue;
                int y = cy + line * 18 - (int)((1 - ui_ease_out_back(lt)) * 30);
                char s[2] = { w[i], 0 };
                gfx_text_scaled(fb, x0 + i * CHAR_W * 2 + 1, y + 1, s, RGB(0, 0, 0), 0, 2);
                gfx_text_scaled(fb, x0 + i * CHAR_W * 2, y, s, line ? C_ACCENT : C_TEXT, 0, 2);
            }
        }
        float bt = ui_ease_out_cubic((t - 0.6f) / 0.6f);
        int bw = (int)((W - 50) * bt);
        if (bw > 0) gfx_rect(fb, (W - bw) / 2, cy + 36, bw, 2, C_ACCENT);
        if (t > 0.9f) gfx_text_center(fb, cy + 44, "v" FW_VERSION, C_DIM);
        display_present();
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

/* ---------------- main loop ---------------- */

void launcher_run(void)
{
    memset(&S, 0, sizeof(S));
    S.stack[0] = SCR_HOME;
    S.depth = 1;
    S.trans = 1;
    splash();
    rescan();
    /* start on the first card that has something in it */
    S.home_sel = lib_recent_count() ? CARD_RECENT : games_count(SYS_NES) ? CARD_NES
               : games_count(SYS_GB) ? CARD_GB : CARD_BUILTIN;
    S.home_cam = S.home_sel;
    S.dots_x = S.home_sel * 7;
    ui_dt();

    for (;;) {
        float dt = ui_dt();
        S.t += dt;
        handle_requests();
        if (S.trans >= 1) handle_input();
        else input_poll();
        S.home_cam = ui_approach(S.home_cam, S.home_sel, 12, dt);

        uint16_t *fb = display_fb();
        if (!draw_ota_overlay(fb)) {
            if (S.trans < 1) {
                S.trans += dt / 0.28f;
                if (S.trans > 1) S.trans = 1;
                float e = ui_ease_out_cubic(S.trans);
                scr_t from = S.trans_from, to = top();
                int d = S.trans_dir;
                /* outgoing screen moves away a bit slower (parallax) */
                gfx_set_origin((int)(-d * e * W * 0.5f), 0);
                draw_screen(from, fb, dt);
                gfx_set_origin((int)(d * (1 - e) * W), 0);
                draw_screen(to, fb, dt);
                /* soft shadow on the leading edge of the incoming screen */
                gfx_set_origin(0, 0);
                int edge = d > 0 ? (int)((1 - e) * W) - 3 : (int)(e * W);
                gfx_rect_alpha(fb, edge, 0, 3, H, C_BLACK, 110);
            } else {
                gfx_set_origin(0, 0);
                draw_screen(top(), fb, dt);
            }
        }
        gfx_set_origin(0, 0);
        ui_draw_toast(fb, dt);
        display_present();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}
