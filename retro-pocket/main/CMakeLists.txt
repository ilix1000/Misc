idf_component_register(
    SRCS "main.c" "display.c" "input.c" "gfx.c" "system.c" "storage.c" "launcher.c"
         "romscan.c" "unzip.c" "ui.c" "bridge.c" "wifi.c" "webui.c" "ota.c"
         "emu_gb.c" "emu_gbc.c" "emu_nes.c" "game_maze.c" "cores/qrcodegen.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES driver esp_adc fatfs sdmmc nvs_flash esp_timer esp_wifi esp_netif esp_event
             esp_http_server esp_http_client esp_https_ota app_update json mbedtls
)
# Emulator cores are third-party code: be fast, and don't fail on their warnings.
set_source_files_properties(emu_gb.c emu_gbc.c emu_nes.c PROPERTIES COMPILE_OPTIONS "-O3;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-maybe-uninitialized;-Wno-type-limits")
