# ESP32-C3 SuperMini + 1.9" TFT test

Test sketch for a 1.9" ST7789 IPS TFT (170x320, SPI) on an ESP32-C3 SuperMini.

## Wiring

| TFT pin | C3 SuperMini |
|---------|--------------|
| GND     | GND          |
| VCC     | 3.3V         |
| SCL     | GPIO 4       |
| SDA     | GPIO 6       |
| RES     | GPIO 10      |
| DC      | GPIO 3       |
| CS      | GPIO 7       |
| BLK     | GPIO 1       |

This avoids the strapping pins (2, 8, 9) and the USB pins (18, 19). Change the
`#define`s at the top of the sketch if you wire it differently.

## What it tests

Loops through: solid color fills, corner/border alignment (checks the 35 px
panel offset), text sizes and font, lines, shapes, RGB gradients, all 4
rotations, color inversion, PWM backlight fade, full-screen FPS, a bouncing
ball animation, and a status screen (CPU MHz, free heap, uptime). Timings are
printed to serial at 115200. The onboard LED (GPIO 8) blinks once per loop.

## Build

**PlatformIO:** `pio run -t upload && pio device monitor`

**Arduino IDE:** open `tft_test/tft_test.ino`, install *Adafruit ST7789 and
ST7735 Library* and *Adafruit GFX Library*, select **ESP32C3 Dev Module**, and set
**USB CDC On Boot: Enabled** (otherwise you get no serial output).

If upload fails, hold BOOT, tap RESET, release BOOT, then upload again.

## Troubleshooting

- **Colors inverted (white shows black):** add `tft.invertDisplay(false);` after `tft.init(...)`.
- **Image shifted / garbage line on an edge:** your panel uses a different
  offset. Check that `tft.init(170, 320)` is used, not `init(240, 320)`.
- **Noise or random pixels:** lower `SPI_HZ` to 27 MHz or shorten the wires.
- **Blank but backlit:** check DC/CS/RES wiring. Some modules have no CS pin;
  then set `TFT_CS` to -1.
