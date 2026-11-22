/*
 * browser.c - a small text web browser (think Lynx) for the 160x128 screen.
 *
 * Fetches http/https pages, turns HTML into wrapped text with selectable
 * links and simple forms (search boxes work), keeps a history, bookmarks,
 * and saves anything that isn't a web page to the SD card: games
 * (.nes .gb .gbc .sms .gg .pce .wad .zip ...) go to /roms, the rest to
 * /downloads. No JavaScript, no images - pages built only with JS
 * show little; the "Simple view" option runs them through FrogFind.
 *
 * Controls: up/down scroll, left/right previous/next link, A open,
 * B back, START menu, SELECT page down, stick click = menu.
 */
/* URLs and titles are cut to fit their buffers on purpose */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "emulators.h"
#include "board.h"
#include "bridge.h"
#include "display.h"
#include "gfx.h"
#include "i18n.h"
#include "input.h"
#include "system.h"
#include "ui.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#ifndef SIM
#include "wifi.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifndef SIM
#include "freertos/idf_additions.h"
#endif

#define W LCD_WIDTH
#define H LCD_HEIGHT
#define UA "Lynx/2.9.0 libwww-FM/2.14 (RetroPocket)"

#define MAX_PAGE   (600 * 1024)
#define MAX_LINES  7000
#define MAX_COLS   (LCD_MAX_DIM / CHAR_W)
#define MAX_LINKS  2500
#define MAX_FORMS  24
#define URL_MAX    512
#define HIST_MAX   24
#define LINE_H     9
#define TOP_H      10
#define BOT_H      9

#define DIR_DOWNLOADS SD_MOUNT "/downloads"
#define DIR_ROMS_     SD_MOUNT "/roms"
#define BOOKMARK_FILE SD_MOUNT "/saves/bookmarks.txt"

static const char *const home_marks[][2] = {
    { "DuckDuckGo Lite (search)", "https://lite.duckduckgo.com/lite/" },
    { "FrogFind (simple web)", "http://frogfind.com/" },
    { "68k.news (news)", "http://68k.news/" },
    { "Wikipedia", "https://en.m.wikipedia.org/" },
    { "Wikipedia (srpski)", "https://sr.m.wikipedia.org/" },
    { "Homebrew Hub (free GB games)", "https://hh.gbdev.io/" },
};

/* ---------------- network ---------------- */

typedef struct {
    char ctype[64], disp[160], location[URL_MAX];
    int status;
    long long len;          /* -1 unknown */
#ifdef SIM
    FILE *f;
#else
    esp_http_client_handle_t c;
#endif
} net_t;

static bool resolve_url(const char *base, const char *href, char *out, size_t n);

#ifdef SIM
/* The simulator fetches with curl (local test pages). */
static void shell_quote(char *o, size_t n, const char *s)
{
    size_t k = 0;
    o[k++] = '\'';
    for (; *s && k + 5 < n; s++) {
        if (*s == '\'') { memcpy(o + k, "'\\''", 4); k += 4; }
        else o[k++] = *s;
    }
    o[k++] = '\'';
    o[k] = 0;
}

static bool net_open(net_t *n, const char *url, const char *post, char *final_url, size_t fu)
{
    memset(n, 0, sizeof(*n));
    char q[URL_MAX * 2], pq[1200] = "", cmd[3000];
    shell_quote(q, sizeof(q), url);
    if (post) { char t[1100]; shell_quote(t, sizeof(t), post); snprintf(pq, sizeof(pq), "--data-binary %s", t); }
    snprintf(cmd, sizeof(cmd), "curl -sS -L --noproxy '*' --max-redirs 8 -A '" UA "' -D /tmp/rp_hdr -o /tmp/rp_body "
             "-w '%%{url_effective}' %s %s > /tmp/rp_url 2>/dev/null", pq, q);
    if (system(cmd) != 0) return false;
    FILE *f = fopen("/tmp/rp_url", "r");
    if (f) { if (!fgets(final_url, fu, f)) snprintf(final_url, fu, "%s", url); fclose(f); }
    f = fopen("/tmp/rp_hdr", "r");
    char line[600];
    n->len = -1;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "HTTP/", 5)) { n->status = atoi(strchr(line, ' ') ? strchr(line, ' ') + 1 : "0"); n->ctype[0] = n->disp[0] = 0; }
        else if (!strncasecmp(line, "content-type:", 13)) snprintf(n->ctype, sizeof(n->ctype), "%.60s", line + 14);
        else if (!strncasecmp(line, "content-disposition:", 20)) snprintf(n->disp, sizeof(n->disp), "%.150s", line + 21);
        else if (!strncasecmp(line, "content-length:", 15)) n->len = atoll(line + 16);
    }
    if (f) fclose(f);
    n->f = fopen("/tmp/rp_body", "rb");
    return n->f != NULL;
}
static int net_read(net_t *n, char *buf, int len) { return (int)fread(buf, 1, len, n->f); }
static void net_close(net_t *n) { if (n->f) fclose(n->f); n->f = NULL; }
static bool net_ready(void) { return true; }
#else
static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    net_t *n = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && n) {
        if (!strcasecmp(e->header_key, "Content-Type")) snprintf(n->ctype, sizeof(n->ctype), "%s", e->header_value);
        else if (!strcasecmp(e->header_key, "Content-Disposition")) snprintf(n->disp, sizeof(n->disp), "%s", e->header_value);
        else if (!strcasecmp(e->header_key, "Location")) snprintf(n->location, sizeof(n->location), "%s", e->header_value);
    }
    return ESP_OK;
}

static bool net_open(net_t *n, const char *url, const char *post, char *final_url, size_t fu)
{
    char cur[URL_MAX];
    snprintf(cur, sizeof(cur), "%s", url);
    memset(n, 0, sizeof(*n));
    for (int redirects = 0; redirects < 8; redirects++) {
        n->ctype[0] = n->disp[0] = n->location[0] = 0;
        esp_http_client_config_t cfg = {
            .url = cur, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
            .buffer_size = 4096, .buffer_size_tx = 2048, .event_handler = on_http_event,
            .user_data = n, .disable_auto_redirect = true,
            .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        };
        n->c = esp_http_client_init(&cfg);
        if (!n->c) return false;
        esp_http_client_set_header(n->c, "User-Agent", UA);
        esp_http_client_set_header(n->c, "Accept", "text/html,text/plain;q=0.9,*/*;q=0.5");
        esp_http_client_set_header(n->c, "Accept-Language", g_settings.lang == LANG_SR ? "sr,en;q=0.7" : "en");
        int plen = post ? (int)strlen(post) : 0;
        if (post) esp_http_client_set_header(n->c, "Content-Type", "application/x-www-form-urlencoded");
        if (esp_http_client_open(n->c, plen) != ESP_OK) { esp_http_client_cleanup(n->c); n->c = NULL; return false; }
        if (plen) esp_http_client_write(n->c, post, plen);
        long long len = esp_http_client_fetch_headers(n->c);
        n->status = esp_http_client_get_status_code(n->c);
        if (n->status >= 300 && n->status < 400 && n->location[0]) {
            char next[URL_MAX];
            if (!resolve_url(cur, n->location, next, sizeof(next))) break;
            esp_http_client_cleanup(n->c);
            n->c = NULL;
            snprintf(cur, sizeof(cur), "%s", next);
            if (n->status != 307 && n->status != 308) post = NULL;
            continue;
        }
        n->len = esp_http_client_is_chunked_response(n->c) || len <= 0 ? -1 : len;
        snprintf(final_url, fu, "%s", cur);
        return true;
    }
    if (n->c) esp_http_client_cleanup(n->c);
    n->c = NULL;
    return false;
}
static int net_read(net_t *n, char *buf, int len) { return esp_http_client_read(n->c, buf, len); }
static void net_close(net_t *n) { if (n->c) { esp_http_client_close(n->c); esp_http_client_cleanup(n->c); } n->c = NULL; }
static bool net_ready(void) { return wifi_state() == WIFI_CONNECTED; }
#endif

/* ---------------- URLs ---------------- */

static void url_encode(const char *s, char *o, size_t n)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr("-_.~", c)) o[k++] = c;
        else if (c == ' ') o[k++] = '+';
        else { o[k++] = '%'; o[k++] = hx[c >> 4]; o[k++] = hx[c & 15]; }
    }
    o[k] = 0;
}

/* turns a relative link into an absolute http(s) URL; false for mailto:, js, anchors */
static bool resolve_url(const char *base, const char *href, char *out, size_t n)
{
    while (*href == ' ' || *href == '\t' || *href == '\n' || *href == '\r') href++;
    if (!*href || *href == '#') return false;
    const char *p = href;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    static char tmp[URL_MAX * 2];
    if (*p == ':' && p > href) {
        if (strncasecmp(href, "http:", 5) && strncasecmp(href, "https:", 6) && strncasecmp(href, "rp:", 3)) return false;
        snprintf(tmp, sizeof(tmp), "%s", href);
    } else {
        const char *se = strstr(base, "://");
        if (!se) return false;
        const char *host = se + 3, *path = strchr(host, '/');
        int scheme_len = (int)(se - base), host_end = path ? (int)(path - base) : (int)strlen(base);
        if (href[0] == '/' && href[1] == '/') snprintf(tmp, sizeof(tmp), "%.*s:%s", scheme_len, base, href);
        else if (href[0] == '/') snprintf(tmp, sizeof(tmp), "%.*s%s", host_end, base, href);
        else if (href[0] == '?') {
            int q = (int)strcspn(base, "?#");
            snprintf(tmp, sizeof(tmp), "%.*s%s", q, base, href);
        } else {
            int q = (int)strcspn(base, "?#"), dir = host_end;
            for (int i = host_end; i < q; i++) if (base[i] == '/') dir = i + 1;
            if (dir == host_end) snprintf(tmp, sizeof(tmp), "%.*s/%s", host_end, base, href);
            else snprintf(tmp, sizeof(tmp), "%.*s%s", dir, base, href);
        }
        /* normalise /./ and /../ in the path */
        char *h = strstr(tmp, "://");
        char *root = h ? strchr(h + 3, '/') : NULL;
        if (root) {
            char *q = root + strcspn(root, "?#");
            char save = *q; *q = 0;
            static char seg[URL_MAX * 2];
            size_t k = 0;
            for (char *s = root; *s;) {
                if (!strncmp(s, "/./", 3)) { s += 2; continue; }
                if (!strcmp(s, "/.")) { s += 1; continue; }
                if (!strncmp(s, "/../", 4) || !strcmp(s, "/..")) {
                    while (k > 0 && seg[k - 1] != '/') k--;
                    if (k > 0) k--;
                    s += 3;
                    if (!*s) { seg[k++] = '/'; }
                    continue;
                }
                seg[k++] = *s++;
            }
            seg[k] = 0;
            *q = save;
            char rest[URL_MAX];
            snprintf(rest, sizeof(rest), "%s", q);
            snprintf(root, sizeof(tmp) - (root - tmp), "%s%s", k ? seg : "/", rest);
        }
    }
    tmp[strcspn(tmp, "#")] = 0;
    /* escape spaces and control chars */
    size_t k = 0;
    for (const char *s = tmp; *s && k + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= ' ') { if (c == ' ') { memcpy(out + k, "%20", 3); k += 3; } }
        else out[k++] = c;
    }
    out[k] = 0;
    return k > 0;
}

/* ---------------- document ---------------- */

enum { L_LINK, L_INPUT, L_SUBMIT };
typedef struct { char *url; uint8_t type, form; char *name; char value[72]; } link_t;
typedef struct { char *action; bool post; char *hidden; } form_t;

enum { ST_TEXT, ST_HEAD, ST_LINK, ST_DIM };

static char *s_page;                 /* raw bytes */
static char *s_txt;                  /* MAX_LINES * MAX_COLS */
static uint8_t *s_sty;
static uint16_t *s_lnk;              /* link id + 1, 0 = none */
static int s_lines, s_cols;
static link_t *s_links;
static int s_nlinks;
static int *s_link_line;
static form_t s_forms[MAX_FORMS];
static int s_nforms;
static char s_title[96], s_url[URL_MAX];

/* layout state */
static int col, cur_link = -1, cur_style, pending_space, blank_lines, in_pre;
static int link_had_text;

static char *pstrdup(const char *s)
{
    size_t l = strlen(s) + 1;
    char *d = heap_caps_malloc(l, MALLOC_CAP_SPIRAM);
    if (!d) d = malloc(l);
    if (d) memcpy(d, s, l);
    return d;
}

static void doc_free(void)
{
    for (int i = 0; i < s_nlinks; i++) { free(s_links[i].url); free(s_links[i].name); }
    for (int i = 0; i < s_nforms; i++) { free(s_forms[i].action); free(s_forms[i].hidden); }
    s_nlinks = s_nforms = 0;
    s_lines = 0;
}

static void new_line(void)
{
    if (s_lines >= MAX_LINES - 1) return;
    s_lines++;
    char *t = s_txt + s_lines * MAX_COLS;
    memset(t, ' ', MAX_COLS);
    memset(s_sty + s_lines * MAX_COLS, 0, MAX_COLS);
    memset(s_lnk + s_lines * MAX_COLS, 0, MAX_COLS * 2);
    col = 0;
}

static void doc_start(void)
{
    s_lines = -1;
    new_line();
    col = 0; cur_link = -1; cur_style = ST_TEXT; pending_space = 0; blank_lines = 1; in_pre = 0;
}

static void put_char(char c)
{
    if (col >= s_cols) new_line();
    int i = s_lines * MAX_COLS + col;
    s_txt[i] = c;
    s_sty[i] = cur_link >= 0 ? ST_LINK : cur_style;
    s_lnk[i] = cur_link >= 0 ? cur_link + 1 : 0;
    col++;
    blank_lines = 0;
    if (cur_link >= 0) {
        link_had_text = 1;
        if (s_link_line[cur_link] < 0) s_link_line[cur_link] = s_lines;
    }
}

static void put_word(const char *w, int len)
{
    if (!len) return;
    if (col > 0 && pending_space) {
        if (col + 1 + len > s_cols && len <= s_cols) new_line();
        else { int l = cur_link; cur_link = -1; put_char(' '); cur_link = l; }
    } else if (col > 0 && col + len > s_cols && len <= s_cols) new_line();
    pending_space = 0;
    for (int i = 0; i < len; i++) put_char(w[i]);
}

static void block_break(int blank)
{
    if (col > 0) new_line();
    while (blank_lines < blank) { new_line(); blank_lines++; }
    pending_space = 0;
}

static int add_link(int type, const char *url, int form)
{
    if (s_nlinks >= MAX_LINKS) return -1;
    link_t *l = &s_links[s_nlinks];
    memset(l, 0, sizeof(*l));
    l->type = type;
    l->url = url ? pstrdup(url) : NULL;
    l->form = form;
    s_link_line[s_nlinks] = -1;
    return s_nlinks++;
}

/* ---------------- text decoding ---------------- */

/* Appends the screen form of one Unicode code point (ASCII + font glyphs). */
static int cp_to_text(unsigned cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    switch (cp) {
        case 0x00A0: o[0] = ' '; return 1;
        case 0x010D: o[0] = '\x84'; return 1; case 0x0107: o[0] = '\x85'; return 1;
        case 0x0111: o[0] = '\x86'; return 1; case 0x0161: o[0] = '\x87'; return 1;
        case 0x017E: o[0] = '\x88'; return 1; case 0x010C: o[0] = '\x89'; return 1;
        case 0x0106: o[0] = '\x8a'; return 1; case 0x0110: o[0] = '\x8b'; return 1;
        case 0x0160: o[0] = '\x8c'; return 1; case 0x017D: o[0] = '\x8d'; return 1;
        case 0x2018: case 0x2019: case 0x00B4: o[0] = '\''; return 1;
        case 0x201C: case 0x201D: case 0x201E: case 0x00AB: case 0x00BB: o[0] = '"'; return 1;
        case 0x2013: case 0x2014: case 0x2212: o[0] = '-'; return 1;
        case 0x2026: memcpy(o, "...", 3); return 3;
        case 0x2022: case 0x00B7: o[0] = '*'; return 1;
        case 0x00A9: memcpy(o, "(c)", 3); return 3;
        case 0x00AE: memcpy(o, "(R)", 3); return 3;
        case 0x2122: memcpy(o, "TM", 2); return 2;
        case 0x20AC: memcpy(o, "EUR", 3); return 3;
        case 0x00B0: o[0] = 'o'; return 1;
        case 0x00D7: o[0] = 'x'; return 1;
        case 0x2190: o[0] = '\x7f'; return 1; case 0x2192: o[0] = '\x80'; return 1;
        case 0x2191: o[0] = '\x81'; return 1; case 0x2193: o[0] = '\x82'; return 1;
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0x00AD: return 0;
    }
    /* Latin-1 / Latin Extended-A letters -> base letter */
    static const char lat1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) { o[0] = lat1[cp - 0xC0]; return 1; }
    static const char latA[] =
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    if (cp >= 0x100 && cp <= 0x17F) { o[0] = latA[cp - 0x100]; return 1; }
    /* Cyrillic (Serbian / Russian) -> Serbian Latin */
    if (cp >= 0x400 && cp <= 0x45F) {
        static const char *const cyr[64] = {
            "A", "B", "V", "G", "D", "E", "\x8d", "Z", "I", "J", "K", "L", "M", "N", "O", "P",
            "R", "S", "T", "U", "F", "H", "C", "\x89", "\x8c", "\x8c", "", "Y", "", "E", "Ju", "Ja",
            "a", "b", "v", "g", "d", "e", "\x88", "z", "i", "j", "k", "l", "m", "n", "o", "p",
            "r", "s", "t", "u", "f", "h", "c", "\x84", "\x87", "\x87", "", "y", "", "e", "ju", "ja" };
        const char *s = NULL;
        if (cp >= 0x410 && cp <= 0x44F) s = cyr[cp - 0x410];
        else switch (cp) {
            case 0x402: s = "\x8b"; break; case 0x452: s = "\x86"; break;   /* Đ đ */
            case 0x408: s = "J"; break;    case 0x458: s = "j"; break;
            case 0x409: s = "Lj"; break;   case 0x459: s = "lj"; break;
            case 0x40A: s = "Nj"; break;   case 0x45A: s = "nj"; break;
            case 0x40B: s = "\x8a"; break; case 0x45B: s = "\x85"; break;   /* Ć ć */
            case 0x40F: s = "D\x8d"; break; case 0x45F: s = "d\x88"; break; /* Dž dž */
            case 0x401: s = "E"; break;    case 0x451: s = "e"; break;
            default: s = "?";
        }
        int l = (int)strlen(s);
        memcpy(o, s, l);
        return l;
    }
    if (cp >= 0x300 && cp <= 0x36F) return 0;  /* combining accents */
    o[0] = '?';
    return 1;
}

static unsigned utf8_next(const char **ps, const char *end)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned c = *s++, cp = c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && c < 0xC0) extra = 0, cp = 0xFFFD;
    else if (extra) cp = c & (0x3F >> extra);
    for (int i = 0; i < extra; i++) {
        if ((const char *)s >= end || (*s & 0xC0) != 0x80) { cp = 0xFFFD; break; }
        cp = (cp << 6) | (*s++ & 0x3F);
    }
    *ps = (const char *)s;
    return cp;
}

static unsigned entity(const char **ps, const char *end)
{
    const char *s = *ps + 1;
    char name[12];
    int k = 0;
    if (*s == '#') {
        s++;
        unsigned v = 0;
        if (*s == 'x' || *s == 'X') { s++; while (s < end && isxdigit((unsigned char)*s)) { v = v * 16 + (isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10)); s++; } }
        else while (s < end && isdigit((unsigned char)*s)) v = v * 10 + (*s++ - '0');
        if (s < end && *s == ';') s++;
        *ps = s;
        return v ? v : '?';
    }
    while (s < end && k < 10 && isalnum((unsigned char)*s)) name[k++] = *s++;
    name[k] = 0;
    static const struct { const char *n; unsigned cp; } ents[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
        {"copy", 0xA9}, {"reg", 0xAE}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022},
        {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"euro", 0x20AC}, {"times", 0xD7},
        {"deg", 0xB0}, {"larr", 0x2190}, {"rarr", 0x2192}, {"trade", 0x2122}, {"shy", 0xAD},
    };
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
        if (!strcmp(name, ents[i].n)) {
            if (s < end && *s == ';') s++;
            *ps = s;
            return ents[i].cp;
        }
    *ps = *ps + 1;
    return '&';
}

/* text run between tags -> words */
static void emit_text(const char *s, const char *end)
{
    char word[MAX_COLS * 4 + 8];
    int wl = 0;
    while (s < end) {
        unsigned cp;
        if (*s == '&') cp = entity(&s, end);
        else cp = utf8_next(&s, end);
        if (cp == '\n' && in_pre) { put_word(word, wl); wl = 0; new_line(); blank_lines = 0; continue; }
        if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || (cp == 0xA0 && !in_pre)) {
            put_word(word, wl); wl = 0;
            pending_space = 1;
            if (in_pre && cp == ' ') { if (col < s_cols) put_char(' '); pending_space = 0; }
            continue;
        }
        char o[4];
        int l = cp_to_text(cp, o);
        for (int i = 0; i < l; i++) {
            if (wl >= (int)sizeof(word) - 1 || wl >= s_cols) { put_word(word, wl); wl = 0; }
            word[wl++] = o[i];
        }
    }
    put_word(word, wl);
}

/* ---------------- HTML parser ---------------- */

static bool get_attr(const char *tag, const char *end, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    const char *s = tag;
    while (s < end && !isspace((unsigned char)*s) && *s != '>') s++;
    while (s < end) {
        while (s < end && (isspace((unsigned char)*s) || *s == '/')) s++;
        if (s >= end || *s == '>') break;
        const char *an = s;
        while (s < end && !isspace((unsigned char)*s) && *s != '=' && *s != '>') s++;
        size_t al = s - an;
        while (s < end && isspace((unsigned char)*s)) s++;
        const char *v = NULL;
        size_t vl = 0;
        if (s < end && *s == '=') {
            s++;
            while (s < end && isspace((unsigned char)*s)) s++;
            if (s < end && (*s == '"' || *s == '\'')) {
                char q = *s++;
                v = s;
                while (s < end && *s != q) s++;
                vl = s - v;
                if (s < end) s++;
            } else {
                v = s;
                while (s < end && !isspace((unsigned char)*s) && *s != '>') s++;
                vl = s - v;
            }
        }
        if (al == nl && !strncasecmp(an, name, nl)) {
            /* decode entities in the value (mostly &amp; in URLs) */
            size_t k = 0;
            const char *e = v ? v + vl : NULL;
            for (const char *p = v; p && p < e && k + 4 < n;) {
                if (*p == '&') {
                    unsigned cp = entity(&p, e);
                    if (cp < 0x80) out[k++] = (char)cp;
                    else if (cp < 0x800) { out[k++] = 0xC0 | (cp >> 6); out[k++] = 0x80 | (cp & 0x3F); }
                    else { out[k++] = 0xE0 | (cp >> 12); out[k++] = 0x80 | ((cp >> 6) & 0x3F); out[k++] = 0x80 | (cp & 0x3F); }
                } else out[k++] = *p++;
            }
            out[k] = 0;
            return true;
        }
    }
    if (n) out[0] = 0;
    return false;
}

static bool tag_is(const char *t, int tl, const char *name)
{
    return (int)strlen(name) == tl && !strncasecmp(t, name, tl);
}

static void show_value(int id)
{
    link_t *l = &s_links[id];
    char box[40];
    int w = s_cols - 4 < 14 ? s_cols - 4 : 14;
    if (l->type == L_SUBMIT) snprintf(box, sizeof(box), "[%.30s]", l->value[0] ? l->value : "Submit");
    else snprintf(box, sizeof(box), "[%-*.*s]", w, w, l->value);
    int save = cur_link;
    cur_link = id;
    pending_space = 1;
    put_word(box, (int)strlen(box));
    cur_link = save;
}

static void parse_html(const char *s, const char *end, bool plain)
{
    doc_start();
    if (plain) { in_pre = 1; emit_text(s, end); return; }
    int form = -1, in_title = 0, list_n[8] = { 0 }, list_depth = 0, in_select = 0;
    char attr[URL_MAX], abs[URL_MAX];
    const char *text = s;
    while (s < end) {
        if (*s != '<') { s++; continue; }
        /* flush text before the tag */
        if (s > text) {
            if (in_title) {
                size_t l = strlen(s_title);
                for (const char *p = text; p < s && l + 4 < sizeof(s_title);) {
                    unsigned cp = *p == '&' ? entity(&p, s) : utf8_next(&p, s);
                    l += cp_to_text(cp == '\n' ? ' ' : cp, s_title + l);
                }
                s_title[l] = 0;
            } else if (!in_select) emit_text(text, s);
        }
        if (s + 4 <= end && !strncmp(s, "<!--", 4)) {
            const char *e = strstr(s + 4, "-->");
            s = e ? e + 3 : end;
            text = s;
            continue;
        }
        const char *te = memchr(s, '>', end - s);
        if (!te) break;
        const char *t = s + 1;
        bool closing = *t == '/';
        if (closing) t++;
        int tl = 0;
        while (t + tl < te && (isalnum((unsigned char)t[tl]) || t[tl] == '!')) tl++;
        s = te + 1;
        text = s;
        if (!tl) continue;

        if (tag_is(t, tl, "script") || tag_is(t, tl, "style") || tag_is(t, tl, "svg") || tag_is(t, tl, "template")) {
            if (!closing) {
                /* jump straight to the matching close tag */
                char close[16];
                snprintf(close, sizeof(close), "</%.*s", tl, t);
                const char *p = s;
                while (p < end) {
                    p = memchr(p, '<', end - p);
                    if (!p) { p = end; break; }
                    if (!strncasecmp(p, close, strlen(close))) break;
                    p++;
                }
                const char *e = p < end ? memchr(p, '>', end - p) : NULL;
                s = text = e ? e + 1 : end;
            }
            continue;
        }
        if (tag_is(t, tl, "head")) { /* only the title matters in <head> */ continue; }
        if (tag_is(t, tl, "title")) { in_title = !closing; if (!closing) s_title[0] = 0; continue; }
        if (in_title) continue;
        if (tag_is(t, tl, "select")) { in_select = !closing; continue; }
        if (tag_is(t, tl, "noscript")) continue;

        if (tag_is(t, tl, "br")) { if (col > 0) new_line(); else if (blank_lines < 2) { new_line(); blank_lines++; } pending_space = 0; continue; }
        if (tag_is(t, tl, "p") || tag_is(t, tl, "blockquote") || tag_is(t, tl, "table") || (tag_is(t, tl, "form") && closing)) {
            block_break(1);
            if (tag_is(t, tl, "form")) form = -1;
            continue;
        }
        if (tag_is(t, tl, "pre")) { block_break(1); in_pre = !closing; continue; }
        if (t[0] == 'h' && tl == 2 && t[1] >= '1' && t[1] <= '6') {
            block_break(1);
            cur_style = closing ? ST_TEXT : ST_HEAD;
            continue;
        }
        if (tag_is(t, tl, "ul") || tag_is(t, tl, "ol")) {
            block_break(0);
            if (!closing && list_depth < 7) list_n[++list_depth] = tag_is(t, tl, "ol") ? 1 : 0;
            else if (closing && list_depth > 0) list_depth--;
            continue;
        }
        if (tag_is(t, tl, "li") && !closing) {
            block_break(0);
            char b[8];
            if (list_n[list_depth]) snprintf(b, sizeof(b), "%d.", list_n[list_depth]++);
            else snprintf(b, sizeof(b), "*");
            int save = cur_style;
            cur_style = ST_DIM;
            put_word(b, (int)strlen(b));
            cur_style = save;
            pending_space = 1;
            continue;
        }
        if (tag_is(t, tl, "div") || tag_is(t, tl, "tr") || tag_is(t, tl, "dt") || tag_is(t, tl, "dd") ||
            tag_is(t, tl, "section") || tag_is(t, tl, "article") || tag_is(t, tl, "header") ||
            tag_is(t, tl, "footer") || tag_is(t, tl, "nav") || tag_is(t, tl, "main") || tag_is(t, tl, "aside") ||
            tag_is(t, tl, "figure") || tag_is(t, tl, "figcaption") || tag_is(t, tl, "center") || tag_is(t, tl, "dl")) {
            block_break(0);
            continue;
        }
        if (tag_is(t, tl, "hr")) { block_break(0); for (int i = 0; i < s_cols; i++) { cur_style = ST_DIM; put_char('-'); } cur_style = ST_TEXT; new_line(); continue; }
        if (tag_is(t, tl, "td") || tag_is(t, tl, "th")) { if (!closing) pending_space = 1; continue; }
        if (tag_is(t, tl, "a")) {
            if (closing) {
                if (cur_link >= 0 && !link_had_text) put_word("[link]", 6);
                cur_link = -1;
            } else if (get_attr(t - 1, te, "href", attr, sizeof(attr)) && resolve_url(s_url, attr, abs, sizeof(abs))) {
                cur_link = add_link(L_LINK, abs, 0);
                link_had_text = 0;
            }
            continue;
        }
        if (tag_is(t, tl, "img") && !closing) {
            get_attr(t - 1, te, "alt", attr, sizeof(attr));
            if (attr[0] || (cur_link >= 0 && !link_had_text)) {
                char b[72];
                snprintf(b, sizeof(b), "[%.60s]", attr[0] ? attr : "img");
                int save = cur_style;
                if (cur_link < 0) cur_style = ST_DIM;
                pending_space = 1;
                emit_text(b, b + strlen(b));
                cur_style = save;
            }
            continue;
        }
        if (tag_is(t, tl, "form") && !closing) {
            block_break(0);
            if (s_nforms < MAX_FORMS) {
                form = s_nforms++;
                form_t *f = &s_forms[form];
                if (!get_attr(t - 1, te, "action", attr, sizeof(attr)) || !attr[0]) snprintf(attr, sizeof(attr), "%s", s_url);
                if (!resolve_url(s_url, attr, abs, sizeof(abs))) snprintf(abs, sizeof(abs), "%s", s_url);
                f->action = pstrdup(abs);
                get_attr(t - 1, te, "method", attr, sizeof(attr));
                f->post = !strcasecmp(attr, "post");
                f->hidden = pstrdup("");
            }
            continue;
        }
        if ((tag_is(t, tl, "input") || tag_is(t, tl, "textarea") || tag_is(t, tl, "button")) && !closing && form >= 0) {
            char type[16], name[64], val[72];
            get_attr(t - 1, te, "type", type, sizeof(type));
            get_attr(t - 1, te, "name", name, sizeof(name));
            get_attr(t - 1, te, "value", val, sizeof(val));
            if (tag_is(t, tl, "button")) snprintf(type, sizeof(type), "submit");
            if (!strcasecmp(type, "hidden")) {
                if (name[0]) {
                    char en[200], ev[200], *old = s_forms[form].hidden;
                    url_encode(name, en, sizeof(en));
                    url_encode(val, ev, sizeof(ev));
                    size_t l = strlen(old) + strlen(en) + strlen(ev) + 3;
                    char *nw = heap_caps_malloc(l, MALLOC_CAP_SPIRAM);
                    if (!nw) nw = malloc(l);
                    if (nw) { snprintf(nw, l, "%s%s%s=%s", old, old[0] ? "&" : "", en, ev); free(old); s_forms[form].hidden = nw; }
                }
            } else if (!strcasecmp(type, "submit") || !strcasecmp(type, "image")) {
                int id = add_link(L_SUBMIT, NULL, form);
                if (id >= 0) {
                    s_links[id].name = name[0] ? pstrdup(name) : NULL;
                    snprintf(s_links[id].value, sizeof(s_links[id].value), "%s", val[0] ? val : "Go");
                    show_value(id);
                }
            } else if (!type[0] || !strcasecmp(type, "text") || !strcasecmp(type, "search") || !strcasecmp(type, "email") ||
                       !strcasecmp(type, "url") || !strcasecmp(type, "number") || !strcasecmp(type, "tel") ||
                       !strcasecmp(type, "password")) {
                if (name[0]) {
                    int id = add_link(L_INPUT, NULL, form);
                    if (id >= 0) {
                        s_links[id].name = pstrdup(name);
                        snprintf(s_links[id].value, sizeof(s_links[id].value), "%s", val);
                        show_value(id);
                    }
                }
            }
            continue;
        }
    }
    if (s > text) emit_text(text, s);
    if (col > 0) new_line();
}

/* ---------------- screen ---------------- */

static int s_scroll, s_sel = -1, s_rows;
static bool s_games_saved;

static void draw_bar_text(uint16_t *fb, int y, const char *s, uint16_t c)
{
    gfx_text(fb, 2, y, s, c, (W - 4) / CHAR_W);
}

static void draw_page(uint16_t *fb)
{
    gfx_rect(fb, 0, 0, W, H, RGB(16, 18, 30));
    for (int r = 0; r < s_rows && s_scroll + r < s_lines; r++) {
        int ln = s_scroll + r, y = TOP_H + 2 + r * LINE_H;
        const char *tx = s_txt + ln * MAX_COLS;
        const uint8_t *st = s_sty + ln * MAX_COLS;
        const uint16_t *lk = s_lnk + ln * MAX_COLS;
        for (int c = 0; c < s_cols;) {
            int e = c;
            while (e < s_cols && st[e] == st[c] && lk[e] == lk[c]) e++;
            uint16_t color = st[c] == ST_HEAD ? RGB(255, 205, 70) : st[c] == ST_DIM ? RGB(120, 126, 150)
                           : st[c] == ST_LINK ? RGB(110, 175, 255) : RGB(225, 228, 238);
            if (lk[c] && lk[c] - 1 == s_sel) {
                gfx_rect(fb, 2 + c * CHAR_W - 1, y - 1, (e - c) * CHAR_W + 1, LINE_H, RGB(110, 175, 255));
                color = RGB(10, 14, 30);
            }
            char seg[MAX_COLS + 1];
            memcpy(seg, tx + c, e - c);
            seg[e - c] = 0;
            gfx_text(fb, 2 + c * CHAR_W, y, seg, color, 0);
            c = e;
        }
    }
    /* scrollbar */
    if (s_lines > s_rows) {
        int ah = H - TOP_H - BOT_H, bh = ah * s_rows / s_lines;
        if (bh < 4) bh = 4;
        int by = TOP_H + (ah - bh) * s_scroll / (s_lines - s_rows);
        gfx_rect(fb, W - 2, by, 2, bh, RGB(90, 96, 130));
    }
    gfx_rect(fb, 0, 0, W, TOP_H, RGB(40, 70, 140));
    draw_bar_text(fb, 2, s_title[0] ? s_title : s_url, RGB(240, 240, 250));
    gfx_rect(fb, 0, H - BOT_H, W, BOT_H, RGB(22, 24, 40));
    const char *hint = s_sel >= 0 && s_links[s_sel].type == L_INPUT ? T("A Type  B Back  START Menu")
                                                                       : T("A Open  B Back  START Menu");
    draw_bar_text(fb, H - 8, hint, RGB(130, 136, 160));
}

static void busy_screen(const char *l1, const char *l2, int pct)
{
    uint16_t *fb = display_fb();
    gfx_vgradient(fb, 0, 0, W, H, 0x1c2040, 0x090a14);
    ui_draw_spinner(fb, W / 2, H / 2 - 22, esp_timer_get_time() / 1e6f, RGB(110, 175, 255));
    gfx_text_center(fb, H / 2 - 4, l1, RGB(230, 232, 240));
    if (l2) {
        char s[MAX_COLS + 1];
        snprintf(s, sizeof(s), "%s", l2);
        gfx_text_center(fb, H / 2 + 8, s, RGB(130, 136, 160));
    }
    if (pct >= 0) {
        gfx_rect(fb, 15, H / 2 + 22, W - 30, 6, RGB(30, 33, 56));
        gfx_rect(fb, 15, H / 2 + 22, (W - 30) * pct / 100, 6, RGB(110, 175, 255));
    }
    gfx_text_center(fb, H - 10, T("B Cancel"), RGB(100, 104, 130));
    display_present();
}

static void host_of(const char *url, char *o, size_t n)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    snprintf(o, n, "%.*s", (int)strcspn(h, "/?#"), h);
}

/* ---------------- downloads ---------------- */

static bool is_game_file(const char *name)
{
    const char *e = strrchr(name, '.');
    if (!e) return false;
    static const char *const ext[] = { ".nes", ".gb", ".gbc", ".dmg", ".sms", ".sg", ".gg", ".pce", ".wad", ".zip" };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) if (!strcasecmp(e, ext[i])) return true;
    return false;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s == '+' ? ' ' : *s;
    }
    *o = 0;
}

static void pick_filename(const net_t *n, const char *url, char *name, size_t sz)
{
    name[0] = 0;
    const char *d = strcasestr(n->disp, "filename=");
    if (d) {
        d += 9;
        if (*d == '"') d++;
        snprintf(name, sz, "%.*s", (int)strcspn(d, "\";"), d);
    }
    if (!name[0]) {
        char tmp[URL_MAX];
        snprintf(tmp, sizeof(tmp), "%s", url);
        tmp[strcspn(tmp, "?#")] = 0;
        char *sl = strrchr(tmp, '/');
        snprintf(name, sz, "%s", sl && sl[1] ? sl + 1 : "download.bin");
        url_decode(name);
    }
    /* keep it FAT friendly */
    for (char *p = name; *p; p++)
        if (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 32) *p = '_';
    if (!name[0] || name[0] == '.') snprintf(name, sz, "download.bin");
}

static bool download(net_t *n, const char *url)
{
    char name[96], path[200], dir[64];
    pick_filename(n, url, name, sizeof(name));
    bool game = is_game_file(name);
    snprintf(dir, sizeof(dir), "%s", game ? DIR_ROMS_ : DIR_DOWNLOADS);
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    for (int i = 1; i < 100 && stat(path, &st) == 0; i++) {
        const char *dot = strrchr(name, '.');
        int bl = dot ? (int)(dot - name) : (int)strlen(name);
        snprintf(path, sizeof(path), "%s/%.*s(%d)%s", dir, bl, name, i, dot ? dot : "");
    }
    FILE *f = fopen(path, "wb");
    if (!f) { message_box(T("Can't write to the SD card"), NULL, 0); return false; }
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(16384);
    long long got = 0;
    bool ok = buf != NULL, cancelled = false;
    int64_t t0 = esp_timer_get_time(), tdraw = 0;
    while (ok) {
        int r = net_read(n, buf, 16384);
        if (r < 0) { ok = false; break; }
        if (r == 0) break;
        if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        got += r;
        int64_t now = esp_timer_get_time();
        if (now - tdraw > 150000) {
            tdraw = now;
            char l2[48];
            int kbs = (int)(got * 1000000 / 1024 / (now - t0 + 1));
            if (n->len > 0) snprintf(l2, sizeof(l2), "%lld/%lld KB  %d KB/s", got / 1024, n->len / 1024, kbs);
            else snprintf(l2, sizeof(l2), "%lld KB  %d KB/s", got / 1024, kbs);
            busy_screen(name, l2, n->len > 0 ? (int)(got * 100 / n->len) : -1);
            input_poll();
            if (input_pressed() & BTN_B) { cancelled = true; ok = false; }
        }
    }
    free(buf);
    fclose(f);
    if (n->len > 0 && got < n->len) ok = false;
    if (!ok) {
        remove(path);
        message_box(cancelled ? T("Download cancelled") : T("Download failed"), NULL, cancelled ? 900 : 0);
        return false;
    }
    if (game) s_games_saved = true;
    message_box(game ? T("Game saved!") : T("File saved!"), path + strlen(SD_MOUNT), 0);
    return true;
}

/* ---------------- navigation ---------------- */

typedef struct { char url[URL_MAX]; int scroll, sel; } hist_t;
static hist_t *s_hist;
static int s_nhist;

static void home_page(void)
{
    /* built-in start page as HTML, rendered by the same parser */
    size_t cap = 8192, k = 0;
    char *h = s_page;
    k += snprintf(h + k, cap - k, "<title>%s</title><h1>%s</h1>", T("Start page"), T("Web browser"));
    k += snprintf(h + k, cap - k, "<a href='rp:search'>%s</a><br><a href='rp:url'>%s</a><p>",
                  T("Search the web"), T("Type an address"));
    k += snprintf(h + k, cap - k, "<h2>%s</h2><ul>", T("Bookmarks"));
    FILE *f = fopen(BOOKMARK_FILE, "r");
    char line[URL_MAX + 100];
    while (f && fgets(line, sizeof(line), f) && k + 700 < cap) {
        line[strcspn(line, "\r\n")] = 0;
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        k += snprintf(h + k, cap - k, "<li><a href='%s'>%s</a>", bar + 1, line);
    }
    if (f) fclose(f);
    for (size_t i = 0; i < sizeof(home_marks) / sizeof(home_marks[0]); i++)
        k += snprintf(h + k, cap - k, "<li><a href='%s'>%s</a>", home_marks[i][1], home_marks[i][0]);
    k += snprintf(h + k, cap - k, "</ul><p><font>%s</font><p>%s",
                  T("Games you download go to /roms and show up in the menu. Other files go to /downloads."),
                  T("Text only: no pictures or JavaScript. If a page looks empty, try Simple view in the START menu."));
    snprintf(s_url, sizeof(s_url), "rp:home");
    s_title[0] = 0;
    parse_html(h, h + k, false);
}

/* Loads url (POST body optional). Returns false if nothing changed. */
static bool load_now(const char *url, const char *post)
{
    if (!strcmp(url, "rp:home")) { home_page(); return true; }
    if (!net_ready()) { message_box(T("Connect to Wi-Fi first"), T("Wi-Fi card on the home screen"), 0); return false; }
    char host[64];
    host_of(url, host, sizeof(host));
    busy_screen(T("Loading..."), host, -1);
    net_t n;
    char final_url[URL_MAX];
    if (!net_open(&n, url, post, final_url, sizeof(final_url))) {
        net_close(&n);
        message_box(T("Can't open the page"), host, 0);
        return false;
    }
    bool html = !n.ctype[0] || strcasestr(n.ctype, "html") || strcasestr(n.ctype, "xml");
    bool text = strcasestr(n.ctype, "text/plain") != NULL;
    bool attach = strcasestr(n.disp, "attachment") != NULL;
    if (n.status >= 400 && (html || text)) attach = false;
    if (attach || (!html && !text)) {
        download(&n, final_url);
        net_close(&n);
        return false;
    }
    int got = 0;
    int64_t tdraw = esp_timer_get_time();
    bool cancelled = false;
    while (got < MAX_PAGE - 1) {
        int r = net_read(&n, s_page + got, MAX_PAGE - 1 - got > 8192 ? 8192 : MAX_PAGE - 1 - got);
        if (r <= 0) break;
        got += r;
        int64_t now = esp_timer_get_time();
        if (now - tdraw > 200000) {
            tdraw = now;
            char l2[40];
            snprintf(l2, sizeof(l2), "%.24s  %d KB", host, got / 1024);
            busy_screen(T("Loading..."), l2, n.len > 0 ? (int)(got * 100LL / n.len) : -1);
            input_poll();
            if (input_pressed() & BTN_B) { cancelled = true; break; }
        }
    }
    net_close(&n);
    if (cancelled) return false;
    s_page[got] = 0;
    doc_free();
    snprintf(s_url, sizeof(s_url), "%s", final_url);
    s_title[0] = 0;
    busy_screen(T("Loading..."), host, 100);
    parse_html(s_page, s_page + got, text && !html);
    if (n.status >= 400 && s_lines < 3) {
        char m[32];
        snprintf(m, sizeof(m), "HTTP %d", n.status);
        snprintf(s_title, sizeof(s_title), "%s", m);
    }
    return true;
}


/* HTTPS (TLS handshakes) needs a lot of stack, more than the menu task has
 * left at this depth, so network work runs in its own task with a big stack
 * while the menu task waits. */
typedef struct { const char *url, *post; bool ret, ext_stack; int link; volatile bool done; } job_t;
static void download_link_now(int id);

#ifndef SIM
static void job_load_task(void *p)
{
    job_t *j = p;
    if (j->link >= 0) download_link_now(j->link);
    else j->ret = load_now(j->url, j->post);
    bool ext = j->ext_stack;
    j->done = true;
    if (ext) vTaskDeleteWithCaps(NULL);
    else vTaskDelete(NULL);
}
#endif

static bool run_job(job_t *j)
{
    j->done = false;
#ifndef SIM
    /* stack in internal RAM if there's room, otherwise in PSRAM */
    j->ext_stack = false;
    bool ok = xTaskCreatePinnedToCore(job_load_task, "web", 24 * 1024, j, 5, NULL, 1) == pdPASS;
    if (!ok) {
        j->ext_stack = true;
        ok = xTaskCreatePinnedToCoreWithCaps(job_load_task, "web", 24 * 1024, j, 5, NULL, 1,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    if (ok) {
        while (!j->done) vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(20)); /* let the task finish deleting itself */
        return j->ret;
    }
#endif
    if (j->link >= 0) download_link_now(j->link);
    else j->ret = load_now(j->url, j->post);
    return j->ret;
}

static bool load(const char *url, const char *post)
{
    job_t j = { .url = url, .post = post, .link = -1 };
    return run_job(&j);
}

static void download_link(int id)
{
    job_t j = { .link = id };
    run_job(&j);
}

/* simpler, explicit navigation helper used everywhere */
static void navigate(const char *url, const char *post)
{
    char prev[URL_MAX];
    int sc = s_scroll, se = s_sel;
    snprintf(prev, sizeof(prev), "%s", s_url);
    if (!load(url, post)) return;
    /* record where we were */
    if (prev[0]) {
        if (s_nhist == HIST_MAX) { memmove(s_hist, s_hist + 1, sizeof(hist_t) * (HIST_MAX - 1)); s_nhist--; }
        snprintf(s_hist[s_nhist].url, URL_MAX, "%s", prev);
        s_hist[s_nhist].scroll = sc;
        s_hist[s_nhist].sel = se;
        s_nhist++;
    }
    s_scroll = 0;
    s_sel = -1;
    for (int i = 0; i < s_nlinks; i++)
        if (s_link_line[i] >= 0 && s_link_line[i] < s_rows) { s_sel = i; break; }
}

static void go_back(bool *quit)
{
    if (!s_nhist) { *quit = true; return; }
    hist_t h = s_hist[--s_nhist];
    if (load(h.url, NULL)) {
        s_scroll = h.scroll < s_lines ? h.scroll : 0;
        s_sel = h.sel < s_nlinks ? h.sel : -1;
    }
}

static void submit_form(int form, int submit_link)
{
    form_t *f = &s_forms[form];
    size_t cap = 1200, k = 0;
    char *q = malloc(cap);
    if (!q) return;
    k += snprintf(q, cap, "%s", f->hidden ? f->hidden : "");
    for (int i = 0; i < s_nlinks && k + 200 < cap; i++) {
        link_t *l = &s_links[i];
        if (l->form != form || !l->name) continue;
        if (l->type == L_SUBMIT && i != submit_link) continue;
        if (l->type == L_LINK) continue;
        char en[128], ev[200];
        url_encode(l->name, en, sizeof(en));
        url_encode(l->value, ev, sizeof(ev));
        k += snprintf(q + k, cap - k, "%s%s=%s", k ? "&" : "", en, ev);
    }
    if (f->post) navigate(f->action, q);
    else {
        char url[URL_MAX];
        char base[URL_MAX];
        snprintf(base, sizeof(base), "%s", f->action);
        base[strcspn(base, "?")] = 0;
        snprintf(url, sizeof(url), "%s?%s", base, q);
        navigate(url, NULL);
    }
    free(q);
}

static void do_search(void)
{
    char q[100] = "";
    if (!ui_keyboard(T("Search"), q, sizeof(q)) || !q[0]) return;
    char e[300], url[URL_MAX];
    url_encode(q, e, sizeof(e));
    snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", e);
    navigate(url, NULL);
}

static void do_address(void)
{
    char a[URL_MAX];
    snprintf(a, sizeof(a), "%s", strncmp(s_url, "http", 4) ? "https://" : s_url);
    if (!ui_keyboard(T("Address"), a, sizeof(a)) || !a[0]) return;
    char url[URL_MAX];
    if (!strstr(a, "://")) {
        if (!strchr(a, '.') || strchr(a, ' ')) { /* not an address: search for it */
            char e[400];
            url_encode(a, e, sizeof(e));
            snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", e);
        } else snprintf(url, sizeof(url), "https://%s", a);
    } else snprintf(url, sizeof(url), "%s", a);
    navigate(url, NULL);
}

static void activate(int id)
{
    if (id < 0 || id >= s_nlinks) return;
    link_t *l = &s_links[id];
    if (l->type == L_INPUT) {
        if (ui_keyboard(l->name, l->value, sizeof(l->value))) {
            /* typing into a search box and pressing OK submits it, like Enter */
            int forms_inputs = 0;
            for (int i = 0; i < s_nlinks; i++) if (s_links[i].form == l->form && s_links[i].type == L_INPUT) forms_inputs++;
            if (forms_inputs == 1) submit_form(l->form, -1);
        }
        return;
    }
    if (l->type == L_SUBMIT) { submit_form(l->form, id); return; }
    if (!strcmp(l->url, "rp:search")) { do_search(); return; }
    if (!strcmp(l->url, "rp:url")) { do_address(); return; }
    navigate(l->url, NULL);
}

static void add_bookmark(void)
{
    if (strncmp(s_url, "http", 4)) return;
    FILE *f = fopen(BOOKMARK_FILE, "a");
    if (!f) return;
    char t[64];
    snprintf(t, sizeof(t), "%s", s_title[0] ? s_title : s_url);
    for (char *p = t; *p; p++) if (*p == '|' || *p == '<' || *p == '>' || *p == '\'') *p = ' ';
    char u[URL_MAX];
    snprintf(u, sizeof(u), "%s", s_url);
    for (char *p = u; *p; p++) if (*p == '\'') *p = ' ';
    fprintf(f, "%s|%s\n", t, u);
    fclose(f);
    ui_toast(T("Bookmark added"));
}

static void download_link_now(int id)
{
    if (id < 0 || id >= s_nlinks || s_links[id].type != L_LINK || strncmp(s_links[id].url, "http", 4)) return;
    if (!net_ready()) { message_box(T("Connect to Wi-Fi first"), NULL, 0); return; }
    net_t n;
    char final_url[URL_MAX], host[64];
    host_of(s_links[id].url, host, sizeof(host));
    busy_screen(T("Loading..."), host, -1);
    if (net_open(&n, s_links[id].url, NULL, final_url, sizeof(final_url))) download(&n, final_url);
    else message_box(T("Can't open the page"), host, 0);
    net_close(&n);
}

/* small list menu, returns choice or -1 */
static int menu(const char *const *items, int n)
{
    int sel = 0;
    snapshot_take();
    for (;;) {
        uint16_t *fb = display_fb();
        snapshot_draw(fb);
        int bw = W - 20, bh = n * 12 + 8, bx = 10, by = (H - bh) / 2;
        gfx_round_rect(fb, bx - 1, by - 1, bw + 2, bh + 2, RGB(110, 175, 255));
        gfx_round_rect(fb, bx, by, bw, bh, RGB(24, 27, 46));
        for (int i = 0; i < n; i++) {
            if (i == sel) gfx_round_rect(fb, bx + 3, by + 3 + i * 12, bw - 6, 11, RGB(60, 90, 170));
            gfx_text(fb, bx + 7, by + 5 + i * 12, items[i], i == sel ? RGB(255, 255, 255) : RGB(170, 175, 200), (bw - 12) / CHAR_W);
        }
        display_present();
        input_poll();
        uint32_t r = input_repeat(), p = input_pressed();
        if (r & BTN_UP) sel = (sel + n - 1) % n;
        if (r & BTN_DOWN) sel = (sel + 1) % n;
        if (p & BTN_A) return sel;
        if (p & (BTN_B | BTN_START | BTN_MENU)) return -1;
        if (bridge_quit_requested()) return -1;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void ensure_visible(void)
{
    if (s_sel < 0) return;
    int l = s_link_line[s_sel];
    if (l < s_scroll) s_scroll = l;
    if (l >= s_scroll + s_rows) s_scroll = l - s_rows + 1;
}

static void select_in_view(void)
{
    if (s_sel >= 0 && s_link_line[s_sel] >= s_scroll && s_link_line[s_sel] < s_scroll + s_rows) return;
    s_sel = -1;
    for (int i = 0; i < s_nlinks; i++)
        if (s_link_line[i] >= s_scroll && s_link_line[i] < s_scroll + s_rows) { s_sel = i; break; }
}

static void clamp_scroll(void)
{
    int max = s_lines - s_rows;
    if (max < 0) max = 0;
    if (s_scroll > max) s_scroll = max;
    if (s_scroll < 0) s_scroll = 0;
}

bool browser_run(void)
{
    s_cols = (W - 5) / CHAR_W;
    if (s_cols > MAX_COLS) s_cols = MAX_COLS;
    s_rows = (H - TOP_H - BOT_H - 2) / LINE_H;
    s_page = heap_caps_malloc(MAX_PAGE, MALLOC_CAP_SPIRAM);
    s_txt = heap_caps_malloc(MAX_LINES * MAX_COLS, MALLOC_CAP_SPIRAM);
    s_sty = heap_caps_malloc(MAX_LINES * MAX_COLS, MALLOC_CAP_SPIRAM);
    s_lnk = heap_caps_malloc(MAX_LINES * MAX_COLS * 2, MALLOC_CAP_SPIRAM);
    s_links = heap_caps_malloc(MAX_LINKS * sizeof(link_t), MALLOC_CAP_SPIRAM);
    s_link_line = heap_caps_malloc(MAX_LINKS * sizeof(int), MALLOC_CAP_SPIRAM);
    s_hist = heap_caps_malloc(HIST_MAX * sizeof(hist_t), MALLOC_CAP_SPIRAM);
    s_games_saved = false;
    bool ok = s_page && s_txt && s_sty && s_lnk && s_links && s_link_line && s_hist;
    if (ok) {
        mkdir(DIR_DOWNLOADS, 0777);
        s_nhist = 0; s_nlinks = s_nforms = 0; s_url[0] = 0;
        bridge_set_now_playing("Web browser");
        home_page();
        s_scroll = 0;
        s_sel = -1;
        select_in_view();
#ifdef SIM
        if (getenv("BROWSER_URL")) navigate(getenv("BROWSER_URL"), NULL);
#endif
        bool quit = false;
        while (!quit) {
            if (bridge_quit_requested()) break;
            input_poll();
            uint32_t r = input_repeat(), p = input_pressed();
            if (r & BTN_DOWN) { s_scroll++; clamp_scroll(); select_in_view(); }
            if (r & BTN_UP) { s_scroll--; clamp_scroll(); select_in_view(); }
            if (r & BTN_SELECT) { s_scroll += s_rows - 1; clamp_scroll(); select_in_view(); }
            if (r & BTN_RIGHT) {
                for (int i = s_sel + 1; i < s_nlinks; i++) if (s_link_line[i] >= 0) { s_sel = i; break; }
                ensure_visible();
            }
            if (r & BTN_LEFT) {
                for (int i = s_sel - 1; i >= 0; i--) if (s_link_line[i] >= 0) { s_sel = i; break; }
                ensure_visible();
            }
            if (p & BTN_A) activate(s_sel);
            if (p & BTN_B) go_back(&quit);
            if (p & (BTN_START | BTN_MENU)) {
                const char *items[8];
                int act[8], n = 0;
                enum { A_SEARCH, A_URL, A_HOME, A_BOOK, A_DL, A_SIMPLE, A_RELOAD, A_QUIT };
                items[n] = T("Search the web"); act[n++] = A_SEARCH;
                items[n] = T("Type an address"); act[n++] = A_URL;
                items[n] = T("Start page / bookmarks"); act[n++] = A_HOME;
                if (!strncmp(s_url, "http", 4)) { items[n] = T("Bookmark this page"); act[n++] = A_BOOK; }
                if (s_sel >= 0 && s_links[s_sel].type == L_LINK && !strncmp(s_links[s_sel].url, "http", 4)) {
                    items[n] = T("Save link to SD card"); act[n++] = A_DL;
                }
                if (!strncmp(s_url, "http", 4)) { items[n] = T("Simple view (FrogFind)"); act[n++] = A_SIMPLE; }
                items[n] = T("Exit browser"); act[n++] = A_QUIT;
                int c = menu(items, n);
                if (c >= 0) switch (act[c]) {
                    case A_SEARCH: do_search(); break;
                    case A_URL: do_address(); break;
                    case A_HOME: navigate("rp:home", NULL); break;
                    case A_BOOK: add_bookmark(); break;
                    case A_DL: download_link(s_sel); break;
                    case A_SIMPLE: {
                        char e[URL_MAX], url[URL_MAX];
                        url_encode(s_url, e, sizeof(e));
                        snprintf(url, sizeof(url), "http://frogfind.com/read.php?a=%s", e);
                        navigate(url, NULL);
                        break;
                    }
                    case A_QUIT: quit = true; break;
                }
            }
            uint16_t *fb = display_fb();
            gfx_set_origin(0, 0);
            draw_page(fb);
            ui_draw_toast(fb, 0.016f);
            display_present();
            vTaskDelay(pdMS_TO_TICKS(16));
        }
        doc_free();
    } else {
        message_box(T("Out of memory"), NULL, 0);
    }
    free(s_page); free(s_txt); free(s_sty); free(s_lnk); free(s_links); free(s_link_line); free(s_hist);
    s_page = s_txt = NULL; s_sty = NULL; s_lnk = NULL; s_links = NULL; s_link_line = NULL; s_hist = NULL;
    return s_games_saved;
}
