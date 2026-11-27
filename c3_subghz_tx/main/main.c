/*
 * ESP32-C3 Sub-GHz OOK transmitter: Princeton (PT2262) 24-bit
 *
 * Wiring: 433 MHz ASK/OOK transmitter module (e.g. FS1000A)
 *   DATA -> GPIO4, VCC -> 3.3V..5V (module-dependent), GND -> GND
 *
 * The carrier frequency is set by the transmitter module (usually 433.92 MHz).
 * A Flipper Zero in Sub-GHz -> Read, at the module's frequency with the
 * AM650 preset, shows: "Princeton 24bit  Key:0x00ABCDE1  TE:390us"
 *
 * Sends a burst every SEND_INTERVAL_MS, and right away when the BOOT button
 * (GPIO9) is pressed.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define TX_GPIO          4
#define BUTTON_GPIO      9          /* BOOT button on most C3 boards */

#define PRINCETON_KEY    0xABCDE1u  /* 24-bit code to send */
#define PRINCETON_BITS   24
#define TE_US            390        /* Flipper's Princeton te_short */
#define GUARD_TE         30         /* gap after each frame = 30 * TE */
#define REPEATS          10         /* frames per burst */
#define SEND_INTERVAL_MS 3000

static const char* TAG = "subghz_tx";

static rmt_channel_handle_t tx_chan;
static rmt_encoder_handle_t copy_enc;
static rmt_symbol_word_t frame[PRINCETON_BITS + 1];

static void build_frame(uint32_t key) {
    for(int i = 0; i < PRINCETON_BITS; i++) {
        const bool one = (key >> (PRINCETON_BITS - 1 - i)) & 1u;
        frame[i] = (rmt_symbol_word_t){
            .level0 = 1,
            .duration0 = one ? 3 * TE_US : TE_US,
            .level1 = 0,
            .duration1 = one ? TE_US : 3 * TE_US,
        };
    }
    /* Stop bit + guard gap (separates frames, lets the receiver sync) */
    frame[PRINCETON_BITS] = (rmt_symbol_word_t){
        .level0 = 1,
        .duration0 = TE_US,
        .level1 = 0,
        .duration1 = GUARD_TE * TE_US, /* 11700 us, fits 15-bit field */
    };
}

static void send_burst(void) {
    const rmt_transmit_config_t cfg = {.loop_count = 0, .flags.eot_level = 0};
    for(int r = 0; r < REPEATS; r++) {
        ESP_ERROR_CHECK(rmt_transmit(tx_chan, copy_enc, frame, sizeof(frame), &cfg));
    }
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_chan, portMAX_DELAY));
    ESP_LOGI(TAG, "sent Princeton key 0x%06X x%d", (unsigned)PRINCETON_KEY, REPEATS);
}

void app_main(void) {
    const rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, /* 1 tick = 1 us */
        .mem_block_symbols = 48,
        .trans_queue_depth = REPEATS,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &tx_chan));
    const rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &copy_enc));
    ESP_ERROR_CHECK(rmt_enable(tx_chan));

    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    build_frame(PRINCETON_KEY);
    ESP_LOGI(TAG, "Princeton TX on GPIO%d, TE=%dus", TX_GPIO, TE_US);

    TickType_t last = 0;
    while(1) {
        const bool pressed = gpio_get_level(BUTTON_GPIO) == 0;
        if(pressed || (xTaskGetTickCount() - last) >= pdMS_TO_TICKS(SEND_INTERVAL_MS)) {
            send_burst();
            last = xTaskGetTickCount();
            if(pressed) {
                while(gpio_get_level(BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
