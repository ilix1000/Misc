#pragma once
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

void gfx_fill(uint16_t *fb, uint16_t c);
void gfx_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
void gfx_frame(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
/* Draws text; stops at max_chars (0 = no limit). Returns the x after the text. */
int gfx_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int max_chars);
/* Word-wraps text inside a box of width w. Returns the y below the text. */
int gfx_text_wrap(uint16_t *fb, int x, int y, int w, const char *s, uint16_t c);
void gfx_text_center(uint16_t *fb, int y, const char *s, uint16_t c);
/* Darkens the whole frame (used behind pop-up menus). */
void gfx_dim(uint16_t *fb);
