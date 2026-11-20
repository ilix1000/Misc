/*
 * Retro Pocket - a Retro-Go style handheld firmware for ESP32-S3.
 */
#include "display.h"
#include "input.h"
#include "system.h"
#include "launcher.h"
#include "lock.h"
#include "ota.h"
#include "webui.h"
#include "wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void app_task(void *arg)
{
    /* We got this far, so a freshly installed update works: keep it. */
    ota_mark_valid();
    launcher_run();
}

void app_main(void)
{
    lib_lock_init();
    settings_load();
    display_init();
    display_set_rotation(g_settings.rotation, g_settings.swap_rb);
    display_set_backlight(g_settings.brightness);
    input_init();
    if (!sd_init()) ESP_LOGW("main", "running without SD card");

    /* Wi-Fi and the web server live on core 0... */
    wifi_start();
    webui_start();

    /* ...games and the menu on core 1. */
    xTaskCreatePinnedToCore(app_task, "app", 20 * 1024, NULL, 5, NULL, 1);
}
