#include <string.h>
#include "gfx.h"

void gfx_fill(uint16_t *fb, uint16_t c)
{
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) fb[i] = c;
}

void gfx_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
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

static void glyph(uint16_t *fb, int x, int y, unsigned char ch, uint16_t c)
{
    if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT) ch = '?';
    const uint8_t *g = font5x7[ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if (px >= 0 && px < LCD_WIDTH && (g[row] & (0x10 >> col)))
                fb[py * LCD_WIDTH + px] = c;
        }
    }
}

int gfx_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int max_chars)
{
    for (int n = 0; *s && (max_chars <= 0 || n < max_chars); n++, s++) {
        glyph(fb, x, y, (unsigned char)*s, c);
        x += CHAR_W;
    }
    return x;
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
    gfx_text(fb, (LCD_WIDTH - (int)strlen(s) * CHAR_W) / 2, y, s, c, 0);
}

void gfx_dim(uint16_t *fb)
{
    /* Pixels are byte-swapped RGB565; un-swap, halve each channel, swap back. */
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint16_t p = SWAP16(fb[i]);
        p = (p >> 1) & 0x7BEF;
        p = (p >> 1) & 0x7BEF;
        fb[i] = SWAP16(p);
    }
}
