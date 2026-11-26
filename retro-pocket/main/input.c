/*
 * input.c - 4 buttons + analog joystick (with click) -> D-pad style bitmask.
 */
#include "input.h"
#include "board.h"
#include "system.h"
#include "bridge.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "input";

/* X and Y may be on different ADC units (ADC2 is shared with Wi-Fi). */
static adc_oneshot_unit_handle_t s_unit[2];
typedef struct { adc_oneshot_unit_handle_t h; adc_channel_t ch; int last; } joy_axis_t;
static joy_axis_t s_ax_x, s_ax_y;
static int s_center_x = 2048, s_center_y = 2048;
static float s_jx, s_jy;
static uint32_t s_held, s_pressed, s_repeat;
static int64_t s_repeat_at;
static int8_t s_dir_x, s_dir_y; /* -1, 0, 1 with hysteresis */

static const int btn_pins[] = { PIN_BTN_A, PIN_BTN_B, PIN_BTN_START, PIN_BTN_SELECT, PIN_JOY_SW };
static const uint32_t btn_bits[] = { BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_MENU };

static int read_adc(joy_axis_t *a)
{
    int v;
    /* ADC2 reads can fail while Wi-Fi is using it: keep the last value then */
    if (adc_oneshot_read(a->h, a->ch, &v) == ESP_OK) a->last = v;
    return a->last;
}

static void axis_init(joy_axis_t *a, int pin)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(pin, &unit, &a->ch));
    if (!s_unit[unit]) {
        adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&uc, &s_unit[unit]));
    }
    a->h = s_unit[unit];
    a->last = 2048;
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(a->h, a->ch, &cc));
}

void input_init(void)
{
    gpio_config_t io = { .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    for (int i = 0; i < sizeof(btn_pins) / sizeof(btn_pins[0]); i++)
        if (btn_pins[i] >= 0) io.pin_bit_mask |= 1ULL << btn_pins[i];
    gpio_config(&io);

    axis_init(&s_ax_x, PIN_JOY_X);
    axis_init(&s_ax_y, PIN_JOY_Y);

    /* Calibrate the resting position - don't touch the stick while booting. */
    int sx = 0, sy = 0;
    for (int i = 0; i < 16; i++) { sx += read_adc(&s_ax_x); sy += read_adc(&s_ax_y); }
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
    float x = axis(read_adc(&s_ax_x), s_center_x);
    float y = axis(read_adc(&s_ax_y), s_center_y);
    if (JOY_SWAP_XY) { float t = x; x = y; y = t; }
    if (JOY_INVERT_X) x = -x;
    if (JOY_INVERT_Y) y = -y;
    for (int r = 0; r < (g_settings.stick_rot & 3); r++) { /* 90 degrees clockwise each */
        float t = x; x = -y; y = t;
    }
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

    /* buttons held on the web remote */
    uint32_t remote = bridge_remote_keys();
    held |= remote;
    if (remote & BTN_LEFT) s_jx = -1;
    if (remote & BTN_RIGHT) s_jx = 1;
    if (remote & BTN_UP) s_jy = -1;
    if (remote & BTN_DOWN) s_jy = 1;

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
