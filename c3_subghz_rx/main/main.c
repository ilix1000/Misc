/*
 * ESP32-C3 Sub-GHz OOK receiver: Princeton (PT2262) 24-bit decoder
 *
 * Wiring: 433 MHz ASK/OOK receiver module (e.g. RXB6, XY-MK-5V, SRX882)
 *   DATA -> GPIO4, VCC -> 3.3V, GND -> GND
 *   (The C3 is NOT 5V tolerant: if the module runs on 5V, put a divider,
 *    e.g. 10k/20k, between its DATA pin and GPIO4.)
 *
 * Decodes Princeton with the same timing rules as the Flipper Zero decoder
 * and prints every code on serial, e.g.:
 *   Princeton 24bit  Key:0xABCDE1  TE:392us  x8  <-- MATCH (expected 0xABCDE1)
 */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define RX_GPIO        4
#define EXPECTED_KEY   0xABCDE1u   /* the code the C3 transmitter sends */

/* Flipper Zero Princeton constants (lib/subghz/protocols/princeton.c) */
#define TE_SHORT       390
#define TE_LONG        1170
#define TE_DELTA       300
#define BITS           24

#define BURST_GAP_MS   300          /* frames closer than this = same press */

typedef struct {
    uint32_t duration; /* us */
    uint8_t level;     /* level that lasted `duration` */
} Pulse;

static QueueHandle_t pulse_q;
static volatile int64_t last_edge_us;

static void IRAM_ATTR rx_isr(void* arg) {
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const int64_t d = now - last_edge_us;
    last_edge_us = now;
    /* The pin now shows the new level; the segment that just ended had the
     * opposite one. */
    Pulse p = {
        .duration = d > 0xFFFFFFF ? 0xFFFFFFF : (uint32_t)d,
        .level = !gpio_get_level(RX_GPIO),
    };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(pulse_q, &p, &woken); /* drop on overflow */
    if(woken) portYIELD_FROM_ISR();
}

/* ---- Princeton decoder: same state machine as the Flipper's ---- */
typedef enum { StepReset, StepSaveDuration, StepCheckDuration } Step;

static struct {
    Step step;
    uint32_t data, last_data, te_last, te;
    uint8_t count;
} dec;

#define DIFF(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))

/* Returns true and fills key/te when a code is confirmed (two equal frames
 * in a row, exactly like the Flipper). */
static bool decoder_feed(bool level, uint32_t duration, uint32_t* key, uint32_t* te) {
    bool found = false;
    switch(dec.step) {
    case StepReset:
        if(!level && DIFF(duration, TE_SHORT * 36) < TE_DELTA * 36) {
            dec.step = StepSaveDuration;
            dec.data = 0;
            dec.count = 0;
            dec.te = 0;
        }
        break;
    case StepSaveDuration:
        if(level) {
            dec.te_last = duration;
            dec.te += duration;
            dec.step = StepCheckDuration;
        }
        break;
    case StepCheckDuration:
        if(level) {
            dec.step = StepReset;
            break;
        }
        if(duration >= TE_LONG * 2) {
            dec.step = StepSaveDuration;
            if(dec.count == BITS) {
                if(dec.last_data == dec.data && dec.last_data) {
                    *key = dec.data;
                    *te = dec.te / (BITS * 4 + 1);
                    found = true;
                }
                dec.last_data = dec.data;
            }
            dec.data = 0;
            dec.count = 0;
            dec.te = 0;
            break;
        }
        dec.te += duration;
        if(DIFF(dec.te_last, TE_SHORT) < TE_DELTA && DIFF(duration, TE_LONG) < TE_DELTA * 3) {
            dec.data <<= 1;
            dec.count++;
            dec.step = StepSaveDuration;
        } else if(
            DIFF(dec.te_last, TE_LONG) < TE_DELTA * 3 && DIFF(duration, TE_SHORT) < TE_DELTA) {
            dec.data = (dec.data << 1) | 1u;
            dec.count++;
            dec.step = StepSaveDuration;
        } else {
            dec.step = StepReset;
        }
        break;
    }
    return found;
}

static void print_burst(uint32_t key, uint32_t te, unsigned hits) {
    printf("Princeton 24bit  Key:0x%06lX  TE:%luus  x%u  %s\n",
           (unsigned long)key,
           (unsigned long)te,
           hits,
           key == EXPECTED_KEY ? "<-- MATCH (expected 0xABCDE1)" : "(other code)");
}

void app_main(void) {
    pulse_q = xQueueCreate(1024, sizeof(Pulse));

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* idle low if DATA floats */
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io);
    last_edge_us = esp_timer_get_time();
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(RX_GPIO, rx_isr, NULL);

    printf("Princeton RX on GPIO%d, waiting for codes (expecting 0x%06X)...\n",
           RX_GPIO, (unsigned)EXPECTED_KEY);

    /* Group the frames of one button press into one line */
    uint32_t burst_key = 0, burst_te = 0;
    unsigned burst_hits = 0;
    int64_t burst_last_us = 0;

    while(1) {
        Pulse p;
        const bool got = xQueueReceive(pulse_q, &p, pdMS_TO_TICKS(50)) == pdTRUE;
        if(got) {
            uint32_t key, te;
            if(decoder_feed(p.level, p.duration, &key, &te)) {
                const int64_t now = esp_timer_get_time();
                if(burst_hits && key != burst_key) {
                    print_burst(burst_key, burst_te, burst_hits);
                    burst_hits = 0;
                }
                burst_key = key;
                burst_te = te;
                burst_hits++;
                burst_last_us = now;
            }
        }
        if(burst_hits && esp_timer_get_time() - burst_last_us > BURST_GAP_MS * 1000) {
            print_burst(burst_key, burst_te, burst_hits);
            burst_hits = 0;
        }
    }
}
