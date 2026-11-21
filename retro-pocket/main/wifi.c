/*
 * wifi.c - station + hotspot management.
 *
 * Saved network -> join it. No saved network, or 20 s without a connection ->
 * also open our own hotspot "RetroPocket-XXXX" (password "retropocket") so the
 * web UI is always reachable at http://192.168.4.1 for setup. Once the router
 * connection works, an automatically opened hotspot closes again.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wifi.h"
#include "i18n.h"
#include "bridge.h"
#include "lock.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define NVS_NS "wifi"
#define FALLBACK_US   (20 * 1000000)
#define AP_LINGER_US  (15 * 1000000) /* let the web page show the new IP first */
#define MAX_SCAN 24

static esp_netif_t *s_sta, *s_ap;
static bool s_started, s_enabled = true;
static volatile wifi_state_t s_state = WIFI_OFF;
static bool s_ap_on, s_ap_pinned;     /* pinned = user turned it on by hand */
static char s_ssid[33], s_pass[65];
static char s_ap_ssid[33];
static const char s_ap_pass[] = "retropocket";
static char s_ip[16];
static esp_timer_handle_t s_fallback_timer, s_retry_timer, s_ap_off_timer;
static int s_retries;

static wifi_ap_t s_scan[MAX_SCAN];
static int s_scan_count;
static volatile bool s_scanning;

/* ---------------- credentials ---------------- */

static void creds_load(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = 0;
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    uint8_t en = 1;
    if (nvs_get_u8(h, "enabled", &en) == ESP_OK) s_enabled = en;
    nvs_close(h);
}

static void creds_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", s_ssid);
    nvs_set_str(h, "pass", s_pass);
    nvs_set_u8(h, "enabled", s_enabled);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- mode switching ---------------- */

static void apply_mode(void)
{
    wifi_mode_t mode = s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_wifi_set_mode(mode);
    if (s_ap_on) {
        wifi_config_t ap = { 0 };
        strcpy((char *)ap.ap.ssid, s_ap_ssid);
        ap.ap.ssid_len = strlen(s_ap_ssid);
        strcpy((char *)ap.ap.password, s_ap_pass);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.max_connection = 4;
        ap.ap.channel = 1;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (s_state != WIFI_CONNECTED && s_state != WIFI_CONNECTING)
        s_state = s_ap_on ? WIFI_HOTSPOT : WIFI_OFF;
}

static void hotspot(bool on, bool pinned)
{
    if (on == s_ap_on) { if (on && pinned) s_ap_pinned = true; return; }
    s_ap_on = on;
    s_ap_pinned = on && pinned;
    apply_mode();
    if (on) {
        char m[64];
        snprintf(m, sizeof(m), T("Hotspot %s on"), s_ap_ssid);
        bridge_toast(m);
    }
}

static void sta_connect(void)
{
    if (!s_ssid[0]) return;
    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strncpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_state = WIFI_CONNECTING;
    esp_wifi_connect();
    esp_timer_stop(s_fallback_timer);
    esp_timer_start_once(s_fallback_timer, FALLBACK_US);
}

static void fallback_cb(void *arg)
{
    if (s_state != WIFI_CONNECTED) {
        ESP_LOGW(TAG, "can't reach '%s', opening hotspot", s_ssid);
        hotspot(true, false);
    }
}

static void retry_cb(void *arg)
{
    if (s_state == WIFI_CONNECTING && s_ssid[0]) esp_wifi_connect();
}

static void ap_off_cb(void *arg)
{
    if (s_state == WIFI_CONNECTED && !s_ap_pinned) {
        s_ap_on = false;
        apply_mode();
    }
}

/* ---------------- events ---------------- */

static int ap_cmp(const void *a, const void *b)
{
    return ((const wifi_ap_t *)b)->rssi - ((const wifi_ap_t *)a)->rssi;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        sta_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = s_state == WIFI_CONNECTED;
        s_ip[0] = 0;
        if (s_ssid[0] && s_enabled) {
            s_state = WIFI_CONNECTING;
            int delay = s_retries < 5 ? 1 : 5;   /* seconds */
            s_retries++;
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, delay * 1000000LL);
            if (was_connected) {
                bridge_toast("Wi-Fi lost, reconnecting");
                esp_timer_stop(s_fallback_timer);
                esp_timer_start_once(s_fallback_timer, FALLBACK_US);
            }
        } else {
            s_state = s_ap_on ? WIFI_HOTSPOT : WIFI_OFF;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        lib_lock();
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        lib_unlock();
        s_state = WIFI_CONNECTED;
        s_retries = 0;
        esp_timer_stop(s_fallback_timer);
        char m[64];
        snprintf(m, sizeof(m), T("Wi-Fi connected: %s"), s_ip);
        bridge_toast(m);
        ESP_LOGI(TAG, "%s", m);
        if (s_ap_on && !s_ap_pinned) {
            esp_timer_stop(s_ap_off_timer);
            esp_timer_start_once(s_ap_off_timer, AP_LINGER_US);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        wifi_ap_record_t *rec = calloc(n ? n : 1, sizeof(*rec));
        if (rec && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
            lib_lock();
            s_scan_count = 0;
            for (int i = 0; i < n; i++) {
                const char *ssid = (const char *)rec[i].ssid;
                if (!ssid[0]) continue;                  /* hidden network */
                int dup = -1;
                for (int j = 0; j < s_scan_count; j++)
                    if (!strcmp(s_scan[j].ssid, ssid)) dup = j;
                if (dup >= 0) {
                    if (rec[i].rssi > s_scan[dup].rssi) s_scan[dup].rssi = rec[i].rssi;
                    continue;
                }
                if (s_scan_count == MAX_SCAN) continue;
                wifi_ap_t *a = &s_scan[s_scan_count++];
                strncpy(a->ssid, ssid, 32);
                a->ssid[32] = 0;
                a->rssi = rec[i].rssi;
                a->secure = rec[i].authmode != WIFI_AUTH_OPEN;
            }
            qsort(s_scan, s_scan_count, sizeof(s_scan[0]), ap_cmp);
            lib_unlock();
        } else {
            esp_wifi_clear_ap_list();
        }
        free(rec);
        s_scanning = false;
    }
}

/* ---------------- public ---------------- */

void wifi_start(void)
{
    creds_load();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "RetroPocket-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "retropocket");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* we keep credentials ourselves */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    esp_timer_create_args_t t = { .callback = fallback_cb, .name = "wifi_fb" };
    esp_timer_create(&t, &s_fallback_timer);
    t.callback = retry_cb; t.name = "wifi_retry";
    esp_timer_create(&t, &s_retry_timer);
    t.callback = ap_off_cb; t.name = "wifi_apoff";
    esp_timer_create(&t, &s_ap_off_timer);

    if (s_enabled) wifi_set_enabled(true);
}

void wifi_set_enabled(bool on)
{
    s_enabled = on;
    creds_save();
    if (on && !s_started) {
        s_ap_on = !s_ssid[0];              /* nothing saved: hotspot for setup */
        apply_mode();
        esp_wifi_start();                  /* STA_START event -> connect */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_started = true;
        if (s_ap_on) bridge_toast("Set up Wi-Fi: join the hotspot");
    } else if (!on && s_started) {
        esp_timer_stop(s_fallback_timer);
        esp_timer_stop(s_retry_timer);
        esp_wifi_stop();
        s_started = false;
        s_ap_on = false;
        s_state = WIFI_OFF;
        s_ip[0] = 0;
    }
}

bool wifi_enabled(void) { return s_enabled; }
wifi_state_t wifi_state(void) { return s_state; }
bool wifi_hotspot_on(void) { return s_ap_on; }

void wifi_set_hotspot(bool on)
{
    if (!s_started) return;
    hotspot(on, on);
    if (!on) s_ap_pinned = false;
}

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state != WIFI_CONNECTED || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

int wifi_bars(void)
{
    int r = wifi_rssi();
    if (!r) return 0;
    return r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
}

void wifi_ip(char *out, int n)
{
    lib_lock();
    snprintf(out, n, "%s", s_state == WIFI_CONNECTED ? s_ip : "");
    lib_unlock();
}

void wifi_ssid(char *out, int n)
{
    lib_lock();
    snprintf(out, n, "%s", s_ssid);
    lib_unlock();
}

const char *wifi_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_ap_pass(void) { return s_ap_pass; }

bool wifi_scan_start(void)
{
    if (!s_started || s_scanning) return false;
    wifi_scan_config_t sc = { .show_hidden = false };
    s_scanning = esp_wifi_scan_start(&sc, false) == ESP_OK;
    return s_scanning;
}

bool wifi_scan_busy(void) { return s_scanning; }

int wifi_scan_results(wifi_ap_t *out, int max)
{
    lib_lock();
    int n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(*out));
    lib_unlock();
    return n;
}

void wifi_connect(const char *ssid, const char *pass)
{
    lib_lock();
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    lib_unlock();
    s_enabled = true;
    creds_save();
    if (!s_started) { wifi_set_enabled(true); return; }
    s_retries = 0;
    esp_wifi_disconnect();
    sta_connect();
}

void wifi_forget(void)
{
    lib_lock();
    s_ssid[0] = s_pass[0] = 0;
    lib_unlock();
    creds_save();
    if (!s_started) return;
    esp_timer_stop(s_fallback_timer);
    esp_wifi_disconnect();
    s_state = WIFI_OFF;
    hotspot(true, false);
}
