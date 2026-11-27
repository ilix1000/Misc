/*
 * ESP32-C3 Sub-GHz scanner: decodes every protocol the Flipper Zero knows
 *
 * Wiring: 433 MHz (or 315/868 MHz) ASK/OOK receiver module
 *   DATA -> GPIO4, VCC -> 3.3V, GND -> GND
 *   (The C3 is NOT 5V tolerant: with a 5V module, put a 10k/20k divider on DATA.)
 *
 * Uses the Flipper firmware's own Sub-GHz protocol library (Princeton, CAME,
 * Nice FLO, KeeLoq, Holtek, Linear, Security+, Somfy, ... ~65 decoders) and
 * prints on serial the same text the Flipper shows in Sub-GHz -> Read.
 *
 * Note: an OOK receiver only hears AM/OOK remotes; FM ones (and FM TPMS
 * sensors) need an FSK receiver.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include <furi.h>
#include "environment.h"
#include "receiver.h"
#include "protocols/protocol_items.h"

#define RX_GPIO          4
#define FILTER_US        30    /* same glitch filter as the Flipper (subghz_worker) */
#define REPEAT_HOLDOFF_MS 1000 /* don't reprint the same code within this time */

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
    last_edge_us = now;
    Pulse p = {
        .duration = d > 0xFFFFFFF ? 0xFFFFFFF : (uint32_t)d,
        .level = !gpio_get_level(RX_GPIO), /* level of the segment that just ended */
    };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(pulse_q, &p, &woken);
    if(woken) portYIELD_FROM_ISR();
}

static FuriString* text;
static uint32_t count;

/* Remember recently printed codes so one button press prints once per
 * protocol (some signals match two protocols, e.g. CAME and Holtek HT12X). */
#define RECENT_SLOTS 8
static struct {
    uint32_t hash;
    int64_t last_us;
} recent[RECENT_SLOTS];

static uint32_t text_hash(const char* s) {
    uint32_t h = 2166136261u;
    while(*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void on_decoded(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder, void* ctx) {
    (void)receiver;
    (void)ctx;
    furi_string_reset(text);
    subghz_protocol_decoder_base_get_string(decoder, text);
    const char* s = furi_string_get_cstr(text);
    const uint32_t h = text_hash(s);
    const int64_t now = esp_timer_get_time();

    int oldest = 0;
    for(int i = 0; i < RECENT_SLOTS; i++) {
        if(recent[i].hash == h && now - recent[i].last_us < REPEAT_HOLDOFF_MS * 1000) {
            recent[i].last_us = now; /* same press still going */
            return;
        }
        if(recent[i].last_us < recent[oldest].last_us) oldest = i;
    }
    recent[oldest].hash = h;
    recent[oldest].last_us = now;

    count++;
    printf("\n%s", s);
    if(s[0] && s[strlen(s) - 1] != '\n') printf("\n");
}

static void decode_task(void* arg) {
    (void)arg;
    SubGhzEnvironment* env = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(env, &subghz_protocol_registry);
    SubGhzReceiver* receiver = subghz_receiver_alloc_init(env);
    subghz_receiver_set_filter(receiver, SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_rx_callback(receiver, on_decoded, NULL);
    text = furi_string_alloc();

    printf("Reading...\n");

    /* Glitch filter, same as the Flipper's subghz_worker: merge pulses
     * shorter than FILTER_US into the current one. */
    uint32_t fed = 0;
    bool cur_level = false;
    uint32_t cur_dur = 0;
    Pulse p;
    while(1) {
        if(xQueueReceive(pulse_q, &p, portMAX_DELAY) != pdTRUE) continue;
        if(++fed % 256 == 0) taskYIELD(); /* share the CPU with the idle task */
        if(p.duration < FILTER_US || p.level == cur_level) {
            cur_dur += p.duration;
        } else {
            subghz_receiver_decode(receiver, cur_level, cur_dur);
            cur_level = p.level;
            cur_dur = p.duration;
        }
    }
}

void app_main(void) {
    pulse_q = xQueueCreate(2048, sizeof(Pulse));

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io);
    last_edge_us = esp_timer_get_time();
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(RX_GPIO, rx_isr, NULL);

    /* Lowest priority: on a noisy receiver it shares the CPU with the idle task
     * instead of starving it. */
    xTaskCreate(decode_task, "subghz_decode", 12 * 1024, NULL, tskIDLE_PRIORITY, NULL);
}
