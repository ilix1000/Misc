#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_OFF,
    WIFI_CONNECTING,   /* trying the saved network */
    WIFI_CONNECTED,    /* on your router; web UI at wifi_ip() */
    WIFI_HOTSPOT,      /* only our own access point is up */
} wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_ap_t;

/* Starts Wi-Fi: joins the saved network, or opens the hotspot if there is none
 * (or if joining fails for 20 s). */
void wifi_start(void);
void wifi_set_enabled(bool on);
bool wifi_enabled(void);

wifi_state_t wifi_state(void);
bool wifi_hotspot_on(void);
void wifi_set_hotspot(bool on);
int wifi_rssi(void);                 /* dBm, 0 if not connected */
int wifi_bars(void);                 /* 0..4 for the status bar */
void wifi_ip(char *out, int n);      /* "192.168.1.23" or "" */
void wifi_ssid(char *out, int n);    /* saved / connected network */
const char *wifi_ap_ssid(void);      /* our hotspot name, e.g. RetroPocket-1A2B */
const char *wifi_ap_pass(void);      /* our hotspot password */
#define WIFI_AP_IP "192.168.4.1"

/* Network scan (non-blocking). */
bool wifi_scan_start(void);
bool wifi_scan_busy(void);
int wifi_scan_results(wifi_ap_t *out, int max);

/* Saves and joins a network. Empty password for open networks. */
void wifi_connect(const char *ssid, const char *pass);
void wifi_forget(void);
