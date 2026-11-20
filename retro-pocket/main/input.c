/*
 * input.c - 4 buttons + analog joystick (with click) -> D-pad style bitmask.
 */
#include "input.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "input";

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_ch_x, s_ch_y;
static int s_center_x = 2048, s_center_y = 2048;
static float s_jx, s_jy;
static uint32_t s_held, s_pressed, s_repeat;
static int64_t s_repeat_at;
static int8_t s_dir_x, s_dir_y; /* -1, 0, 1 with hysteresis */

static const int btn_pins[] = { PIN_BTN_A, PIN_BTN_B, PIN_BTN_START, PIN_BTN_SELECT, PIN_JOY_SW };
static const uint32_t btn_bits[] = { BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_MENU };

static int read_adc(adc_channel_t ch)
{
    int v = 0;
    adc_oneshot_read(s_adc, ch, &v);
    return v;
}

void input_init(void)
{
    gpio_config_t io = { .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    for (int i = 0; i < sizeof(btn_pins) / sizeof(btn_pins[0]); i++)
        if (btn_pins[i] >= 0) io.pin_bit_mask |= 1ULL << btn_pins[i];
    gpio_config(&io);

    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(PIN_JOY_X, &unit, &s_ch_x));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(PIN_JOY_Y, &unit, &s_ch_y));
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&uc, &s_adc));
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_x, &cc));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_y, &cc));

    /* Calibrate the resting position - don't touch the stick while booting. */
    int sx = 0, sy = 0;
    for (int i = 0; i < 16; i++) { sx += read_adc(s_ch_x); sy += read_adc(s_ch_y); }
    s_center_x = sx / 16;
    s_center_y = sy / 16;
    ESP_LOGI(TAG, "joystick center %d,%d", s_center_x, s_center_y);
}

static float axis(int raw, int center)
{
    float v = raw < center ? (float)(raw - center) / center
                           : (float)(raw - center) / (4095 - center);
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

static int8_t hysteresis(float v, int8_t prev)
{
    const float on = JOY_DEADZONE, off = JOY_DEADZONE * 0.7f;
    if (v > on || (prev > 0 && v > off)) return 1;
    if (v < -on || (prev < 0 && v < -off)) return -1;
    return 0;
}

uint32_t input_poll(void)
{
    float x = axis(read_adc(s_ch_x), s_center_x);
    float y = axis(read_adc(s_ch_y), s_center_y);
    if (JOY_SWAP_XY) { float t = x; x = y; y = t; }
    if (JOY_INVERT_X) x = -x;
    if (JOY_INVERT_Y) y = -y;
    s_jx = x; s_jy = y;
    s_dir_x = hysteresis(x, s_dir_x);
    s_dir_y = hysteresis(y, s_dir_y);

    uint32_t held = 0;
    if (s_dir_x < 0) held |= BTN_LEFT;
    if (s_dir_x > 0) held |= BTN_RIGHT;
    if (s_dir_y < 0) held |= BTN_UP;
    if (s_dir_y > 0) held |= BTN_DOWN;
    for (int i = 0; i < sizeof(btn_pins) / sizeof(btn_pins[0]); i++)
        if (btn_pins[i] >= 0 && gpio_get_level(btn_pins[i]) == 0) held |= btn_bits[i];

    s_pressed = held & ~s_held;
    int64_t now = esp_timer_get_time();
    if (s_pressed) {
        s_repeat = s_pressed;
        s_repeat_at = now + 350000;
    } else if (held && now >= s_repeat_at) {
        s_repeat = held & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
        s_repeat_at = now + 90000;
    } else {
        s_repeat = 0;
    }
    s_held = held;
    return held;
}

uint32_t input_pressed(void) { return s_pressed; }
uint32_t input_repeat(void)  { return s_repeat; }
void input_joystick(float *x, float *y) { *x = s_jx; *y = s_jy; }
