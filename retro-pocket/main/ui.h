#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Seconds since the previous call (clamped), for frame-rate independent animation. */
float ui_dt(void);
/* Seconds since boot. */
float ui_time(void);

/* Moves `cur` toward `target` smoothly (exponential ease). speed ~ 10..25.
 * Snaps instantly when animations are turned off in Settings. */
float ui_approach(float cur, float target, float speed, float dt);
bool ui_anim_on(void);
float ui_ease_out_cubic(float t);
float ui_ease_out_back(float t);

/* Pop-up messages that slide down from the top. Also shows messages posted
 * through bridge_toast() (Wi-Fi events, web UI...). */
void ui_toast(const char *msg);
void ui_draw_toast(uint16_t *fb, float dt);

/* 12 px top bar: title on the left, Wi-Fi state on the right. */
#define UI_BAR_H 12
void ui_draw_statusbar(uint16_t *fb, const char *title, uint16_t color);

/* Animated background tinted with the given colour (0xRRGGBB). */
void ui_draw_background(uint16_t *fb, uint32_t tint, float t);

/* Spinning dots. */
void ui_draw_spinner(uint16_t *fb, int cx, int cy, float t, uint16_t c);

/* Full-screen on-screen keyboard. Returns false if cancelled. */
bool ui_keyboard(const char *title, char *buf, size_t n);
