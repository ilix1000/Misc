/*
 * display.c - ST7735 / ST7735S driver for 1.77" 128x160 SPI TFTs.
 * Runs the panel in landscape (160x128) and pushes whole frames with DMA.
 */
#include <string.h>
#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

#define FB_PIXELS LCD_PIXELS

int g_lcd_w = LCD_PANEL_LONG, g_lcd_h = LCD_PANEL_SHORT;
static bool s_portrait;

static spi_device_handle_t s_spi;
static uint16_t *s_fb[2];
static int s_back = 0;           /* index of the buffer the app draws into */
static int s_in_flight = 0;      /* number of queued pixel transactions */
/* The S3's SPI can move at most 32 KB per transaction, a frame is 40 KB. */
#define FRAME_CHUNKS 2
static spi_transaction_t s_trans[FRAME_CHUNKS];

static void cmd(uint8_t c, const uint8_t *data, int len)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &c, .user = (void *)0 };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    if (len > 0) {
        spi_transaction_t d = { .length = len * 8, .tx_buffer = data, .user = (void *)1 };
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &d));
    }
}

/* Called by the SPI driver right before each transaction: sets the D/C pin. */
static void IRAM_ATTR pre_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_LCD_DC, (int)t->user);
}

#define DELAY 0x80
static const uint8_t init_seq[] = {
    /* cmd, n_args | DELAY, args..., [delay ms] */
    0x01, DELAY, 150,                       /* SWRESET */
    0x11, DELAY, 255,                       /* SLPOUT */
    0xB1, 3, 0x01, 0x2C, 0x2D,              /* FRMCTR1: normal mode frame rate */
    0xB2, 3, 0x01, 0x2C, 0x2D,              /* FRMCTR2 */
    0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,
    0xB4, 1, 0x07,                          /* INVCTR: no inversion */
    0xC0, 3, 0xA2, 0x02, 0x84,              /* PWCTR1 */
    0xC1, 1, 0xC5,
    0xC2, 2, 0x0A, 0x00,
    0xC3, 2, 0x8A, 0x2A,
    0xC4, 2, 0x8A, 0xEE,
    0xC5, 1, 0x0E,                          /* VMCTR1 */
    0x3A, 1, 0x05,                          /* COLMOD: 16 bit */
    0xE0, 16, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
              0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
    0xE1, 16, 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
              0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
    0x13, DELAY, 10,                        /* NORON */
    0x00                                    /* end */
};

static void set_window(int x0, int y0, int x1, int y1)
{
    int ox = s_portrait ? LCD_Y_OFFSET : LCD_X_OFFSET;
    int oy = s_portrait ? LCD_X_OFFSET : LCD_Y_OFFSET;
    x0 += ox; x1 += ox;
    y0 += oy; y1 += oy;
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    cmd(0x2C, NULL, 0);
}

static void backlight_init(void)
{
    if (PIN_LCD_BL < 0) return;
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tc));
    ledc_channel_config_t cc = {
        .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cc));
}

void display_set_backlight(int percent)
{
    if (PIN_LCD_BL < 0) return;
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_init(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_LCD_DC, .mode = GPIO_MODE_OUTPUT };
    if (PIN_LCD_RST >= 0) io.pin_bit_mask |= 1ULL << PIN_LCD_RST;
    gpio_config(&io);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = FB_PIXELS * 2 / FRAME_CHUNKS,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ, .mode = 0, .spics_io_num = PIN_LCD_CS,
        .queue_size = 4, .pre_cb = pre_cb, .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    for (int i = 0; i < 2; i++) {
        s_fb[i] = heap_caps_calloc(FB_PIXELS, 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_fb[i]);
    }

    if (PIN_LCD_RST >= 0) {
        gpio_set_level(PIN_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    for (const uint8_t *p = init_seq; *p; ) {
        uint8_t c = *p++, n = *p++;
        int has_delay = n & DELAY;
        n &= ~DELAY;
        cmd(c, p, n);
        p += n;
        if (has_delay) {
            int ms = *p++;
            vTaskDelay(pdMS_TO_TICKS(ms == 255 ? 500 : ms));
        }
    }

    display_set_rotation(ROT_LANDSCAPE, false);
    cmd(LCD_INVERT ? 0x21 : 0x20, NULL, 0);

    /* Clear the panel before turning it on so there's no noise flash. */
    display_present();
    display_wait();
    cmd(0x29, NULL, 0); /* DISPON */

    backlight_init();
    display_set_backlight(80);
    ESP_LOGI(TAG, "ST7735 ready (%dx%d @ %d MHz)", LCD_WIDTH, LCD_HEIGHT, LCD_SPI_HZ / 1000000);
}

void display_set_rotation(int rot, bool swap_rb)
{
    /* MADCTL: MV swaps rows/cols (landscape), MX/MY mirror, bit 3 = BGR. */
    static const uint8_t madctl[ROT_COUNT] = { 0x60, 0x00, 0xA0, 0xC0 };
    if (rot < 0 || rot >= ROT_COUNT) rot = ROT_LANDSCAPE;
    display_wait();
    uint8_t m = madctl[rot];
    if (LCD_BGR ^ swap_rb) m |= 0x08;
    cmd(0x36, &m, 1);
    s_portrait = rot == ROT_PORTRAIT || rot == ROT_PORTRAIT_180;
    g_lcd_w = s_portrait ? LCD_PANEL_SHORT : LCD_PANEL_LONG;
    g_lcd_h = s_portrait ? LCD_PANEL_LONG : LCD_PANEL_SHORT;
}

uint16_t *display_fb(void)
{
    return s_fb[s_back];
}

const uint16_t *display_front(void)
{
    return s_fb[s_back ^ 1];
}

void display_wait(void)
{
    while (s_in_flight > 0) {
        spi_transaction_t *done;
        ESP_ERROR_CHECK(spi_device_get_trans_result(s_spi, &done, portMAX_DELAY));
        s_in_flight--;
    }
}

void display_present(void)
{
    display_wait();               /* previous frame must be off the bus */
    set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    const int chunk = FB_PIXELS / FRAME_CHUNKS;
    for (int i = 0; i < FRAME_CHUNKS; i++) {
        memset(&s_trans[i], 0, sizeof(s_trans[i]));
        s_trans[i].length = chunk * 16;
        s_trans[i].tx_buffer = s_fb[s_back] + i * chunk;
        s_trans[i].user = (void *)1;
        ESP_ERROR_CHECK(spi_device_queue_trans(s_spi, &s_trans[i], portMAX_DELAY));
        s_in_flight++;
    }
    s_back ^= 1;                  /* app keeps drawing into the other buffer */
}
