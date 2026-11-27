#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Start the OLED (SSD1306 128x64, I2C SDA=GPIO5 SCL=GPIO6). */
bool display_start(void);

/* Called by the decoder for every (filtered) pulse: drives the live signal view. */
void display_on_pulse(bool level, uint32_t duration_us);

/* Called when a code was decoded (Flipper description text). */
void display_on_decoded(const char* protocol, const char* text);
