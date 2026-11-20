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
#include "system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
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
    nvs_get_blob_compat(h, "src", out, sizeof(*out));
    nvs_close(h);
    out->repo[sizeof(out->repo) - 1] = out->branch[sizeof(out->branch) - 1] = 0;
    out->path[sizeof(out->path) - 1] = out->token[sizeof(out->token) - 1] = 0;
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

static void check_rollback(void);

void ota_mark_valid(void)
{
    check_rollback();
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
    if (msg) snprintf(s_st.message, sizeof(s_st.message), "%.63s", msg);
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
        esp_app_desc_t d;
        if (esp_https_ota_get_img_desc(h, &d) != ESP_OK ||
            strcmp(d.project_name, esp_app_get_description()->project_name)) {
            esp_https_ota_abort(h);
            set_status(OTA_FAILED, 0, "Online file isn't Retro Pocket firmware");
            goto out;
        }
    }
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
/*
 * Accepts either file:
 *  - firmware/ota/retro-pocket.bin        (just the app), or
 *  - firmware/retro-pocket-esp32s3.bin    (the full USB image: bootloader at
 *    0x0, partition table at 0x8000, app at 0x10000) - we skip to the app.
 * Nothing is written until the image is checked to be Retro Pocket firmware,
 * so a wrong file can never replace the working one.
 */

#define HEAD_LEN   (0x10000 + 0x200)
#define APP_OFFSET_IN_FULL_IMAGE 0x10000

static uint8_t *s_head;
static size_t s_head_len, s_total, s_written;
static bool s_up_started;
static char s_up_error[80];
static char s_up_version[32];

const char *ota_upload_error(void) { return s_up_error; }
const char *ota_upload_version(void) { return s_up_version; }

/* Checks the app image at `p` and remembers its version. */
static bool check_app(const uint8_t *p, size_t len)
{
    if (len < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) ||
        p[0] != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(s_up_error, sizeof(s_up_error), "Not an ESP32 firmware file");
        return false;
    }
    const esp_app_desc_t *d = (const esp_app_desc_t *)(p + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(s_up_error, sizeof(s_up_error), "Not an app image (wrong file?)");
        return false;
    }
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d->project_name, me->project_name, sizeof(d->project_name))) {
        snprintf(s_up_error, sizeof(s_up_error), "That's firmware for '%.24s', not Retro Pocket", d->project_name);
        return false;
    }
    snprintf(s_up_version, sizeof(s_up_version), "%.31s", d->version);
    return true;
}

static bool start_writing(void)
{
    size_t skip = 0;
    /* full USB image? partition table magic (0x50AA) sits at 0x8000 */
    if (s_head_len >= HEAD_LEN && s_head[0] == ESP_IMAGE_HEADER_MAGIC &&
        s_head[0x8000] == 0xAA && s_head[0x8001] == 0x50)
        skip = APP_OFFSET_IN_FULL_IMAGE;
    if (!check_app(s_head + skip, s_head_len - skip)) return false;

    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part || esp_ota_begin(s_up_part, OTA_SIZE_UNKNOWN, &s_up_handle) != ESP_OK) {
        snprintf(s_up_error, sizeof(s_up_error), "No room for the update");
        return false;
    }
    s_up_started = true;
    if (esp_ota_write(s_up_handle, s_head + skip, s_head_len - skip) != ESP_OK) {
        snprintf(s_up_error, sizeof(s_up_error), "Flash write failed");
        return false;
    }
    char m[64];
    snprintf(m, sizeof(m), "Installing v%s...", s_up_version);
    set_status(OTA_DOWNLOADING, (int)(s_written * 100 / s_total), m);
    return true;
}

bool ota_upload_begin(size_t size)
{
    if (s_busy) { snprintf(s_up_error, sizeof(s_up_error), "An update is already running"); return false; }
    free(s_head);
    s_head = heap_caps_malloc(HEAD_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_head) { snprintf(s_up_error, sizeof(s_up_error), "Out of memory"); return false; }
    s_head_len = s_written = 0;
    s_total = size;
    s_up_started = false;
    s_up_error[0] = s_up_version[0] = 0;
    s_busy = true;
    set_status(OTA_DOWNLOADING, 0, "Receiving upload...");
    return true;
}

bool ota_upload_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    s_written += len;
    if (!s_up_started) {
        size_t take = HEAD_LEN - s_head_len < len ? HEAD_LEN - s_head_len : len;
        memcpy(s_head + s_head_len, p, take);
        s_head_len += take;
        p += take;
        len -= take;
        if (s_head_len < HEAD_LEN) return true;   /* need more before deciding */
        if (!start_writing()) return false;
    }
    if (len && esp_ota_write(s_up_handle, p, len) != ESP_OK) {
        snprintf(s_up_error, sizeof(s_up_error), "Flash write failed");
        return false;
    }
    if (s_total) set_status(OTA_DOWNLOADING, (int)(s_written * 100 / s_total), NULL);
    return true;
}

bool ota_upload_end(void)
{
    bool ok = s_up_started || start_writing();   /* small files never filled the buffer */
    esp_err_t err = ESP_OK;
    if (ok) {
        err = esp_ota_end(s_up_handle);
        s_up_started = false;
        if (err == ESP_OK) err = esp_ota_set_boot_partition(s_up_part);
        if (err != ESP_OK) {
            ok = false;
            snprintf(s_up_error, sizeof(s_up_error), err == ESP_ERR_OTA_VALIDATE_FAILED
                     ? "Firmware file is damaged (incomplete upload?)" : "Install failed: %s", esp_err_to_name(err));
        }
    }
    if (!ok && s_up_started) { esp_ota_abort(s_up_handle); s_up_started = false; }
    free(s_head);
    s_head = NULL;
    s_busy = false;
    char m[64];
    snprintf(m, sizeof(m), "Installed v%s! Restarting...", s_up_version);
    set_status(ok ? OTA_DONE : OTA_FAILED, ok ? 100 : 0, ok ? m : s_up_error);
    if (ok) ota_reboot_soon();
    return ok;
}

void ota_upload_abort(void)
{
    if (s_up_started) esp_ota_abort(s_up_handle);
    s_up_started = false;
    free(s_head);
    s_head = NULL;
    s_busy = false;
    set_status(OTA_FAILED, 0, s_up_error[0] ? s_up_error : "Upload interrupted");
}

/* ---------------- after reboot ---------------- */

static char s_rollback[96];
const char *ota_rollback_info(void) { return s_rollback; }

/* If the last update couldn't boot, the bootloader went back to this
 * version; tell the user once instead of failing silently. */
static void check_rollback(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (!bad) return;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(bad, &d) != ESP_OK) return;
    snprintf(s_rollback, sizeof(s_rollback), "Update to v%.20s didn't start, kept v" FW_VERSION, d.version);
    nvs_handle_t h;
    char seen[33] = "";
    size_t n = sizeof(seen);
    char key[33];
    for (int i = 0; i < 16; i++) sprintf(key + i * 2, "%02x", d.app_elf_sha256[i]);
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, "rb_seen", seen, &n) != ESP_OK || strcmp(seen, key)) {
        bridge_toast(s_rollback);
        nvs_set_str(h, "rb_seen", key);
        nvs_commit(h);
    }
    nvs_close(h);
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
