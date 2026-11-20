/*
 * webui.c - the device's web page and its JSON API.
 *
 * Open http://<device ip>/ (or http://192.168.4.1 on the hotspot):
 *   remote control with live screen, game library, SD file manager, settings,
 *   Wi-Fi setup, firmware updates, system info.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "webui.h"
#include "board.h"
#include "bridge.h"
#include "display.h"
#include "lock.h"
#include "ota.h"
#include "romscan.h"
#include "system.h"
#include "unzip.h"
#include "version.h"
#include "wifi.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

/* ---------------- helpers ---------------- */

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o = *s == '+' ? ' ' : *s;
        }
    }
    *o = 0;
}

static bool query(httpd_req_t *req, const char *key, char *out, size_t n)
{
    char q[700];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, n) != ESP_OK) return false;
    url_decode(out);
    return true;
}

/* Turns a web path ("/roms/a.zip") into an SD path, refusing "..". */
static bool sd_path(const char *web, char *out, size_t n)
{
    if (!web[0] || web[0] != '/' || strstr(web, "..")) return false;
    snprintf(out, n, "%s%s", SD_MOUNT, strcmp(web, "/") ? web : "");
    return true;
}

/* Appends a JSON string literal with escaping. */
static void jstr(char **p, char *end, const char *s)
{
    char *o = *p;
    if (o < end) *o++ = '"';
    for (; *s && o < end - 7; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = c; }
        else if (c < 0x20) o += sprintf(o, "\\u%04x", c);
        else *o++ = c;
    }
    if (o < end) *o++ = '"';
    *p = o;
}

#define JBUF 1024
typedef struct { char buf[JBUF]; char *p; httpd_req_t *req; } jout_t;

static void jo_init(jout_t *j, httpd_req_t *req)
{
    j->p = j->buf;
    j->req = req;
    httpd_resp_set_type(req, "application/json");
}

static void jo_flush(jout_t *j, bool final)
{
    if (j->p > j->buf) httpd_resp_send_chunk(j->req, j->buf, j->p - j->buf);
    j->p = j->buf;
    if (final) httpd_resp_send_chunk(j->req, NULL, 0);
}

static void jo_raw(jout_t *j, const char *s)
{
    size_t n = strlen(s);
    if (j->p + n >= j->buf + JBUF) jo_flush(j, false);
    memcpy(j->p, s, n);
    j->p += n;
}

static void jo_str(jout_t *j, const char *s)
{
    if (j->p + strlen(s) * 6 + 4 >= j->buf + JBUF) jo_flush(j, false);
    jstr(&j->p, j->buf + JBUF, s);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char b[160], *p = b;
    p += sprintf(p, "{\"ok\":false,\"error\":");
    jstr(&p, b + sizeof(b) - 2, msg);
    strcpy(p, "}");
    return httpd_resp_sendstr(req, b);
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *b = malloc(req->content_len + 1);
    if (!b) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) { free(b); return NULL; }
        got += r;
    }
    b[got] = 0;
    cJSON *o = cJSON_Parse(b);
    free(b);
    return o;
}

/* ---------------- page ---------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

/* Phones probe these to detect a captive portal; send them to the page. */
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------- info ---------------- */

static esp_err_t h_info(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    char s[96];
    cJSON_AddStringToObject(o, "version", FW_VERSION);
    static const char *const states[] = { "off", "connecting", "connected", "hotspot" };
    cJSON_AddStringToObject(o, "wifi", states[wifi_state()]);
    wifi_ip(s, sizeof(s));
    cJSON_AddStringToObject(o, "ip", s);
    wifi_ssid(s, sizeof(s));
    cJSON_AddStringToObject(o, "ssid", s);
    cJSON_AddNumberToObject(o, "rssi", wifi_rssi());
    cJSON_AddBoolToObject(o, "hotspot", wifi_hotspot_on());
    cJSON_AddStringToObject(o, "ap_ssid", wifi_ap_ssid());
    cJSON_AddNumberToObject(o, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    uint64_t total = 0, freeb = 0;
    if (sd_ok() && esp_vfs_fat_info(SD_MOUNT, &total, &freeb) == ESP_OK) {
        cJSON_AddNumberToObject(o, "sd_total", (double)total);
        cJSON_AddNumberToObject(o, "sd_free", (double)freeb);
    }
    cJSON_AddBoolToObject(o, "sd", sd_ok());
    bridge_get_now_playing(s, sizeof(s));
    cJSON_AddStringToObject(o, "playing", s);
    cJSON_AddNumberToObject(o, "screen_w", LCD_WIDTH);
    cJSON_AddNumberToObject(o, "screen_h", LCD_HEIGHT);
    lib_lock();
    cJSON_AddNumberToObject(o, "nes", games_count(SYS_NES));
    cJSON_AddNumberToObject(o, "gb", games_count(SYS_GB));
    lib_unlock();
    ota_status_t st;
    ota_status(&st);
    cJSON *u = cJSON_AddObjectToObject(o, "ota");
    static const char *const phases[] = { "idle", "checking", "downloading", "done", "failed" };
    cJSON_AddStringToObject(u, "phase", phases[st.phase]);
    cJSON_AddNumberToObject(u, "percent", st.percent);
    cJSON_AddStringToObject(u, "latest", st.latest);
    cJSON_AddStringToObject(u, "message", st.message);
    cJSON_AddNumberToObject(u, "available", ota_update_available());
    cJSON_AddStringToObject(u, "rollback", ota_rollback_info());
    return send_json(req, o);
}

/* ---------------- games ---------------- */

static void jo_game(jout_t *j, const game_t *g, bool *first)
{
    if (!*first) jo_raw(j, ",");
    *first = false;
    char num[48];
    jo_raw(j, "{\"name\":");
    jo_str(j, g->name);
    jo_raw(j, ",\"path\":");
    jo_str(j, g->path + strlen(SD_MOUNT));
    snprintf(num, sizeof(num), ",\"sys\":%d,\"fav\":%s}", g->sys, lib_is_fav(g->path) ? "true" : "false");
    jo_raw(j, num);
}

static esp_err_t h_games(httpd_req_t *req)
{
    jout_t *j = malloc(sizeof(*j));
    if (!j) return fail(req, "500 Internal Server Error", "no memory");
    jo_init(j, req);
    lib_lock();
    jo_raw(j, "{\"games\":[");
    bool first = true;
    for (int s = 1; s < SYS_COUNT; s++)
        for (int i = 0; i < games_count(s); i++) jo_game(j, games_get(s, i), &first);
    jo_raw(j, "],\"recent\":[");
    first = true;
    for (int i = 0; i < lib_recent_count(); i++) jo_game(j, lib_recent_get(i), &first);
    jo_raw(j, "]}");
    lib_unlock();
    jo_flush(j, true);
    free(j);
    return ESP_OK;
}

static esp_err_t h_launch(httpd_req_t *req)
{
    char web[300], path[320];
    if (!query(req, "path", web, sizeof(web)) || !sd_path(web, path, sizeof(path)))
        return fail(req, "400 Bad Request", "bad path");
    bridge_request_launch(path);
    return ok(req);
}

static esp_err_t h_exit(httpd_req_t *req)
{
    bridge_request_quit();
    return ok(req);
}

static esp_err_t h_fav(httpd_req_t *req)
{
    char web[300], path[320], on[4] = "1";
    if (!query(req, "path", web, sizeof(web)) || !sd_path(web, path, sizeof(path)))
        return fail(req, "400 Bad Request", "bad path");
    query(req, "on", on, sizeof(on));
    lib_set_fav(path, on[0] == '1');
    return ok(req);
}

static esp_err_t h_rescan(httpd_req_t *req)
{
    bridge_request(REQ_RESCAN);
    return ok(req);
}

/* ---------------- files ---------------- */

static esp_err_t h_files(httpd_req_t *req)
{
    char web[300] = "/", path[320];
    query(req, "dir", web, sizeof(web));
    if (!sd_path(web, path, sizeof(path))) return fail(req, "400 Bad Request", "bad path");
    DIR *d = opendir(path);
    if (!d) return fail(req, "404 Not Found", "folder not found");
    jout_t *j = malloc(sizeof(*j));
    if (!j) { closedir(d); return fail(req, "500 Internal Server Error", "no memory"); }
    jo_init(j, req);
    jo_raw(j, "{\"files\":[");
    bool first = true;
    struct dirent *e;
    char full[600], num[64];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st = { 0 };
        stat(full, &st);
        if (!first) jo_raw(j, ",");
        first = false;
        jo_raw(j, "{\"name\":");
        jo_str(j, e->d_name);
        snprintf(num, sizeof(num), ",\"dir\":%s,\"size\":%ld}", e->d_type == DT_DIR ? "true" : "false",
                 (long)st.st_size);
        jo_raw(j, num);
    }
    closedir(d);
    jo_raw(j, "]}");
    jo_flush(j, true);
    free(j);
    return ESP_OK;
}

static esp_err_t h_download(httpd_req_t *req)
{
    char web[300], path[320];
    if (!query(req, "path", web, sizeof(web)) || !sd_path(web, path, sizeof(path)))
        return fail(req, "400 Bad Request", "bad path");
    FILE *f = fopen(path, "rb");
    if (!f) return fail(req, "404 Not Found", "file not found");
    const char *base = strrchr(path, '/') + 1;
    char disp[300];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *buf = malloc(4096);
    size_t n;
    while (buf && (n = fread(buf, 1, 4096, f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char web[300], path[320], tmp[330];
    if (!query(req, "path", web, sizeof(web)) || !sd_path(web, path, sizeof(path)))
        return fail(req, "400 Bad Request", "bad path");
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return fail(req, "500 Internal Server Error", "can't create file (folder missing?)");
    char *buf = malloc(8192);
    int left = req->content_len;
    bool good = buf != NULL;
    while (good && left > 0) {
        int r = httpd_req_recv(req, buf, left < 8192 ? left : 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) good = false;
        else left -= r;
    }
    free(buf);
    fclose(f);
    if (!good) {
        unlink(tmp);
        return fail(req, "500 Internal Server Error", "upload failed (SD full?)");
    }
    unlink(path);
    rename(tmp, path);
    if (has_ext(path, ".zip .nes .gb .gbc .dmg")) bridge_request(REQ_RESCAN);
    return ok(req);
}

static int rm_rf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char sub[600];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            rm_rf(sub);
        }
        closedir(d);
    }
    return rmdir(path);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    char web[300], path[320];
    if (!query(req, "path", web, sizeof(web)) || !sd_path(web, path, sizeof(path)) || !strcmp(web, "/"))
        return fail(req, "400 Bad Request", "bad path");
    if (rm_rf(path) != 0) return fail(req, "500 Internal Server Error", "delete failed");
    bridge_request(REQ_RESCAN);
    return ok(req);
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    char web[300], path[320];
    if (!query(req, "path", web, sizeof(web)) || !sd_path(web, path, sizeof(path)))
        return fail(req, "400 Bad Request", "bad path");
    return mkdir(path, 0777) == 0 ? ok(req) : fail(req, "500 Internal Server Error", "mkdir failed");
}

static esp_err_t h_rename(httpd_req_t *req)
{
    char wf[300], wt[300], from[320], to[320];
    if (!query(req, "from", wf, sizeof(wf)) || !query(req, "to", wt, sizeof(wt)) ||
        !sd_path(wf, from, sizeof(from)) || !sd_path(wt, to, sizeof(to)))
        return fail(req, "400 Bad Request", "bad path");
    if (rename(from, to) != 0) return fail(req, "500 Internal Server Error", "rename failed");
    bridge_request(REQ_RESCAN);
    return ok(req);
}

/* ---------------- settings ---------------- */

static esp_err_t h_settings_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "brightness", g_settings.brightness);
    cJSON_AddNumberToObject(o, "rotation", g_settings.rotation);
    cJSON_AddNumberToObject(o, "stick_rot", g_settings.stick_rot);
    cJSON_AddNumberToObject(o, "swap_rb", g_settings.swap_rb);
    cJSON_AddNumberToObject(o, "gb_palette", g_settings.gb_palette);
    cJSON_AddNumberToObject(o, "nes_scale", g_settings.nes_scale);
    cJSON_AddNumberToObject(o, "show_fps", g_settings.show_fps);
    cJSON_AddNumberToObject(o, "ui_anim", g_settings.ui_anim);
    ota_source_t src;
    ota_source_get(&src);
    cJSON_AddStringToObject(o, "ota_repo", src.repo);
    cJSON_AddStringToObject(o, "ota_branch", src.branch);
    cJSON_AddStringToObject(o, "ota_path", src.path);
    cJSON_AddBoolToObject(o, "ota_has_token", src.token[0] != 0);
    return send_json(req, o);
}

static void set_int(cJSON *o, const char *k, int *dst, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) {
        int x = v->valueint;
        *dst = x < lo ? lo : x > hi ? hi : x;
    }
}

static void set_str(cJSON *o, const char *k, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v)) snprintf(dst, n, "%s", v->valuestring);
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    cJSON *o = read_json(req);
    if (!o) return fail(req, "400 Bad Request", "bad json");
    set_int(o, "brightness", &g_settings.brightness, 10, 100);
    set_int(o, "rotation", &g_settings.rotation, 0, 3);
    set_int(o, "stick_rot", &g_settings.stick_rot, 0, 3);
    set_int(o, "swap_rb", &g_settings.swap_rb, 0, 1);
    set_int(o, "gb_palette", &g_settings.gb_palette, 0, 4);
    set_int(o, "nes_scale", &g_settings.nes_scale, 0, 1);
    set_int(o, "show_fps", &g_settings.show_fps, 0, 1);
    set_int(o, "ui_anim", &g_settings.ui_anim, 0, 1);
    if (cJSON_GetObjectItem(o, "ota_repo")) {
        ota_source_t src;
        ota_source_get(&src);
        set_str(o, "ota_repo", src.repo, sizeof(src.repo));
        set_str(o, "ota_branch", src.branch, sizeof(src.branch));
        set_str(o, "ota_path", src.path, sizeof(src.path));
        cJSON *t = cJSON_GetObjectItem(o, "ota_token");
        if (cJSON_IsString(t) && strcmp(t->valuestring, "********")) /* placeholder = keep */
            snprintf(src.token, sizeof(src.token), "%s", t->valuestring);
        ota_source_set(&src);
    }
    cJSON_Delete(o);
    bridge_request(REQ_APPLY_SETTINGS);
    return ok(req);
}

/* ---------------- Wi-Fi ---------------- */

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    wifi_scan_start();
    for (int i = 0; i < 80 && wifi_scan_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    wifi_ap_t *aps = malloc(sizeof(wifi_ap_t) * 24);
    int n = aps ? wifi_scan_results(aps, 24) : 0;
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(a, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, a);
    }
    free(aps);
    return send_json(req, o);
}

static esp_err_t h_wifi_connect(httpd_req_t *req)
{
    cJSON *o = read_json(req);
    cJSON *ssid = o ? cJSON_GetObjectItem(o, "ssid") : NULL;
    cJSON *pass = o ? cJSON_GetObjectItem(o, "pass") : NULL;
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32) {
        cJSON_Delete(o);
        return fail(req, "400 Bad Request", "missing ssid");
    }
    wifi_connect(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(o);
    return ok(req);
}

static esp_err_t h_wifi_forget(httpd_req_t *req)
{
    wifi_forget();
    return ok(req);
}

static esp_err_t h_wifi_hotspot(httpd_req_t *req)
{
    char on[4] = "1";
    query(req, "on", on, sizeof(on));
    wifi_set_hotspot(on[0] == '1');
    return ok(req);
}

/* ---------------- OTA / system ---------------- */

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    if (req->content_len < 64 * 1024) return fail(req, "400 Bad Request", "that file is too small to be firmware");
    if (!ota_upload_begin(req->content_len)) return fail(req, "409 Conflict", ota_upload_error());
    char *buf = malloc(4096);
    int left = req->content_len;
    bool good = buf != NULL, net_err = false;
    while (good && left > 0) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { good = false; net_err = true; break; }
        if (!ota_upload_write(buf, r)) good = false;
        else left -= r;
    }
    free(buf);
    if (!good) {
        ota_upload_abort();
        return fail(req, "400 Bad Request", net_err ? "upload interrupted" : ota_upload_error());
    }
    if (!ota_upload_end()) return fail(req, "400 Bad Request", ota_upload_error());
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":true,\"version\":\"%s\"}", ota_upload_version());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

static esp_err_t h_ota_check(httpd_req_t *req)
{
    char install[4] = "0";
    query(req, "install", install, sizeof(install));
    if (install[0] == '1') bridge_request(REQ_OTA_ONLINE); /* the device shows progress */
    else ota_online_start(false);
    return ok(req);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    ota_reboot_soon();
    return ok(req);
}

/* 24-bit BMP of the current screen. */
static esp_err_t h_screenshot(httpd_req_t *req)
{
    int w = LCD_WIDTH, h = LCD_HEIGHT, row = w * 3;
    uint16_t *px = heap_caps_malloc(LCD_PIXELS * 2, MALLOC_CAP_SPIRAM);
    uint8_t *line = malloc(row);
    if (!px || !line) { free(px); free(line); return fail(req, "500 Internal Server Error", "no memory"); }
    memcpy(px, display_front(), LCD_PIXELS * 2);
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t size = 54 + row * h;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"screenshot.bmp\"");
    httpd_resp_send_chunk(req, (char *)hdr, 54);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t c = SWAP16(px[y * w + x]);
            line[x * 3 + 0] = (c & 31) << 3;
            line[x * 3 + 1] = ((c >> 5) & 63) << 2;
            line[x * 3 + 2] = (c >> 11) << 3;
        }
        httpd_resp_send_chunk(req, (char *)line, row);
    }
    free(px);
    free(line);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------------- WebSocket: remote control + live screen ---------------- */
/* client -> "k<number>" buttons held (BTN_* bits), "f" = send me a frame
 * server -> binary: u16 width, u16 height, then width*height RGB565 (big endian) */

static uint8_t *s_frame;

static esp_err_t h_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK; /* handshake */
    httpd_ws_frame_t f = { 0 };
    char msg[32];
    f.payload = (uint8_t *)msg;
    if (httpd_ws_recv_frame(req, &f, sizeof(msg) - 1) != ESP_OK) return ESP_FAIL;
    msg[f.len] = 0;
    if (f.type == HTTPD_WS_TYPE_CLOSE) { bridge_set_remote_keys(0); return ESP_OK; }
    if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    if (msg[0] == 'k') {
        bridge_set_remote_keys((uint32_t)strtoul(msg + 1, NULL, 10));
    } else if (msg[0] == 'f') {
        if (!s_frame) s_frame = heap_caps_malloc(4 + LCD_PIXELS * 2, MALLOC_CAP_SPIRAM);
        if (!s_frame) return ESP_OK;
        uint16_t w = LCD_WIDTH, h = LCD_HEIGHT;
        memcpy(s_frame, &w, 2);
        memcpy(s_frame + 2, &h, 2);
        memcpy(s_frame + 4, display_front(), LCD_PIXELS * 2);
        httpd_ws_frame_t out = { .type = HTTPD_WS_TYPE_BINARY, .payload = s_frame, .len = 4 + LCD_PIXELS * 2 };
        return httpd_ws_send_frame(req, &out);
    }
    return ESP_OK;
}

/* ---------------- start ---------------- */

void webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 40;
    cfg.stack_size = 10240;
    cfg.core_id = 0;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        return;
    }
    const struct { const char *uri; httpd_method_t m; esp_err_t (*h)(httpd_req_t *); } routes[] = {
        { "/",                  HTTP_GET,    h_index },
        { "/api/info",          HTTP_GET,    h_info },
        { "/api/games",         HTTP_GET,    h_games },
        { "/api/launch",        HTTP_POST,   h_launch },
        { "/api/exit",          HTTP_POST,   h_exit },
        { "/api/fav",           HTTP_POST,   h_fav },
        { "/api/rescan",        HTTP_POST,   h_rescan },
        { "/api/files",         HTTP_GET,    h_files },
        { "/api/file",          HTTP_GET,    h_download },
        { "/api/file",          HTTP_PUT,    h_upload },
        { "/api/file",          HTTP_DELETE, h_delete },
        { "/api/mkdir",         HTTP_POST,   h_mkdir },
        { "/api/rename",        HTTP_POST,   h_rename },
        { "/api/settings",      HTTP_GET,    h_settings_get },
        { "/api/settings",      HTTP_POST,   h_settings_post },
        { "/api/wifi/scan",     HTTP_GET,    h_wifi_scan },
        { "/api/wifi/connect",  HTTP_POST,   h_wifi_connect },
        { "/api/wifi/forget",   HTTP_POST,   h_wifi_forget },
        { "/api/wifi/hotspot",  HTTP_POST,   h_wifi_hotspot },
        { "/api/ota/upload",    HTTP_POST,   h_ota_upload },
        { "/api/ota/check",     HTTP_POST,   h_ota_check },
        { "/api/reboot",        HTTP_POST,   h_reboot },
        { "/api/screenshot.bmp", HTTP_GET,   h_screenshot },
        { "/generate_204",      HTTP_GET,    h_redirect },
        { "/hotspot-detect.html", HTTP_GET,  h_redirect },
        { "/connecttest.txt",   HTTP_GET,    h_redirect },
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t u = { .uri = routes[i].uri, .method = routes[i].m, .handler = routes[i].h };
        httpd_register_uri_handler(s_server, &u);
    }
    httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true };
    httpd_register_uri_handler(s_server, &ws);
    ESP_LOGI(TAG, "web UI running");
}
