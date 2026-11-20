#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "board.h"

/* Pixels are RGB565, stored byte-swapped so they can go straight to the SPI
 * bus. Always build colours with RGB() so you never have to think about it. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define SWAP16(c)       ((uint16_t)(((c) << 8) | ((c) >> 8)))
#define RGB(r, g, b)    SWAP16(RGB565(r, g, b))

/* Current logical screen size: 160x128 in landscape, 128x160 in portrait. */
extern int g_lcd_w, g_lcd_h;
#define LCD_WIDTH   g_lcd_w
#define LCD_HEIGHT  g_lcd_h
#define LCD_PIXELS  (LCD_PANEL_LONG * LCD_PANEL_SHORT)
#define LCD_MAX_DIM LCD_PANEL_LONG

enum { ROT_LANDSCAPE, ROT_PORTRAIT, ROT_LANDSCAPE_180, ROT_PORTRAIT_180, ROT_COUNT };

void display_init(void);
/* Changes orientation (ROT_*) and colour order. Safe to call any time. */
void display_set_rotation(int rot, bool swap_rb);
void display_set_backlight(int percent);

/* Double-buffered: draw into display_fb(), then display_present() sends it
 * over DMA in the background and hands you the other buffer. */
uint16_t *display_fb(void);
void display_present(void);
/* The frame most recently passed to display_present(). */
const uint16_t *display_front(void);
/* Blocks until the last frame has finished sending. */
void display_wait(void);
