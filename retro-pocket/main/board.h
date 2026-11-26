/*
 * board.h - wiring for the Retro Pocket handheld.
 *
 * Change these to match how you wired your ESP32-S3. The defaults avoid the
 * pins an ESP32-S3-N16R8 / N8R8 module (octal PSRAM) uses internally
 * (GPIO 26-37), the USB pins (19/20) and the strapping pins (0, 3, 45, 46).
 */
#pragma once

/* ---------- 1.77" ST7735 TFT (SPI2) ---------- */
#define PIN_LCD_MOSI   11   /* "SDA" / "DIN" on most modules */
#define PIN_LCD_SCLK   12   /* "SCL" / "CLK" */
#define PIN_LCD_CS     10
#define PIN_LCD_DC      9   /* "A0" / "RS" / "DC" */
#define PIN_LCD_RST     8   /* -1 if tied to EN/3V3 */
#define PIN_LCD_BL      7   /* "LED" / "BLK". -1 if tied to 3V3 */

#define LCD_SPI_HZ     (40 * 1000 * 1000) /* drop to 26 MHz if you see garbage */

/* Panel geometry: 128 x 160. Landscape (160x128) or portrait (128x160) is
 * picked at runtime in Settings > Screen. */
#define LCD_PANEL_LONG  160
#define LCD_PANEL_SHORT 128
/* Some 1.77" panels are offset by 1-2 px (values are for landscape). */
#define LCD_X_OFFSET   0
#define LCD_Y_OFFSET   0
#define LCD_BGR        1    /* default colour order; Settings > Swap red/blue flips it */
#define LCD_INVERT     0    /* 1 if colours look like a photo negative */

/* ---------- micro SD card (SPI3, separate bus) ---------- */
#define PIN_SD_MOSI    15
#define PIN_SD_SCLK    16
#define PIN_SD_MISO    17
#define PIN_SD_CS      18
#define SD_SPI_KHZ     20000

/* ---------- Buttons (wired between GPIO and GND, internal pull-ups) ---------- */
#define PIN_BTN_A       4
#define PIN_BTN_B       5
#define PIN_BTN_START   6
#define PIN_BTN_SELECT 21

/* ---------- Analog joystick module (power it from 3V3, NOT 5V) ---------- */
#ifdef JOY_ALT_PINS
/* "joy3-14" firmware variant: stick on GPIO 3 (ADC1) and 14 (ADC2) */
#define PIN_JOY_X       3
#define PIN_JOY_Y      14
#define FW_VARIANT_DIR  "ota-joy3-14"   /* its own OTA channel */
#else
#define PIN_JOY_X       1   /* ADC1 channel 0 */
#define PIN_JOY_Y       2   /* ADC1 channel 1 */
#define FW_VARIANT_DIR  "ota"
#endif
#define PIN_JOY_SW     47   /* joystick push = MENU button */
#define JOY_INVERT_X    0
#define JOY_INVERT_Y    0
#define JOY_SWAP_XY     0   /* 1 if your module is mounted rotated 90 degrees */
#define JOY_DEADZONE    0.35f /* fraction of full travel */

/* ---------- Storage layout on the SD card ---------- */
#ifndef SD_MOUNT
#define SD_MOUNT        "/sd"
#endif
#define DIR_ROMS        SD_MOUNT "/roms"
#define DIR_SAVES       SD_MOUNT "/saves"
