#pragma once
#include <stdint.h>

enum {
    BTN_UP     = 1 << 0,
    BTN_DOWN   = 1 << 1,
    BTN_LEFT   = 1 << 2,
    BTN_RIGHT  = 1 << 3,
    BTN_A      = 1 << 4,
    BTN_B      = 1 << 5,
    BTN_START  = 1 << 6,
    BTN_SELECT = 1 << 7,
    BTN_MENU   = 1 << 8,   /* joystick click */
};

void input_init(void);
/* Reads the hardware. Call once per frame. Returns the held buttons. */
uint32_t input_poll(void);
/* Buttons that went down on the latest input_poll(). */
uint32_t input_pressed(void);
/* Like input_pressed(), but held directions/buttons auto-repeat (for menus). */
uint32_t input_repeat(void);
/* Raw joystick, -1.0 .. 1.0 each axis (after calibration and inversion). */
void input_joystick(float *x, float *y);
