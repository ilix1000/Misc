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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void app_task(void *arg)
{
    /* We got this far, so a freshly installed update works: keep it. */
    ota_mark_valid();
    ESP_LOGI("main", "launcher");
    launcher_run();
}

void app_main(void)
{
    ESP_LOGI("main", "Retro Pocket starting");
    lib_lock_init();
    settings_load();
    ESP_LOGI("main", "settings loaded");
    display_init();
    display_set_rotation(g_settings.rotation, g_settings.swap_rb);
    display_set_backlight(g_settings.brightness);
    input_init();
    ESP_LOGI("main", "display + input ready");
    if (!sd_init()) ESP_LOGW("main", "running without SD card");

    /* Start the menu task before Wi-Fi so its 20 KB stack is guaranteed to
     * fit in internal RAM (it saves settings to flash, which needs that). */
    ESP_LOGI("main", "internal RAM free: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (xTaskCreatePinnedToCore(app_task, "app", 20 * 1024, NULL, 5, NULL, 1) != pdPASS)
        ESP_LOGE("main", "can't start the menu task");

    /* Wi-Fi and the web server live on core 0. */
    wifi_start();
    webui_start();
    ESP_LOGI("main", "network started, internal RAM free: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
