/*
 * storage.c - NVS settings and SD card mount (ESP-IDF only; the PC simulator
 * has its own version in sim/).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "system.h"
#include "board.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"

static const char *TAG = "storage";

/* ---------------- settings (NVS) ---------------- */

settings_t g_settings = { .brightness = 80, .gb_palette = 0, .nes_scale = 0, .show_fps = 0, .ui_anim = 1 };

void settings_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    if (nvs_open("retropocket", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_blob_compat(h, "settings", &g_settings, sizeof(g_settings));
    nvs_close(h);
    /* clamp anything an older/newer firmware may have left out of range */
    if (g_settings.brightness < 10 || g_settings.brightness > 100) g_settings.brightness = 80;
    g_settings.rotation &= 3;
    g_settings.stick_rot &= 3;
    if (g_settings.gb_palette < 0 || g_settings.gb_palette > 4) g_settings.gb_palette = 0;
}

/* Reads a struct saved by any firmware version. Settings structs only ever
 * grow at the end, so an older (shorter) blob fills the fields it knows and
 * the new fields keep their defaults; a newer (longer) one is truncated.
 * This is what keeps your settings across updates. */
bool nvs_get_blob_compat(nvs_handle_t h, const char *key, void *dst, size_t dst_size)
{
    size_t stored = 0;
    if (nvs_get_blob(h, key, NULL, &stored) != ESP_OK || stored == 0) return false;
    uint8_t *tmp = malloc(stored);
    if (!tmp) return false;
    bool ok = nvs_get_blob(h, key, tmp, &stored) == ESP_OK;
    if (ok) memcpy(dst, tmp, stored < dst_size ? stored : dst_size);
    free(tmp);
    return ok;
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("retropocket", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "settings", &g_settings, sizeof(g_settings));
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- SD card ---------------- */

static bool s_sd_ok;

bool sd_ok(void) { return s_sd_ok; }

bool sd_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = SD_SPI_KHZ;
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI, .miso_io_num = PIN_SD_MISO, .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI3_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mc, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }
    sdmmc_card_print_info(stdout, card);
    /* Games can go anywhere on the card; /roms is just a suggestion. */
    const char *dirs[] = { DIR_ROMS, DIR_SAVES, DIR_SAVES "/nes", DIR_SAVES "/gb",
                           DIR_SAVES "/sms", DIR_SAVES "/pce" };
    for (int i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) mkdir(dirs[i], 0777);
    s_sd_ok = true;
    return true;
}

