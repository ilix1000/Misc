/*
 * ESP32-C3 Sub-GHz transceiver — scan, capture, replay, send.
 *
 * Wiring
 *   433 MHz OOK receiver DATA -> GPIO4   (3.3V module; 5V needs a divider)
 *   433 MHz OOK transmitter DATA -> GPIO3
 *   SSD1306 OLED  SCL -> GPIO5, SDA -> GPIO6  (3.3V, addr 0x3C)
 *   Buttons to GND: UP -> GPIO7, DOWN -> GPIO10, OK -> GPIO9
 *       short OK = select, long OK = back
 *
 * Decoding uses the Flipper firmware's own Sub-GHz protocol library (~62
 * decoders). Replay re-transmits the raw timing of a captured signal, so it
 * works for any signal the receiver hears, decodable or not.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <furi.h>
#include "environment.h"
#include "receiver.h"
#include "protocols/protocol_items.h"

#include "app.h"
#include "display.h"

/* ------------------------------ pins ------------------------------ */
#define RX_GPIO    4
#define TX_GPIO    3
#define BTN_UP     7
#define BTN_DOWN   10
#define BTN_OK     9

#define FILTER_US  30      /* glitch filter, same as the Flipper worker */
#define HOLDOFF_MS 1200    /* ignore repeats of the same code within this */

#define W 128
#define H 64

/* ------------------------------ captures ------------------------------ */
#define MAX_PULSES 400
#define HISTORY    12

typedef struct {
    bool used;
    char proto[24];
    char text[256];
    uint16_t npulses;
    uint8_t lvl0;             /* level of the first stored pulse */
    uint16_t dur[MAX_PULSES]; /* microseconds, capped at 65535 */
    int64_t time_us;
} Capture;

static Capture history[HISTORY];
static int hist_count;   /* number stored (<= HISTORY) */
static int hist_next;    /* ring write index */
static SemaphoreHandle_t hist_lock;

/* ------------------------------ raw ring (for replay snapshot) ------------------------------ */
#define RING 1600
static uint16_t ring_dur[RING];
static uint8_t ring_lvl[RING];
static uint32_t ring_head; /* next write index */

/* ------------------------------ live scope + RF activity ------------------------------ */
#define SCOPE_BIN_US 250
static uint8_t scope[W];
static volatile uint32_t scope_head;
static uint32_t scope_carry;
static volatile uint32_t pulses_total;

/* ------------------------------ UI state ------------------------------ */
typedef enum { M_MENU, M_SCAN, M_HIST, M_DETAIL, M_SEND, M_ABOUT } Mode;

static volatile Mode mode = M_MENU;
static int menu_sel;
static int hist_sel;
static int detail_idx;   /* which capture is open */
static int send_sel;
static volatile int tx_flash;    /* frames to show the "TX" overlay */
static volatile bool tx_busy;

static QueueHandle_t btn_q;
typedef enum { EV_UP, EV_DOWN, EV_OK, EV_BACK } Event;

/* latest decode for the scanner screen */
static SemaphoreHandle_t cur_lock;
static char cur_proto[24];
static char cur_text[256];
static uint32_t cur_count;
static int64_t cur_time_us;

/* ------------------------------ built-in send list ------------------------------ */
typedef struct {
    const char* name;
    uint32_t key;
    uint8_t bits;
    uint16_t te; /* base element, us */
} Builtin;

static const Builtin builtins[] = {
    {"Princeton A", 0xABCDE1, 24, 390},
    {"Princeton B", 0x123456, 24, 390},
    {"CAME 12bit", 0x000E4, 12, 320},
    {"Holtek 12b", 0x5A3, 12, 320},
    {"Nice FLO 12", 0x0A5, 12, 700},
};
#define N_BUILTIN (int)(sizeof(builtins) / sizeof(builtins[0]))

/* ======================================================================
 *  RX path
 * ====================================================================== */
typedef struct {
    uint32_t duration;
    uint8_t level;
} Pulse;

static QueueHandle_t pulse_q;
static volatile int64_t last_edge_us;

static void IRAM_ATTR rx_isr(void* arg) {
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const int64_t d = now - last_edge_us;
    /* Noise guard: ignore edges closer than 20us. A floating/AGC-noisy OOK
     * receiver can emit edges at hundreds of kHz; queuing every one floods
     * the CPU and trips the watchdog so nothing else (the UI) ever runs.
     * These sub-20us pulses are below the 30us glitch filter anyway, so
     * dropping them here loses nothing and keeps the system responsive. */
    if(d < 20) return;
    last_edge_us = now;
    Pulse p = {
        .duration = d > 0xFFFFFFF ? 0xFFFFFFF : (uint32_t)d,
        .level = !gpio_get_level(RX_GPIO),
    };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(pulse_q, &p, &woken);
    if(woken) portYIELD_FROM_ISR();
}

static void scope_push(bool level, uint32_t dur) {
    pulses_total++;
    uint32_t t = dur + scope_carry;
    uint32_t bins = t / SCOPE_BIN_US;
    scope_carry = t % SCOPE_BIN_US;
    if(bins > W) bins = W;
    uint32_t h = scope_head;
    for(uint32_t i = 0; i < bins; i++) {
        h = (h + 1) % W;
        scope[h] = level;
    }
    scope_head = h;
}

static void ring_push(bool level, uint32_t dur) {
    ring_dur[ring_head] = dur > 65535 ? 65535 : dur;
    ring_lvl[ring_head] = level;
    ring_head = (ring_head + 1) % RING;
}

/* Snapshot the recent raw pulses that make up the just-decoded burst and
 * store them with the decode in the history ring. Runs in the decode task,
 * the same context that fills ring_*, so there is no race with the writer. */
static void capture_store(const char* proto, const char* text) {
    xSemaphoreTake(hist_lock, portMAX_DELAY);
    Capture* c = &history[hist_next];
    memset(c, 0, sizeof(*c));
    strncpy(c->proto, proto, sizeof(c->proto) - 1);
    strncpy(c->text, text, sizeof(c->text) - 1);
    c->time_us = esp_timer_get_time();

    /* Walk back up to ~2*MAX_PULSES and start replay at the EARLIEST
     * inter-frame gap (a long low pulse) in that window, so the snapshot
     * holds whole frames. The most recent pulse is usually the trailing gap
     * that triggered the decode, so we must not start right after it. */
    const int look = MAX_PULSES * 2;
    int best = -1;
    for(int i = 1; i < look; i++) { /* skip i=0 (the trailing gap) */
        int idx = (ring_head + RING - 1 - i) % RING;
        if(ring_lvl[idx] == 0 && ring_dur[idx] >= 8000) {
            best = (idx + 1) % RING; /* keep updating -> furthest back wins */
        }
    }
    if(best < 0) best = (int)((ring_head + RING - look) % RING);
    const int start = best;

    int n = 0;
    c->lvl0 = ring_lvl[start];
    for(int idx = start; idx != (int)ring_head && n < MAX_PULSES; idx = (idx + 1) % RING) {
        c->dur[n++] = ring_dur[idx];
    }
    c->npulses = n;
    c->used = n > 0;

    if(hist_count < HISTORY) hist_count++;
    hist_next = (hist_next + 1) % HISTORY;
    xSemaphoreGive(hist_lock);
}

/* de-dup repeats of the same code */
static uint32_t text_hash(const char* s) {
    uint32_t h = 2166136261u;
    while(*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static FuriString* dec_text;

static void on_decoded(SubGhzReceiver* r, SubGhzProtocolDecoderBase* d, void* ctx) {
    (void)r;
    (void)ctx;
    static uint32_t last_hash;
    static int64_t last_us;

    furi_string_reset(dec_text);
    subghz_protocol_decoder_base_get_string(d, dec_text);
    const char* s = furi_string_get_cstr(dec_text);
    const uint32_t h = text_hash(s);
    const int64_t now = esp_timer_get_time();
    if(h == last_hash && now - last_us < HOLDOFF_MS * 1000) {
        last_us = now;
        return;
    }
    last_hash = h;
    last_us = now;

    capture_store(d->protocol->name, s);

    xSemaphoreTake(cur_lock, portMAX_DELAY);
    strncpy(cur_proto, d->protocol->name, sizeof(cur_proto) - 1);
    strncpy(cur_text, s, sizeof(cur_text) - 1);
    cur_count++;
    cur_time_us = now;
    xSemaphoreGive(cur_lock);

    printf("\n[%s]\n%s", d->protocol->name, s);
    if(s[0] && s[strlen(s) - 1] != '\n') printf("\n");
}

static void decode_task(void* arg) {
    (void)arg;
    SubGhzEnvironment* env = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(env, &subghz_protocol_registry);
    SubGhzReceiver* receiver = subghz_receiver_alloc_init(env);
    subghz_receiver_set_filter(receiver, SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_rx_callback(receiver, on_decoded, NULL);
    dec_text = furi_string_alloc();

    uint32_t fed = 0;
    bool cur_level = false;
    uint32_t cur_dur = 0;
    Pulse p;
    while(1) {
        if(xQueueReceive(pulse_q, &p, portMAX_DELAY) != pdTRUE) continue;
        if(++fed % 256 == 0) taskYIELD();
        if(p.duration < FILTER_US || p.level == cur_level) {
            cur_dur += p.duration;
        } else {
            subghz_receiver_decode(receiver, cur_level, cur_dur);
            scope_push(cur_level, cur_dur);
            ring_push(cur_level, cur_dur);
            cur_level = p.level;
            cur_dur = p.duration;
        }
    }
}

/* ======================================================================
 *  TX path (RMT, 1us resolution, OOK: level 1 = carrier on)
 * ====================================================================== */
static rmt_channel_handle_t tx_chan;
static rmt_encoder_handle_t tx_enc;
static rmt_symbol_word_t txbuf[1400];

static int tx_nsym;
static int tx_half;

static void tx_reset(void) {
    tx_nsym = 0;
    tx_half = 0;
    memset(txbuf, 0, sizeof(txbuf));
}

static void tx_put(int level, uint32_t dur) {
    if(dur == 0) dur = 1;
    while(dur && tx_nsym < (int)(sizeof(txbuf) / sizeof(txbuf[0]))) {
        uint32_t d = dur > 32767 ? 32767 : dur;
        dur -= d;
        if(tx_half == 0) {
            txbuf[tx_nsym].duration0 = d;
            txbuf[tx_nsym].level0 = level;
            tx_half = 1;
        } else {
            txbuf[tx_nsym].duration1 = d;
            txbuf[tx_nsym].level1 = level;
            tx_half = 0;
            tx_nsym++;
        }
    }
}

static void tx_finish(void) {
    if(tx_half == 1) { /* close a half-filled symbol with a short idle */
        txbuf[tx_nsym].duration1 = 1;
        txbuf[tx_nsym].level1 = 0;
        tx_nsym++;
        tx_half = 0;
    }
}

static bool tx_init(void) {
    const rmt_tx_channel_config_t cfg = {
        .gpio_num = TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 4,
    };
    if(rmt_new_tx_channel(&cfg, &tx_chan) != ESP_OK) return false;
    const rmt_copy_encoder_config_t enc = {};
    if(rmt_new_copy_encoder(&enc, &tx_enc) != ESP_OK) return false;
    return rmt_enable(tx_chan) == ESP_OK;
}

static void tx_send_symbols(int repeats, uint32_t gap_us) {
    const rmt_transmit_config_t cfg = {.loop_count = 0, .flags.eot_level = 0};
    tx_busy = true;
    for(int r = 0; r < repeats; r++) {
        rmt_transmit(tx_chan, tx_enc, txbuf, tx_nsym * sizeof(txbuf[0]), &cfg);
        rmt_tx_wait_all_done(tx_chan, portMAX_DELAY);
        if(gap_us) esp_rom_delay_us(gap_us);
    }
    tx_busy = false;
}

/* Replay a captured raw signal */
static void tx_replay(const Capture* c) {
    tx_reset();
    int level = c->lvl0;
    for(int i = 0; i < c->npulses; i++) {
        tx_put(level, c->dur[i]);
        level = !level;
    }
    tx_finish();
    printf("TX replay: %s (%u pulses)\n", c->proto, c->npulses);
    tx_send_symbols(4, 20000);
}

/* Build + send a Princeton-style built-in code */
static void tx_builtin(const Builtin* b) {
    tx_reset();
    const uint32_t te = b->te;
    for(int i = b->bits - 1; i >= 0; i--) {
        const int one = (b->key >> i) & 1;
        tx_put(1, one ? 3 * te : te);
        tx_put(0, one ? te : 3 * te);
    }
    tx_put(1, te);      /* stop bit */
    tx_put(0, 30 * te); /* guard gap */
    tx_finish();
    printf("TX builtin: %s key=0x%lX\n", b->name, (unsigned long)b->key);
    tx_send_symbols(8, 0);
}

/* ======================================================================
 *  Buttons
 * ====================================================================== */
static void button_task(void* arg) {
    (void)arg;
    const int pins[3] = {BTN_UP, BTN_DOWN, BTN_OK};
    const Event ev[3] = {EV_UP, EV_DOWN, EV_OK};
    int stable[3] = {1, 1, 1};
    int raw[3] = {1, 1, 1};
    int cnt[3] = {0, 0, 0};
    int64_t press_us[3] = {0, 0, 0};
    bool longfired[3] = {false, false, false};

    while(1) {
        for(int i = 0; i < 3; i++) {
            int v = gpio_get_level(pins[i]);
            if(v == raw[i]) {
                if(cnt[i] < 3) cnt[i]++;
            } else {
                raw[i] = v;
                cnt[i] = 0;
            }
            if(cnt[i] >= 3 && v != stable[i]) {
                stable[i] = v;
                if(v == 0) { /* pressed */
                    press_us[i] = esp_timer_get_time();
                    longfired[i] = false;
                } else { /* released */
                    if(!longfired[i]) {
                        Event e = ev[i];
                        xQueueSend(btn_q, &e, 0);
                    }
                }
            }
            /* long-press OK = back */
            if(stable[i] == 0 && !longfired[i] && pins[i] == BTN_OK &&
               esp_timer_get_time() - press_us[i] > 600000) {
                longfired[i] = true;
                Event e = EV_BACK;
                xQueueSend(btn_q, &e, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ======================================================================
 *  UI rendering
 * ====================================================================== */
#define HEADER_H 10

static void header(u8g2_t* u, const char* title, uint32_t frame, bool rf) {
    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, 0, 0, W, HEADER_H);
    u8g2_SetDrawColor(u, 0);
    /* antenna icon */
    u8g2_DrawVLine(u, 4, 3, 6);
    u8g2_DrawPixel(u, 3, 2);
    u8g2_DrawPixel(u, 5, 2);
    const int ph = rf ? (frame / 2) % 3 : 0;
    for(int r = 0; r <= ph; r++) {
        u8g2_DrawPixel(u, 7 + r * 2, 2);
        u8g2_DrawPixel(u, 8 + r * 2, 4);
        u8g2_DrawPixel(u, 7 + r * 2, 6);
    }
    u8g2_SetFont(u, u8g2_font_5x7_tr);
    u8g2_DrawStr(u, 16, 2, title);
    u8g2_SetDrawColor(u, 1);
}

static void list_view(
    u8g2_t* u, const char* items[], int n, int sel, int y0, int rows) {
    u8g2_SetFont(u, u8g2_font_6x12_tr);
    int top = sel - rows / 2;
    if(top < 0) top = 0;
    if(top > n - rows) top = n > rows ? n - rows : 0;
    for(int i = 0; i < rows && top + i < n; i++) {
        const int idx = top + i;
        const int y = y0 + i * 13;
        if(idx == sel) {
            u8g2_DrawBox(u, 0, y - 1, W, 12);
            u8g2_SetDrawColor(u, 0);
        }
        u8g2_DrawStr(u, 3, y, items[idx]);
        if(idx == sel) u8g2_SetDrawColor(u, 1);
    }
    /* scrollbar */
    if(n > rows) {
        const int bar = (H - y0) * rows / n;
        const int pos = (H - y0) * top / n;
        u8g2_DrawVLine(u, W - 1, y0, H - y0);
        u8g2_DrawBox(u, W - 2, y0 + pos, 2, bar > 2 ? bar : 2);
    }
}

static void draw_menu(u8g2_t* u, uint32_t frame) {
    header(u, "SUB-GHZ  MAIN MENU", frame, pulses_total > 0);
    static const char* items[] = {"Scanner", "History", "Send code", "About"};
    list_view(u, items, 4, menu_sel, 14, 4);
}

static void draw_scope(u8g2_t* u, int y) {
    u8g2_DrawHLine(u, 0, y - 2, W);
    const uint32_t head = scope_head;
    int prev = -1;
    for(int x = 0; x < W; x++) {
        const int lv = scope[(head + 1 + x) % W];
        const int yy = lv ? y : y + 8;
        u8g2_DrawPixel(u, x, yy);
        if(prev >= 0 && prev != lv) u8g2_DrawVLine(u, x, y, 9);
        prev = lv;
    }
}

static int split_lines(const char* text, char lines[][40], int maxl, bool skip_first) {
    int n = 0;
    const char* p = text;
    if(skip_first) {
        const char* nl = strchr(text, '\n');
        p = nl ? nl + 1 : text;
    }
    while(*p && n < maxl) {
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while(len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if(len > 39) len = 39;
        if(len) {
            memcpy(lines[n], p, len);
            lines[n][len] = 0;
            n++;
        }
        if(!e) break;
        p = e + 1;
    }
    return n;
}

static void draw_scan(u8g2_t* u, uint32_t frame, uint32_t pps) {
    char proto[24], text[256];
    uint32_t count;
    int64_t t;
    xSemaphoreTake(cur_lock, portMAX_DELAY);
    strcpy(proto, cur_proto);
    strcpy(text, cur_text);
    count = cur_count;
    t = cur_time_us;
    xSemaphoreGive(cur_lock);

    char h[24];
    snprintf(h, sizeof(h), "SCAN  #%lu", (unsigned long)count);
    header(u, h, frame, pps > 200);

    if(count == 0) {
        const int cx = 20, cy = 32, r = 15;
        u8g2_DrawCircle(u, cx, cy, r, U8G2_DRAW_ALL);
        u8g2_DrawPixel(u, cx, cy);
        const float a = frame * 0.18f;
        for(int k = 0; k < 4; k++) {
            float aa = a - k * 0.12f;
            u8g2_DrawLine(u, cx, cy, cx + (int)(cosf(aa) * r), cy + (int)(sinf(aa) * r));
        }
        u8g2_SetFont(u, u8g2_font_7x13B_tr);
        u8g2_DrawStr(u, 44, 18, "LISTEN");
        u8g2_SetFont(u, u8g2_font_5x7_tr);
        static const char* d[] = {"", ".", "..", "..."};
        char s[20];
        snprintf(s, sizeof(s), "433 OOK%s", d[(frame / 4) % 4]);
        u8g2_DrawStr(u, 44, 33, s);
    } else {
        u8g2_SetFont(u, u8g2_font_7x13B_tr);
        u8g2_DrawStr(u, 0, 11, proto);
        if(esp_timer_get_time() - t < 3000000 && (frame / 3) % 2 == 0) {
            u8g2_SetFont(u, u8g2_font_5x7_tr);
            u8g2_DrawRBox(u, W - 22, 12, 22, 10, 2);
            u8g2_SetDrawColor(u, 0);
            u8g2_DrawStr(u, W - 19, 13, "NEW");
            u8g2_SetDrawColor(u, 1);
        }
        char lines[6][40];
        int n = split_lines(text, lines, 6, true);
        u8g2_SetFont(u, u8g2_font_5x7_tr);
        for(int i = 0; i < 3 && i < n; i++) u8g2_DrawStr(u, 0, 25 + i * 8, lines[i]);
    }
    draw_scope(u, 53);
}

static void draw_hist(u8g2_t* u, uint32_t frame) {
    header(u, "HISTORY", frame, false);
    xSemaphoreTake(hist_lock, portMAX_DELAY);
    if(hist_count == 0) {
        xSemaphoreGive(hist_lock);
        u8g2_SetFont(u, u8g2_font_6x12_tr);
        u8g2_DrawStr(u, 10, 28, "No captures yet.");
        u8g2_DrawStr(u, 10, 42, "Use Scanner.");
        return;
    }
    /* newest first */
    static char rows[HISTORY][40];
    const char* items[HISTORY];
    for(int i = 0; i < hist_count; i++) {
        int idx = (hist_next - 1 - i + HISTORY * 2) % HISTORY;
        Capture* c = &history[idx];
        char key[24];
        char lines[4][40];
        int n = split_lines(c->text, lines, 4, true);
        key[0] = 0;
        for(int k = 0; k < n; k++)
            if(strncmp(lines[k], "Key:", 4) == 0) {
                strncpy(key, lines[k] + 4, sizeof(key) - 1);
                break;
            }
        snprintf(rows[i], sizeof(rows[i]), "%-9.9s %s", c->proto, key);
        items[i] = rows[i];
    }
    xSemaphoreGive(hist_lock);
    list_view(u, items, hist_count, hist_sel, 14, 4);
}

static void draw_detail(u8g2_t* u, uint32_t frame) {
    xSemaphoreTake(hist_lock, portMAX_DELAY);
    int idx = (hist_next - 1 - detail_idx + HISTORY * 2) % HISTORY;
    Capture c = history[idx];
    xSemaphoreGive(hist_lock);

    header(u, "CAPTURE", frame, false);
    u8g2_SetFont(u, u8g2_font_7x13B_tr);
    u8g2_DrawStr(u, 0, 11, c.proto);

    char lines[6][40];
    int n = split_lines(c.text, lines, 6, true);
    u8g2_SetFont(u, u8g2_font_5x7_tr);
    for(int i = 0; i < 3 && i < n; i++) u8g2_DrawStr(u, 0, 25 + i * 8, lines[i]);

    /* action hint */
    u8g2_DrawHLine(u, 0, 55, W);
    u8g2_SetFont(u, u8g2_font_5x7_tr);
    if(tx_flash > 0 || tx_busy) {
        u8g2_DrawBox(u, 0, 56, W, 8);
        u8g2_SetDrawColor(u, 0);
        u8g2_DrawStr(u, 40, 57, "TRANSMITTING");
        u8g2_SetDrawColor(u, 1);
    } else {
        char s[32];
        snprintf(s, sizeof(s), "OK=replay  %up", c.npulses);
        u8g2_DrawStr(u, 2, 57, s);
    }
}

static void draw_send(u8g2_t* u, uint32_t frame) {
    header(u, "SEND CODE", frame, false);
    const char* items[N_BUILTIN];
    for(int i = 0; i < N_BUILTIN; i++) items[i] = builtins[i].name;
    list_view(u, items, N_BUILTIN, send_sel, 14, 3);
    u8g2_DrawHLine(u, 0, 55, W);
    u8g2_SetFont(u, u8g2_font_5x7_tr);
    if(tx_flash > 0 || tx_busy) {
        u8g2_DrawBox(u, 0, 56, W, 8);
        u8g2_SetDrawColor(u, 0);
        u8g2_DrawStr(u, 40, 57, "TRANSMITTING");
        u8g2_SetDrawColor(u, 1);
    } else {
        u8g2_DrawStr(u, 2, 57, "OK=send  hold OK=back");
    }
}

static void draw_about(u8g2_t* u, uint32_t frame) {
    header(u, "ABOUT", frame, false);
    u8g2_SetFont(u, u8g2_font_5x7_tr);
    u8g2_DrawStr(u, 0, 14, "C3 Sub-GHz transceiver");
    u8g2_DrawStr(u, 0, 24, "62 Flipper decoders");
    u8g2_DrawStr(u, 0, 34, "RX GPIO4  TX GPIO3");
    u8g2_DrawStr(u, 0, 44, "OLED SCL5 SDA6");
    u8g2_DrawStr(u, 0, 54, "Btn 7/10/9  holdOK=back");
}

static void draw_boot(u8g2_t* u, int step) {
    u8g2_ClearBuffer(u);
    u8g2_SetFont(u, u8g2_font_7x13B_tr);
    const char* t = "SUB-GHZ";
    u8g2_DrawStr(u, (W - u8g2_GetStrWidth(u, t)) / 2, 14, t);
    u8g2_SetFont(u, u8g2_font_5x7_tr);
    t = "transceiver";
    u8g2_DrawStr(u, (W - u8g2_GetStrWidth(u, t)) / 2, 30, t);
    u8g2_DrawFrame(u, 14, 44, 100, 8);
    u8g2_DrawBox(u, 16, 46, step, 4);
    display_flush();
}

/* ======================================================================
 *  UI task: events + render
 * ====================================================================== */
static void do_replay_current(void) {
    tx_flash = 6;
    xSemaphoreTake(hist_lock, portMAX_DELAY);
    int idx = (hist_next - 1 - detail_idx + HISTORY * 2) % HISTORY;
    Capture c = history[idx];
    xSemaphoreGive(hist_lock);
    if(c.used) tx_replay(&c);
}

static void handle_event(Event e) {
    switch(mode) {
    case M_MENU:
        if(e == EV_UP) menu_sel = (menu_sel + 3) % 4;
        else if(e == EV_DOWN) menu_sel = (menu_sel + 1) % 4;
        else if(e == EV_OK) {
            if(menu_sel == 0) mode = M_SCAN;
            else if(menu_sel == 1) { hist_sel = 0; mode = M_HIST; }
            else if(menu_sel == 2) { send_sel = 0; mode = M_SEND; }
            else mode = M_ABOUT;
        }
        break;
    case M_SCAN:
        if(e == EV_BACK) mode = M_MENU;
        break;
    case M_HIST:
        if(e == EV_BACK) mode = M_MENU;
        else if(hist_count) {
            if(e == EV_UP) hist_sel = (hist_sel + hist_count - 1) % hist_count;
            else if(e == EV_DOWN) hist_sel = (hist_sel + 1) % hist_count;
            else if(e == EV_OK) { detail_idx = hist_sel; mode = M_DETAIL; }
        }
        break;
    case M_DETAIL:
        if(e == EV_BACK) mode = M_HIST;
        else if(e == EV_OK) do_replay_current();
        break;
    case M_SEND:
        if(e == EV_BACK) mode = M_MENU;
        else if(e == EV_UP) send_sel = (send_sel + N_BUILTIN - 1) % N_BUILTIN;
        else if(e == EV_DOWN) send_sel = (send_sel + 1) % N_BUILTIN;
        else if(e == EV_OK) { tx_flash = 6; tx_builtin(&builtins[send_sel]); }
        break;
    case M_ABOUT:
        if(e == EV_BACK || e == EV_OK) mode = M_MENU;
        break;
    }
}

static void ui_task(void* arg) {
    (void)arg;
    u8g2_t* u = display_u8g2();
    for(int i = 0; i <= 96; i += 8) {
        draw_boot(u, i);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    uint32_t frame = 0, last_pulses = 0;
    int64_t last_rate = esp_timer_get_time();
    uint32_t pps = 0;

    while(1) {
        Event e;
        while(xQueueReceive(btn_q, &e, 0) == pdTRUE) handle_event(e);

        const int64_t now = esp_timer_get_time();
        if(now - last_rate >= 250000) {
            uint32_t p = pulses_total;
            pps = (uint32_t)((uint64_t)(p - last_pulses) * 1000000 / (now - last_rate));
            last_pulses = p;
            last_rate = now;
        }

        u8g2_ClearBuffer(u);
        switch(mode) {
        case M_MENU: draw_menu(u, frame); break;
        case M_SCAN: draw_scan(u, frame, pps); break;
        case M_HIST: draw_hist(u, frame); break;
        case M_DETAIL: draw_detail(u, frame); break;
        case M_SEND: draw_send(u, frame); break;
        case M_ABOUT: draw_about(u, frame); break;
        }
        if(tx_flash > 0) tx_flash--;
        display_flush();
        frame++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ======================================================================
 *  bring-up
 * ====================================================================== */
void app_start(void) {
    hist_lock = xSemaphoreCreateMutex();
    cur_lock = xSemaphoreCreateMutex();
    btn_q = xQueueCreate(16, sizeof(Event));
    pulse_q = xQueueCreate(2048, sizeof(Pulse));

    /* RX pin + interrupt */
    const gpio_config_t rx = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&rx);

    /* buttons */
    const gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BTN_UP) | (1ULL << BTN_DOWN) | (1ULL << BTN_OK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    last_edge_us = esp_timer_get_time();
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(RX_GPIO, rx_isr, NULL);

    if(!tx_init()) printf("TX init failed (GPIO%d)\n", TX_GPIO);
    if(!display_init()) {
        printf("OLED not found (SCL=GPIO5 SDA=GPIO6 addr 0x3C)\n");
    } else {
        /* Clear the power-on RAM noise and show a splash right away, before
         * the radios start, so the screen is confirmed independent of RX. */
        u8g2_t* u = display_u8g2();
        u8g2_ClearBuffer(u);
        u8g2_SetFont(u, u8g2_font_7x13B_tr);
        const char* t = "SUB-GHZ";
        u8g2_DrawStr(u, (W - u8g2_GetStrWidth(u, t)) / 2, 22, t);
        u8g2_SetFont(u, u8g2_font_5x7_tr);
        t = "booting...";
        u8g2_DrawStr(u, (W - u8g2_GetStrWidth(u, t)) / 2, 38, t);
        display_flush();
    }

    printf("Sub-GHz transceiver ready. RX=GPIO4 TX=GPIO3.\n");

    xTaskCreate(decode_task, "decode", 12 * 1024, NULL, tskIDLE_PRIORITY, NULL);
    xTaskCreate(button_task, "btn", 3 * 1024, NULL, 4, NULL);
    xTaskCreate(ui_task, "ui", 8 * 1024, NULL, 3, NULL);
}
