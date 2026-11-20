/*
 * Retro Pocket - a tiny Retro-Go style handheld firmware for ESP32-S3.
 */
#include "display.h"
#include "input.h"
#include "system.h"
#include "launcher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void app_task(void *arg)
{
    launcher_run();
}

void app_main(void)
{
    settings_load();
    display_init();
    display_set_backlight(g_settings.brightness);
    input_init();
    if (!sd_init()) ESP_LOGW("main", "running without SD card");

    /* Emulators run on core 1, away from Wi-Fi/system tasks on core 0. */
    xTaskCreatePinnedToCore(app_task, "app", 32 * 1024, NULL, 5, NULL, 1);
}
