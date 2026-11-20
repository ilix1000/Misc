#pragma once
#include <stdint.h>
#include "board.h"

/* Pixels are RGB565, stored byte-swapped so they can go straight to the SPI
 * bus. Always build colours with RGB() so you never have to think about it. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define SWAP16(c)       ((uint16_t)(((c) << 8) | ((c) >> 8)))
#define RGB(r, g, b)    SWAP16(RGB565(r, g, b))

void display_init(void);
void display_set_backlight(int percent);

/* Double-buffered: draw into display_fb(), then display_present() sends it
 * over DMA in the background and hands you the other buffer. */
uint16_t *display_fb(void);
void display_present(void);
/* The frame most recently passed to display_present(). */
const uint16_t *display_front(void);
/* Blocks until the last frame has finished sending. */
void display_wait(void);
