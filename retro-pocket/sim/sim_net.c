/* Simulator stand-ins for Wi-Fi, OTA and the web server.
 * SIM_WIFI=sta (connected, default) | ap (hotspot) | off */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi.h"
#include "ota.h"
#include "webui.h"
#include "bridge.h"
#include "esp_timer.h"

static wifi_state_t st = WIFI_CONNECTED;
static bool ap, en = true;
static int64_t scan_until;
static char ssid[33] = "HomeNetwork";

void wifi_start(void)
{
    const char *m = getenv("SIM_WIFI");
    if (m && !strcmp(m, "ap")) { st = WIFI_HOTSPOT; ap = true; ssid[0] = 0; }
    if (m && !strcmp(m, "off")) { st = WIFI_OFF; en = false; }
}
void wifi_set_enabled(bool on) { en = on; st = on ? WIFI_CONNECTED : WIFI_OFF; }
bool wifi_enabled(void) { return en; }
wifi_state_t wifi_state(void) { return st; }
bool wifi_hotspot_on(void) { return ap; }
void wifi_set_hotspot(bool on) { ap = on; if (st != WIFI_CONNECTED) st = on ? WIFI_HOTSPOT : WIFI_OFF; }
int wifi_rssi(void) { return st == WIFI_CONNECTED ? -58 : 0; }
int wifi_bars(void) { return st == WIFI_CONNECTED ? 3 : 0; }
void wifi_ip(char *o, int n) { snprintf(o, n, "%s", st == WIFI_CONNECTED ? "192.168.1.42" : ""); }
void wifi_ssid(char *o, int n) { snprintf(o, n, "%s", ssid); }
const char *wifi_ap_ssid(void) { return "RetroPocket-1A2B"; }
const char *wifi_ap_pass(void) { return "retropocket"; }
bool wifi_scan_start(void) { scan_until = esp_timer_get_time() + 1500000; return true; }
bool wifi_scan_busy(void) { return esp_timer_get_time() < scan_until; }
int wifi_scan_results(wifi_ap_t *out, int max)
{
    static const wifi_ap_t f[] = { { "HomeNetwork", -48, true }, { "Pizzeria Guest", -63, false },
        { "Neighbour 5G", -71, true }, { "FRITZ!Box 7530 XY", -80, true }, { "Phone Hotspot", -55, true } };
    int n = 5 < max ? 5 : max;
    memcpy(out, f, n * sizeof(*out));
    return n;
}
void wifi_connect(const char *s, const char *p)
{
    snprintf(ssid, sizeof(ssid), "%s", s);
    st = WIFI_CONNECTED;
    fprintf(stderr, "wifi_connect('%s', '%s')\n", s, p);
    bridge_toast("Wi-Fi connected: 192.168.1.42");
}
void wifi_forget(void) { ssid[0] = 0; st = WIFI_HOTSPOT; ap = true; }

static ota_status_t ost;
void ota_source_get(ota_source_t *o) { memset(o, 0, sizeof(*o)); }
void ota_source_set(const ota_source_t *s) {}
void ota_mark_valid(void) {}
void ota_status(ota_status_t *o)
{
    if (getenv("SIM_OTA")) { ost.phase = OTA_DOWNLOADING; ost.percent = atoi(getenv("SIM_OTA")); }
    *o = ost;
}
void ota_online_start(bool install) { ost.phase = OTA_IDLE; strcpy(ost.latest, "2.0.1"); }
int ota_update_available(void) { return ost.latest[0] ? 1 : -1; }
void webui_start(void) {}
