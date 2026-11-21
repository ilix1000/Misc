#include <string.h>
#include "video.h"
#include "display.h"
#include "gfx.h"
#include "system.h"
#include "esp_attr.h"
#include "esp_timer.h"

static uint16_t s_xmap[LCD_MAX_DIM], s_ymap[LCD_MAX_DIM];
static int s_ox, s_oy, s_dw, s_dh, s_sw, s_sh;
static bool s_blend_x, s_blend_y;

static const uint8_t *s_src;
static int s_pitch;
static uint16_t s_pal[256];      /* unswapped RGB565 for blending */

static int s_shown, s_fps;
static int64_t s_fps_t0;

void video_setup(int sw, int sh)
{
    s_sw = sw;
    s_sh = sh;
    if (LCD_WIDTH >= LCD_HEIGHT) {
        /* landscape: fill the screen */
        s_dw = LCD_WIDTH;
        s_dh = LCD_HEIGHT;
        /* ...but don't stretch a small picture past its proportions much:
         * Game Gear (160x144) squeezes 144 -> 128 lines like the Game Boy */
    } else {
        /* portrait: full width, keep the aspect ratio */
        s_dw = LCD_WIDTH;
        s_dh = sh * s_dw / sw;
        if (s_dh > LCD_HEIGHT) s_dh = LCD_HEIGHT;
    }
    s_ox = (LCD_WIDTH - s_dw) / 2;
    s_oy = (LCD_HEIGHT - s_dh) / 2;
    for (int x = 0; x < s_dw; x++) s_xmap[x] = x * sw / s_dw;
    for (int y = 0; y < s_dh; y++) s_ymap[y] = y * sh / s_dh;
    s_blend_x = sw > s_dw;   /* only blend when shrinking */
    s_blend_y = sh > s_dh;
}

#define BLEND(a, b) ((uint16_t)((((a) & 0xF7DE) >> 1) + (((b) & 0xF7DE) >> 1)))

static void IRAM_ATTR blit(uint16_t *fb)
{
    if (s_oy > 0) {
        gfx_rect(fb, 0, 0, LCD_WIDTH, s_oy, C_BLACK);
        gfx_rect(fb, 0, s_oy + s_dh, LCD_WIDTH, LCD_HEIGHT - s_oy - s_dh, C_BLACK);
    }
    for (int y = 0; y < s_dh; y++) {
        int sy = s_ymap[y];
        const uint8_t *r0 = s_src + sy * s_pitch;
        const uint8_t *r1 = (s_blend_y && sy + 1 < s_sh) ? r0 + s_pitch : r0;
        uint16_t *dst = fb + (s_oy + y) * LCD_WIDTH + s_ox;
        if (!s_blend_x && !s_blend_y) {
            for (int x = 0; x < s_dw; x++) dst[x] = SWAP16(s_pal[r0[s_xmap[x]]]);
            continue;
        }
        for (int x = 0; x < s_dw; x++) {
            int sx = s_xmap[x], sx1 = (s_blend_x && sx + 1 < s_sw) ? sx + 1 : sx;
            uint16_t top = BLEND(s_pal[r0[sx]], s_pal[r0[sx1]]);
            uint16_t bot = BLEND(s_pal[r1[sx]], s_pal[r1[sx1]]);
            dst[x] = SWAP16(BLEND(top, bot));
        }
    }
}

static void show(void)
{
    uint16_t *fb = display_fb();
    blit(fb);
    s_shown++;
    int64_t now = esp_timer_get_time();
    if (now - s_fps_t0 >= 1000000) { s_fps = s_shown; s_shown = 0; s_fps_t0 = now; }
    if (g_settings.show_fps) draw_fps(fb, s_fps);
    display_present();
}

int video_fps(void) { return s_fps; }

static void take_frame(const uint8_t *src, int pitch, const uint16_t *palette)
{
    s_src = src;
    s_pitch = pitch;
    for (int i = 0; i < 256; i++) s_pal[i] = SWAP16(palette[i]);
}

#ifdef SIM
bool video_submit(const uint8_t *src, int pitch, const uint16_t *palette)
{
    take_frame(src, pitch, palette);
    show();
    return true;
}
void video_wait(void) {}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_work, s_idle;

static void video_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_work, portMAX_DELAY);
        show();
        xSemaphoreGive(s_idle);
    }
}

bool video_submit(const uint8_t *src, int pitch, const uint16_t *palette)
{
    if (!s_work) {
        s_work = xSemaphoreCreateBinary();
        s_idle = xSemaphoreCreateBinary();
        xSemaphoreGive(s_idle);
        xTaskCreatePinnedToCore(video_task, "video", 4096, NULL, 6, NULL, 0);
    }
    if (xSemaphoreTake(s_idle, 0) != pdTRUE) return false;
    take_frame(src, pitch, palette);
    xSemaphoreGive(s_work);
    return true;
}

void video_wait(void)
{
    if (!s_idle) return;
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
    display_wait();
}
#endif
