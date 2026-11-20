/*
 * ota.c - firmware updates over Wi-Fi.
 *
 * Online: downloads <path>/version.txt and <path>/retro-pocket.bin from a
 * GitHub repo through the GitHub API (works for public repos, and private
 * ones with a read-only token). Upload: the web UI streams a .bin in.
 * With app rollback enabled, a new firmware that crashes before reaching
 * the menu is undone automatically on the next boot.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ota.h"
#include "version.h"
#include "bridge.h"
#include "lock.h"
#include "wifi.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

static ota_status_t s_st = { .phase = OTA_IDLE };
static int s_available = -1;
static volatile bool s_busy;

static esp_ota_handle_t s_up_handle;
static const esp_partition_t *s_up_part;

/* ---------------- source config ---------------- */

static void defaults(ota_source_t *s)
{
    memset(s, 0, sizeof(*s));
    strcpy(s->repo, "ilix1000/Misc");
    strcpy(s->branch, "ccr-39f5922b-51eoij");
    strcpy(s->path, "retro-pocket/firmware/ota");
}

void ota_source_get(ota_source_t *out)
{
    defaults(out);
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(*out);
    ota_source_t tmp;
    if (nvs_get_blob(h, "src", &tmp, &n) == ESP_OK && n == sizeof(tmp)) *out = tmp;
    nvs_close(h);
}

void ota_source_set(const ota_source_t *src)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "src", src, sizeof(*src));
    nvs_commit(h);
    nvs_close(h);
    s_available = -1;
}

void ota_mark_valid(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware %s confirmed", FW_VERSION);
        bridge_toast("Updated to v" FW_VERSION);
    }
}

/* ---------------- status ---------------- */

static void set_status(ota_phase_t p, int pct, const char *msg)
{
    lib_lock();
    s_st.phase = p;
    s_st.percent = pct;
    if (msg) snprintf(s_st.message, sizeof(s_st.message), "%s", msg);
    lib_unlock();
}

void ota_status(ota_status_t *out)
{
    lib_lock();
    *out = s_st;
    lib_unlock();
}

int ota_update_available(void) { return s_available; }

/* ---------------- online ---------------- */

static void make_url(const ota_source_t *s, const char *file, char *url, size_t n)
{
    snprintf(url, n, "https://api.github.com/repos/%s/contents/%s/%s?ref=%s",
             s->repo, s->path, file, s->branch);
}

static void add_headers(esp_http_client_handle_t c, const ota_source_t *s)
{
    esp_http_client_set_header(c, "Accept", "application/vnd.github.raw+json");
    esp_http_client_set_header(c, "User-Agent", "RetroPocket-OTA");
    esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");
    if (s->token[0]) {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", s->token);
        esp_http_client_set_header(c, "Authorization", auth);
    }
}

static ota_source_t s_src; /* used by the http init callback */

static esp_err_t https_init_cb(esp_http_client_handle_t c)
{
    add_headers(c, &s_src);
    return ESP_OK;
}

/* "2.0.1" > "2.0.0" ? */
static int vercmp(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
    return 0;
}

static bool fetch_version(char *out, size_t n, int *http_status)
{
    char url[300];
    make_url(&s_src, "version.txt", url, sizeof(url));
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
        .buffer_size = 2048, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    add_headers(c, &s_src);
    bool ok = false;
    *http_status = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        *http_status = esp_http_client_get_status_code(c);
        int len = esp_http_client_read_response(c, out, n - 1);
        if (*http_status == 200 && len > 0) {
            out[len] = 0;
            out[strcspn(out, " \r\n")] = 0;
            ok = out[0] != 0;
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void online_task(void *arg)
{
    bool install = (bool)(intptr_t)arg;
    ota_source_get(&s_src);
    set_status(OTA_CHECKING, 0, "Checking for updates...");

    char ver[24];
    int status;
    if (!fetch_version(ver, sizeof(ver), &status)) {
        char m[64];
        if (status == 404 || status == 401 || status == 403)
            snprintf(m, sizeof(m), "GitHub said %d (private repo? add a token)", status);
        else
            snprintf(m, sizeof(m), status ? "GitHub error %d" : "No internet connection", status);
        set_status(OTA_FAILED, 0, m);
        s_available = -1;
        goto out;
    }
    lib_lock();
    snprintf(s_st.latest, sizeof(s_st.latest), "%s", ver);
    lib_unlock();
    s_available = vercmp(ver, FW_VERSION) > 0;
    if (!s_available) {
        set_status(OTA_IDLE, 0, "Up to date (v" FW_VERSION ")");
        goto out;
    }
    if (!install) {
        char m[64];
        snprintf(m, sizeof(m), "Update available: v%s", ver);
        set_status(OTA_IDLE, 0, m);
        goto out;
    }

    char url[300];
    make_url(&s_src, "retro-pocket.bin", url, sizeof(url));
    esp_http_client_config_t http = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
        .buffer_size = 4096, .buffer_size_tx = 1024, .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http, .http_client_init_cb = https_init_cb };
    esp_https_ota_handle_t h = NULL;
    set_status(OTA_DOWNLOADING, 0, "Downloading...");
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        int total = esp_https_ota_get_image_size(h);
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int got = esp_https_ota_get_image_len_read(h);
            if (total > 0) set_status(OTA_DOWNLOADING, got * 100 / total, NULL);
        }
        if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
            err = esp_https_ota_finish(h);
            h = NULL;
        }
    }
    if (h) esp_https_ota_abort(h);
    if (err == ESP_OK) {
        set_status(OTA_DONE, 100, "Installed! Restarting...");
        ota_reboot_soon();
    } else {
        char m[64];
        snprintf(m, sizeof(m), "Update failed: %s", esp_err_to_name(err));
        set_status(OTA_FAILED, 0, m);
    }
out:
    s_busy = false;
    vTaskDelete(NULL);
}

void ota_online_start(bool install)
{
    if (s_busy) return;
    if (wifi_state() != WIFI_CONNECTED) {
        set_status(OTA_FAILED, 0, "Connect to Wi-Fi with internet first");
        return;
    }
    s_busy = true;
    xTaskCreatePinnedToCore(online_task, "ota", 8192, (void *)(intptr_t)install, 4, NULL, 0);
}

/* ---------------- upload ---------------- */

bool ota_upload_begin(size_t size)
{
    if (s_busy) return false;
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part || esp_ota_begin(s_up_part, size, &s_up_handle) != ESP_OK) return false;
    s_busy = true;
    set_status(OTA_DOWNLOADING, 0, "Receiving upload...");
    return true;
}

bool ota_upload_write(const void *data, size_t len)
{
    return esp_ota_write(s_up_handle, data, len) == ESP_OK;
}

bool ota_upload_end(void)
{
    bool ok = esp_ota_end(s_up_handle) == ESP_OK && esp_ota_set_boot_partition(s_up_part) == ESP_OK;
    s_busy = false;
    set_status(ok ? OTA_DONE : OTA_FAILED, ok ? 100 : 0,
               ok ? "Installed! Restarting..." : "Not a valid firmware file");
    if (ok) ota_reboot_soon();
    return ok;
}

void ota_upload_abort(void)
{
    esp_ota_abort(s_up_handle);
    s_busy = false;
    set_status(OTA_FAILED, 0, "Upload interrupted");
}

static void reboot_cb(void *arg) { esp_restart(); }

void ota_reboot_soon(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1200000);
}
