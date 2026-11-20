idf_component_register(
    SRCS "main.c" "display.c" "input.c" "gfx.c" "system.c" "storage.c" "launcher.c"
         "emu_gb.c" "emu_nes.c" "game_maze.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc esp_lcd fatfs sdmmc nvs_flash esp_timer
)
# Emulator cores are third-party code: be fast, and don't fail on their warnings.
set_source_files_properties(emu_gb.c emu_nes.c PROPERTIES COMPILE_OPTIONS "-O3;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-maybe-uninitialized;-Wno-type-limits")
