/*
 * SSD1306 128x64 I2C OLED low-level driver for the Sub-GHz transceiver.
 * Renders through u8g2 into an off-screen buffer, then pushes it over I2C.
 *
 *   OLED SCL -> GPIO5, SDA -> GPIO6, VCC -> 3.3V, GND -> GND, addr 0x3C
 */
#include "display.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "u8g2.h"

#define OLED_SCL     5
#define OLED_SDA     6
#define OLED_ADDR    0x3C
#define OLED_FREQ_HZ 400000

#define W 128
#define H 64

static i2c_master_dev_handle_t oled;
static u8g2_t u8g2;

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
        0xAE,       0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
        0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    return oled_cmds(init, sizeof(init)) == ESP_OK;
}

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

bool display_init(void) {
    if(!oled_init()) return false;
    uint8_t tile_buf_height;
    u8g2_SetupDisplay(&u8g2, mem_display_cb, u8x8_cad_empty, u8x8_byte_empty, u8x8_dummy_cb);
    uint8_t* buf = u8g2_m_16_8_f(&tile_buf_height);
    u8g2_SetupBuffer(&u8g2, buf, tile_buf_height, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetFontMode(&u8g2, 1);
    u8g2_SetFontPosTop(&u8g2);
    return true;
}

u8g2_t* display_u8g2(void) {
    return &u8g2;
}

void display_flush(void) {
    static const uint8_t window[] = {0x21, 0, W - 1, 0x22, 0, H / 8 - 1};
    oled_cmds(window, sizeof(window));
    static uint8_t buf[1 + W * H / 8];
    buf[0] = 0x40; /* data stream */
    memcpy(&buf[1], u8g2_GetBufferPtr(&u8g2), W * H / 8);
    i2c_master_transmit(oled, buf, sizeof(buf), 200);
}
