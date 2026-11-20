#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "display.h"
#include "font5x7.h"

#define CHAR_W 6   /* 5 px glyph + 1 px spacing */
#define CHAR_H 8

/* UI theme */
#define C_BG      RGB(16, 18, 32)
#define C_PANEL   RGB(34, 38, 64)
#define C_TEXT    RGB(230, 232, 240)
#define C_DIM     RGB(130, 136, 160)
#define C_ACCENT  RGB(255, 196, 40)
#define C_SEL     RGB(70, 90, 200)
#define C_RED     RGB(230, 70, 70)
#define C_GREEN   RGB(80, 200, 120)
#define C_BLACK   RGB(0, 0, 0)

/* All drawing below (except gfx_fill) is offset by this origin. */
void gfx_set_origin(int x, int y);

void gfx_fill(uint16_t *fb, uint16_t c);
void gfx_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
void gfx_frame(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
void gfx_round_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
/* alpha 0..255 */
void gfx_rect_alpha(uint16_t *fb, int x, int y, int w, int h, uint16_t c, int alpha);
void gfx_vgradient(uint16_t *fb, int x, int y, int w, int h, uint32_t top_rgb, uint32_t bottom_rgb);
/* Mixes two 0xRRGGBB colours, t = 0..1, returns a panel colour. */
uint16_t gfx_lerp_color(uint32_t rgb1, uint32_t rgb2, float t);

/* Draws text; stops at max_chars (0 = no limit). Returns the x after the text. */
int gfx_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int max_chars);
int gfx_text_scaled(uint16_t *fb, int x, int y, const char *s, uint16_t c, int max_chars, int scale);
int gfx_text_width(const char *s, int scale);
/* Word-wraps text inside a box of width w. Returns the y below the text. */
int gfx_text_wrap(uint16_t *fb, int x, int y, int w, const char *s, uint16_t c);
void gfx_text_center(uint16_t *fb, int y, const char *s, uint16_t c);
void gfx_text_center_scaled(uint16_t *fb, int cx, int y, const char *s, uint16_t c, int scale);

/* size x size icon from icons.h, 0 pixels are transparent. */
void gfx_icon(uint16_t *fb, int x, int y, const uint16_t *icon, int size, int scale);

/* Darkens the whole frame (used behind pop-up menus). */
void gfx_dim(uint16_t *fb);

/* QR code centred at (cx, cy), at most max_px wide. Returns its size. */
int gfx_qr(uint16_t *fb, int cx, int cy, const char *text, int max_px);
