# ESP32-C3 SuperMini + 1.77" TFT test

Test sketch for a 1.77" ST7735S TFT (128x160, SPI) on an ESP32-C3 SuperMini.
Same wiring and same test sequence as `../c3-tft-1.9`.

| TFT pin | C3 SuperMini |
|---------|--------------|
| GND     | GND          |
| VCC     | 3.3V         |
| SCL     | GPIO 4       |
| SDA     | GPIO 6       |
| RES     | GPIO 10      |
| DC (RS/A0) | GPIO 3    |
| CS      | GPIO 7       |
| BLK     | GPIO 1       |

## Flash prebuilt firmware

`esptool --chip esp32c3 write-flash 0x0 firmware/firmware-merged.bin`

## Troubleshooting

- **Shifted image / garbage line on an edge, or red and blue swapped:** change
  `TFT_TAB` from `INITR_BLACKTAB` to `INITR_GREENTAB`.
- **Plain white screen:** the backlight is on but the controller isn't getting
  commands. Check SCL/SDA/DC/CS/RES wiring.
- **Noise:** lower `SPI_HZ` to 16 MHz.
