#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "u8g2.h"

/* SSD1306 128x64 OLED on I2C (SCL=GPIO5, SDA=GPIO6, addr 0x3C). */
bool display_init(void);

/* The u8g2 render context drawing into the off-screen buffer. */
u8g2_t* display_u8g2(void);

/* Push the current buffer to the panel. */
void display_flush(void);
