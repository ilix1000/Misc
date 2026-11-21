/*
 * emu_gbc.c - Game Boy Color games, on froggestspirit's Color-capable fork of
 * Peanut-GB (MIT, cores/peanut_gbc.h). Its functions share names with the
 * original core, so they're renamed here to live side by side.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define __gb_draw_line gbc_gb_draw_line
#define __gb_execute_cb gbc_gb_execute_cb
#define __gb_read gbc_gb_read
#define __gb_step_cpu gbc_gb_step_cpu
#define __gb_write gbc_gb_write
#define gb_colour_hash gbc_gb_colour_hash
#define gb_get_rom_name gbc_gb_get_rom_name
#define gb_get_save_size gbc_gb_get_save_size
#define gb_init gbc_gb_init
#define gb_init_lcd gbc_gb_init_lcd
#define gb_init_serial gbc_gb_init_serial
#define gb_reset gbc_gb_reset
#define gb_run_frame gbc_gb_run_frame
#define gb_set_rtc gbc_gb_set_rtc
#define gb_tick_rtc gbc_gb_tick_rtc

#define ENABLE_SOUND 0
#include "cores/peanut_gbc.h"
#undef LCD_WIDTH
#undef LCD_HEIGHT
#define LCD_HEIGHT_GB 144

/* same joypad bit layout as Peanut-GB, the fork just doesn't name them */
#define JOYPAD_A      0x01
#define JOYPAD_B      0x02
#define JOYPAD_SELECT 0x04
#define JOYPAD_START  0x08
#define JOYPAD_RIGHT  0x10
#define JOYPAD_LEFT   0x20
#define JOYPAD_UP     0x40
#define JOYPAD_DOWN   0x80

#include "emulators.h"
#include "romscan.h"
#include "system.h"
#include "bridge.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define GB_FRAME_US 16743 /* 59.73 Hz */
#define GB_CGB 1
#define GB_STATE_MAGIC 0x47424331 /* "GBC1" */
#define GB_RUN_DECL void emu_gbc_run

#include "emu_gb_impl.h"
