/*
 * SSD1306 128x64 I2C OLED UI for the Sub-GHz scanner.
 *
 * Layout
 *   +--------------------------------+
 *   |)) SUB-GHZ SCANNER        #12 | <- inverted header, antenna animates
 *   | PROTOCOL NAME (big)      NEW | <- or radar animation while idle
 *   | Key:0x00ABCDE1               |
 *   | Sn:0xABCDE Btn:1             |
 *   | Te:390us  GT:Te*30      3s   |
 *   |~~~|_|~|_|~~|___|~|_|~~|_______| <- live signal from the receiver
 *   +--------------------------------+
 */
#include "display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "u8g2.h"

#define OLED_SDA     5
#define OLED_SCL     6
#define OLED_ADDR    0x3C
#define OLED_FREQ_HZ 400000

#define W 128
#define H 64

/* ---------------- shared state (written by the decoder task) ---------------- */

static SemaphoreHandle_t lock;
static char cur_proto[32];
static char cur_text[512];
static uint32_t cur_count;
static int64_t cur_time_us;

static volatile uint32_t pulses_total;

/* Live signal: 128 bins of SCOPE_BIN_US each, newest at scope_head */
#define SCOPE_BIN_US 250
static uint8_t scope[W];
static volatile uint32_t scope_head;
static uint32_t scope_carry_us;

void display_on_pulse(bool level, uint32_t duration_us) {
    pulses_total++;
    uint32_t t = duration_us + scope_carry_us;
    uint32_t bins = t / SCOPE_BIN_US;
    scope_carry_us = t % SCOPE_BIN_US;
    if(bins > W) bins = W;
    uint32_t h = scope_head;
    for(uint32_t i = 0; i < bins; i++) {
        h = (h + 1) % W;
        scope[h] = level;
    }
    scope_head = h;
}

void display_on_decoded(const char* protocol, const char* text) {
    xSemaphoreTake(lock, portMAX_DELAY);
    strncpy(cur_proto, protocol, sizeof(cur_proto) - 1);
    strncpy(cur_text, text, sizeof(cur_text) - 1);
    cur_count++;
    cur_time_us = esp_timer_get_time();
    xSemaphoreGive(lock);
}

/* ---------------- SSD1306 over I2C ---------------- */

static i2c_master_dev_handle_t oled;

static esp_err_t oled_cmds(const uint8_t* cmds, size_t n) {
    uint8_t buf[32];
    buf[0] = 0x00; /* command stream */
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(oled, buf, n + 1, 100);
}

static bool oled_init(void) {
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA,
        .scl_io_num = OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if(i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) return false;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = OLED_FREQ_HZ,
    };
    if(i2c_master_bus_add_device(bus, &dev_cfg, &oled) != ESP_OK) return false;

    static const uint8_t init[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock */
        0xA8, 0x3F, /* multiplex 64 */
        0xD3, 0x00, /* offset */
        0x40,       /* start line 0 */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x00, /* horizontal addressing: whole frame in one write */
        0xA1, 0xC8, /* segment remap / COM scan: normal orientation */
        0xDA, 0x12, /* COM pins */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1, /* precharge */
        0xDB, 0x40, /* VCOM detect */
        0xA4, 0xA6, /* resume RAM, normal (not inverted) */
        0xAF,       /* display on */
    };
    return oled_cmds(init, sizeof(init)) == ESP_OK;
}

static void oled_push(const uint8_t* fb) {
    static const uint8_t window[] = {0x21, 0, W - 1, 0x22, 0, H / 8 - 1};
    oled_cmds(window, sizeof(window));
    static uint8_t buf[1 + W * H / 8];
    buf[0] = 0x40; /* data stream */
    memcpy(&buf[1], fb, W * H / 8);
    i2c_master_transmit(oled, buf, sizeof(buf), 200);
}

/* ---------------- u8g2 as an off-screen renderer ---------------- */

static u8g2_t u8g2;

static const u8x8_display_info_t mem_display_info = {
    .tile_width = W / 8,
    .tile_height = H / 8,
    .pixel_width = W,
    .pixel_height = H,
};

static uint8_t mem_display_cb(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr) {
    (void)arg_int;
    (void)arg_ptr;
    if(msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &mem_display_info);
    }
    return 1;
}

static void gfx_init(void) {
    uint8_t tile_buf_height;
    u8g2_SetupDisplay(&u8g2, mem_display_cb, u8x8_cad_empty, u8x8_byte_empty, u8x8_dummy_cb);
    uint8_t* buf = u8g2_m_16_8_f(&tile_buf_height);
    /* vertical_top_lsb = SSD1306 page layout, so the buffer can be sent as is */
    u8g2_SetupBuffer(&u8g2, buf, tile_buf_height, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetFontMode(&u8g2, 1);
    u8g2_SetFontPosTop(&u8g2);
}

/* ---------------- drawing ---------------- */

#define HEADER_H 10
#define SCOPE_Y  52
#define SCOPE_H  10

static void draw_header(uint32_t frame, uint32_t count, bool rf_busy) {
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, W, HEADER_H);
    u8g2_SetDrawColor(&u8g2, 0);

    /* antenna: mast + arcs that pulse outward */
    u8g2_DrawVLine(&u8g2, 4, 3, 6);
    u8g2_DrawPixel(&u8g2, 3, 2);
    u8g2_DrawPixel(&u8g2, 5, 2);
    const int phase = rf_busy ? (frame / 2) % 3 : 2;
    for(int r = 0; r <= phase; r++) {
        u8g2_DrawPixel(&u8g2, 7 + r * 2, 2);
        u8g2_DrawPixel(&u8g2, 8 + r * 2, 4);
        u8g2_DrawPixel(&u8g2, 7 + r * 2, 6);
    }

    u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
    u8g2_DrawStr(&u8g2, 16, 2, "SUB-GHZ SCANNER");
    char n[16];
    snprintf(n, sizeof(n), "#%lu", (unsigned long)count);
    u8g2_DrawStr(&u8g2, W - 1 - u8g2_GetStrWidth(&u8g2, n), 2, n);
    u8g2_SetDrawColor(&u8g2, 1);
}

static void draw_scope(void) {
    u8g2_DrawHLine(&u8g2, 0, SCOPE_Y - 2, W);
    const uint32_t head = scope_head;
    int prev = -1;
    for(int x = 0; x < W; x++) {
        const int lv = scope[(head + 1 + x) % W];
        const int y = lv ? SCOPE_Y : SCOPE_Y + SCOPE_H - 1;
        u8g2_DrawPixel(&u8g2, x, y);
        if(prev >= 0 && prev != lv) u8g2_DrawVLine(&u8g2, x, SCOPE_Y, SCOPE_H);
        prev = lv;
    }
}

/* Idle: radar sweep + "LISTENING" + RF activity meter */
static void draw_idle(uint32_t frame, uint32_t pps) {
    const int cx = 22, cy = 30, r = 17;
    u8g2_DrawCircle(&u8g2, cx, cy, r, U8G2_DRAW_ALL);
    u8g2_DrawCircle(&u8g2, cx, cy, r / 2, U8G2_DRAW_ALL);
    u8g2_DrawPixel(&u8g2, cx, cy);
    const float a = frame * 0.18f;
    for(int t = 0; t < 4; t++) { /* sweep with a short trail */
        const float aa = a - t * 0.12f;
        u8g2_DrawLine(&u8g2, cx, cy, cx + (int)(cosf(aa) * r), cy + (int)(sinf(aa) * r));
    }

    u8g2_SetFont(&u8g2, u8g2_font_7x13B_tr);
    u8g2_DrawStr(&u8g2, 46, 14, "LISTENING");
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
    static const char* dots[] = {"", ".", "..", "..."};
    char s[24];
    snprintf(s, sizeof(s), "433 MHz OOK%s", dots[(frame / 4) % 4]);
    u8g2_DrawStr(&u8g2, 46, 29, s);

    /* activity meter: pulses per second on a log-ish scale */
    u8g2_DrawStr(&u8g2, 46, 39, "RF");
    int level = 0;
    for(uint32_t v = pps; v > 50 && level < 10; v /= 2) level++;
    for(int i = 0; i < 10; i++) {
        const int x = 60 + i * 6;
        if(i < level)
            u8g2_DrawBox(&u8g2, x, 38, 4, 7);
        else
            u8g2_DrawFrame(&u8g2, x, 41, 4, 4);
    }
}

/* Received code: protocol title, detail lines, age */
static void draw_code(uint32_t frame, const char* proto, const char* text, int64_t age_us) {
    u8g2_SetFont(&u8g2, u8g2_font_7x13B_tr);
    u8g2_DrawStr(&u8g2, 0, 11, proto);

    if(age_us < 3000000 && (frame / 3) % 2 == 0) { /* blinking NEW badge */
        u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
        u8g2_DrawRBox(&u8g2, W - 22, 12, 22, 10, 2);
        u8g2_SetDrawColor(&u8g2, 0);
        u8g2_DrawStr(&u8g2, W - 19, 13, "NEW");
        u8g2_SetDrawColor(&u8g2, 1);
    }

    /* detail lines = Flipper text minus its first line (the title) */
    char lines[8][40];
    int n = 0;
    const char* p = strchr(text, '\n');
    p = p ? p + 1 : text;
    while(*p && n < 8) {
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while(len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if(len) {
            if(len > sizeof(lines[0]) - 1) len = sizeof(lines[0]) - 1;
            memcpy(lines[n], p, len);
            lines[n][len] = 0;
            n++;
        }
        if(!e) break;
        p = e + 1;
    }

    /* 3 rows visible; longer descriptions page through every 2 s */
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
    const int rows = 3;
    const int pages = n > rows ? (n + rows - 1) / rows : 1;
    const int page = (int)((age_us / 2000000) % pages);
    for(int i = 0; i < rows && page * rows + i < n; i++) {
        u8g2_DrawStr(&u8g2, 0, 25 + i * 8, lines[page * rows + i]);
    }

    char age[24];
    const int64_t s = age_us / 1000000;
    if(s < 60)
        snprintf(age, sizeof(age), "%llds", (long long)s);
    else
        snprintf(age, sizeof(age), "%lldm", (long long)(s / 60));
    u8g2_SetFont(&u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&u8g2, W - u8g2_GetStrWidth(&u8g2, age), 43, age);
}

static void draw_boot(int step) {
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_7x13B_tr);
    const char* t = "SUB-GHZ";
    u8g2_DrawStr(&u8g2, (W - u8g2_GetStrWidth(&u8g2, t)) / 2, 14, t);
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tr);
    t = "62 protocols";
    u8g2_DrawStr(&u8g2, (W - u8g2_GetStrWidth(&u8g2, t)) / 2, 30, t);
    u8g2_DrawFrame(&u8g2, 14, 44, 100, 8);
    u8g2_DrawBox(&u8g2, 16, 46, step, 4);
    oled_push(u8g2_GetBufferPtr(&u8g2));
}

static void display_task(void* arg) {
    (void)arg;
    for(int i = 0; i <= 96; i += 8) {
        draw_boot(i);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    uint32_t frame = 0, shown_count = 0, last_pulses = 0;
    int64_t last_rate_us = esp_timer_get_time();
    uint32_t pps = 0;
    int flash = 0;
    char proto[32] = "", text[512] = "";
    uint32_t count = 0;
    int64_t code_time = 0;

    while(1) {
        const int64_t now = esp_timer_get_time();
        if(now - last_rate_us >= 250000) { /* RF activity, pulses/s */
            const uint32_t p = pulses_total;
            pps = (uint32_t)((uint64_t)(p - last_pulses) * 1000000 / (now - last_rate_us));
            last_pulses = p;
            last_rate_us = now;
        }

        xSemaphoreTake(lock, portMAX_DELAY);
        if(cur_count != shown_count) {
            strcpy(proto, cur_proto);
            strcpy(text, cur_text);
            count = cur_count;
            code_time = cur_time_us;
            shown_count = cur_count;
            flash = 3;
        }
        xSemaphoreGive(lock);

        u8g2_ClearBuffer(&u8g2);
        draw_header(frame, count, pps > 200);
        if(count)
            draw_code(frame, proto, text, now - code_time);
        else
            draw_idle(frame, pps);
        draw_scope();

        if(flash > 0) { /* invert-flash when a new code arrives */
            if(flash != 2) {
                u8g2_SetDrawColor(&u8g2, 2);
                u8g2_DrawBox(&u8g2, 0, 0, W, H);
                u8g2_SetDrawColor(&u8g2, 1);
            }
            flash--;
        }

        oled_push(u8g2_GetBufferPtr(&u8g2));
        frame++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool display_start(void) {
    lock = xSemaphoreCreateMutex();
    if(!oled_init()) return false;
    gfx_init();
    /* Above the decoder (idle priority) so the screen stays smooth */
    xTaskCreate(display_task, "oled", 6 * 1024, NULL, 2, NULL);
    return true;
}
