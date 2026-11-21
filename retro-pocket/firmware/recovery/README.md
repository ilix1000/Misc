# Recovery images

Full images: flash any of them over USB at offset **0x0**. Wi-Fi, settings
and saves are kept.

- `retro-pocket-v2.2.1-full.bin`, `retro-pocket-v2.3.0-full.bin` - older
  releases from before Doom (v2.4.0), for getting a board running again.
- `retro-pocket-v2.7.0-safe-full.bin` - current features, but keeps all of
  its memory in internal RAM and boots even if the PSRAM chip isn't found.
  It also logs each boot step on the serial console (115200 baud).
