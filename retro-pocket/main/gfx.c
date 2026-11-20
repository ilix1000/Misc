#include <string.h>
#include "gfx.h"
#include "cores/qrcodegen.h"

/* Everything except gfx_fill() is drawn relative to this origin, which lets
 * a whole screen slide in or out during transitions. */
static int s_ox, s_oy;

void gfx_set_origin(int x, int y) { s_ox = x; s_oy = y; }

void gfx_fill(uint16_t *fb, uint16_t c)
{
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) fb[i] = c;
}

/* Clips a rect (already in screen space) to the panel. */
static bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > LCD_WIDTH) *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

void gfx_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    x += s_ox; y += s_oy;
    if (!clip(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = fb + (y + j) * LCD_WIDTH + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void gfx_frame(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(fb, x, y, w, 1, c);
    gfx_rect(fb, x, y + h - 1, w, 1, c);
    gfx_rect(fb, x, y, 1, h, c);
    gfx_rect(fb, x + w - 1, y, 1, h, c);
}

void gfx_round_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    /* 2 px corner radius */
    gfx_rect(fb, x + 2, y, w - 4, 1, c);
    gfx_rect(fb, x + 1, y + 1, w - 2, 1, c);
    gfx_rect(fb, x, y + 2, w, h - 4, c);
    gfx_rect(fb, x + 1, y + h - 2, w - 2, 1, c);
    gfx_rect(fb, x + 2, y + h - 1, w - 4, 1, c);
}

/* Blend in unswapped RGB565: a = 0..32 */
static inline uint16_t blend565(uint16_t dst, uint16_t src, int a)
{
    uint32_t d = SWAP16(dst), s = SWAP16(src);
    uint32_t dd = (d | (d << 16)) & 0x07E0F81F, ss = (s | (s << 16)) & 0x07E0F81F;
    uint32_t r = ((ss - dd) * a / 32 + dd) & 0x07E0F81F;
    return SWAP16((uint16_t)(r | (r >> 16)));
}

void gfx_rect_alpha(uint16_t *fb, int x, int y, int w, int h, uint16_t c, int alpha)
{
    x += s_ox; y += s_oy;
    if (!clip(&x, &y, &w, &h)) return;
    int a = alpha * 32 / 255;
    for (int j = 0; j < h; j++) {
        uint16_t *row = fb + (y + j) * LCD_WIDTH + x;
        for (int i = 0; i < w; i++) row[i] = blend565(row[i], c, a);
    }
}

uint16_t gfx_lerp_color(uint32_t rgb1, uint32_t rgb2, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = ((rgb1 >> 16) & 255) + (int)((((int)(rgb2 >> 16) & 255) - (int)((rgb1 >> 16) & 255)) * t);
    int g = ((rgb1 >> 8) & 255) + (int)((((int)(rgb2 >> 8) & 255) - (int)((rgb1 >> 8) & 255)) * t);
    int b = (rgb1 & 255) + (int)((((int)rgb2 & 255) - (int)(rgb1 & 255)) * t);
    return RGB(r, g, b);
}

void gfx_vgradient(uint16_t *fb, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < h; j++)
        gfx_rect(fb, x, y + j, w, 1, gfx_lerp_color(top, bottom, h > 1 ? (float)j / (h - 1) : 0));
}

static void glyph(uint16_t *fb, int x, int y, unsigned char ch, uint16_t c, int scale)
{
    if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT) ch = '?';
    const uint8_t *g = font5x7[ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        for (int col = 0; col < FONT_W; col++) {
            if (!(g[row] & (0x10 >> col))) continue;
            int px = x + col * scale, py = y + row * scale;
            if (scale == 1) {
                if (px >= 0 && px < LCD_WIDTH && py >= 0 && py < LCD_HEIGHT) fb[py * LCD_WIDTH + px] = c;
            } else {
                int w = scale, h = scale;
                if (clip(&px, &py, &w, &h))
                    for (int j = 0; j < h; j++)
                        for (int i = 0; i < w; i++) fb[(py + j) * LCD_WIDTH + px + i] = c;
            }
        }
    }
}

int gfx_text_scaled(uint16_t *fb, int x, int y, const char *s, uint16_t c, int max_chars, int scale)
{
    int sx = x + s_ox, sy = y + s_oy;
    for (int n = 0; *s && (max_chars <= 0 || n < max_chars); n++, s++) {
        if (sx > LCD_WIDTH) break;
        if (sx + CHAR_W * scale > 0) glyph(fb, sx, sy, (unsigned char)*s, c, scale);
        sx += CHAR_W * scale;
    }
    return sx - s_ox;
}

int gfx_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int max_chars)
{
    return gfx_text_scaled(fb, x, y, s, c, max_chars, 1);
}

int gfx_text_width(const char *s, int scale)
{
    return (int)strlen(s) * CHAR_W * scale - scale;
}

int gfx_text_wrap(uint16_t *fb, int x, int y, int w, const char *s, uint16_t c)
{
    int max = w / CHAR_W;
    while (*s) {
        int len = strlen(s), n = len;
        if (len > max) {
            n = max;
            while (n > 0 && s[n] != ' ') n--;
            if (n == 0) n = max;
        }
        gfx_text(fb, x, y, s, c, n);
        s += n;
        while (*s == ' ') s++;
        y += CHAR_H + 1;
    }
    return y;
}

void gfx_text_center(uint16_t *fb, int y, const char *s, uint16_t c)
{
    gfx_text(fb, (LCD_WIDTH - gfx_text_width(s, 1)) / 2, y, s, c, 0);
}

void gfx_text_center_scaled(uint16_t *fb, int cx, int y, const char *s, uint16_t c, int scale)
{
    gfx_text_scaled(fb, cx - gfx_text_width(s, scale) / 2, y, s, c, 0, scale);
}

void gfx_icon(uint16_t *fb, int x, int y, const uint16_t *icon, int size, int scale)
{
    x += s_ox; y += s_oy;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size; i++) {
            uint16_t c = icon[j * size + i];
            if (!c) continue;
            int px = x + i * scale, py = y + j * scale, w = scale, h = scale;
            if (!clip(&px, &py, &w, &h)) continue;
            for (int b = 0; b < h; b++)
                for (int a = 0; a < w; a++) fb[(py + b) * LCD_WIDTH + px + a] = c;
        }
    }
}

void gfx_dim(uint16_t *fb)
{
    /* Pixels are byte-swapped RGB565; un-swap, quarter each channel, swap back. */
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint16_t p = SWAP16(fb[i]);
        p = (p >> 1) & 0x7BEF;
        p = (p >> 1) & 0x7BEF;
        fb[i] = SWAP16(p);
    }
}

int gfx_qr(uint16_t *fb, int cx, int cy, const char *text, int max_px)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, 8, qrcodegen_Mask_AUTO, true)) return 0;
    int n = qrcodegen_getSize(qr), quiet = 2;
    int scale = max_px / (n + quiet * 2);
    if (scale < 1) scale = 1;
    int px = (n + quiet * 2) * scale, x0 = cx - px / 2, y0 = cy - px / 2;
    gfx_rect(fb, x0, y0, px, px, RGB(255, 255, 255));
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y))
                gfx_rect(fb, x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, RGB(0, 0, 0));
    return px;
}
