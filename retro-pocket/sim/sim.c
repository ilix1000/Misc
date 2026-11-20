/*
 * PC simulator for Retro Pocket. Runs the real launcher/emulator code with:
 *   - a virtual clock (deterministic, as fast as the PC allows)
 *   - a scripted controller (sim script, see README)
 *   - screenshots written as PPM files
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "display.h"
#include "input.h"
#include "system.h"
#include "launcher.h"

/* ---- clock ---- */
static int64_t s_us;
int64_t esp_timer_get_time(void) { return s_us++; }
void vTaskDelay(uint32_t ticks) { s_us += (int64_t)ticks * 1000; }

/* ---- display ---- */
static uint16_t fb[2][LCD_WIDTH * LCD_HEIGHT];
static int back;
static char pending_shot[128];
static long presents;

void display_init(void) {}
void display_set_backlight(int p) {}
uint16_t *display_fb(void) { return fb[back]; }
const uint16_t *display_front(void) { return fb[back ^ 1]; }
void display_wait(void) {}

static void write_ppm(const char *name, const uint16_t *p)
{
    char path[256];
    snprintf(path, sizeof(path), "%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", LCD_WIDTH, LCD_HEIGHT);
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint16_t c = SWAP16(p[i]);
        uint8_t rgb[3] = { (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "shot %s (present #%ld)\n", path, presents);
}

void display_present(void)
{
    presents++;
    if (pending_shot[0]) { write_ppm(pending_shot, fb[back]); pending_shot[0] = 0; }
    back ^= 1;
}

/* ---- input: script lines "<poll#> <KEYS|shot:NAME|quit>" ---- */
typedef struct { long at; uint32_t keys; char shot[64]; int quit; } step_t;
static step_t steps[512];
static int nsteps, cur;
static long polls;
static uint32_t held, pressed, rep, prev;

static uint32_t parse_keys(const char *s)
{
    static const struct { const char *n; uint32_t b; } k[] = {
        {"UP",BTN_UP},{"DOWN",BTN_DOWN},{"LEFT",BTN_LEFT},{"RIGHT",BTN_RIGHT},{"A",BTN_A},
        {"B",BTN_B},{"START",BTN_START},{"SELECT",BTN_SELECT},{"MENU",BTN_MENU} };
    uint32_t r = 0;
    char buf[64];
    strncpy(buf, s, 63); buf[63] = 0;
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
        for (int i = 0; i < 9; i++) if (!strcmp(t, k[i].n)) r |= k[i].b;
    return r;
}

void input_init(void)
{
    const char *path = getenv("SIM_SCRIPT");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) { fprintf(stderr, "set SIM_SCRIPT\n"); exit(1); }
    char line[128];
    long base = 0;
    while (fgets(line, sizeof(line), f) && nsteps < 512) {
        long n; char what[100];
        if (line[0] == '#' || sscanf(line, "%ld %99s", &n, what) != 2) continue;
        base += n; /* times are relative to the previous line */
        step_t *s = &steps[nsteps++];
        s->at = base;
        if (!strncmp(what, "shot:", 5)) { strcpy(s->shot, what + 5); s->keys = (uint32_t)-1; }
        else if (!strcmp(what, "quit")) s->quit = 1;
        else s->keys = strcmp(what, "-") ? parse_keys(what) : 0;
    }
    fclose(f);
}

uint32_t input_poll(void)
{
    polls++;
    while (cur < nsteps && steps[cur].at <= polls) {
        step_t *s = &steps[cur++];
        if (s->quit) { fprintf(stderr, "done after %ld polls\n", polls); exit(0); }
        if (s->shot[0]) strcpy(pending_shot, s->shot);
        else held = s->keys;
    }
    pressed = held & ~prev;
    rep = pressed;
    prev = held;
    return held;
}
uint32_t input_pressed(void) { return pressed; }
uint32_t input_repeat(void) { return rep; }
void input_joystick(float *x, float *y)
{
    *x = held & BTN_LEFT ? -1 : held & BTN_RIGHT ? 1 : 0;
    *y = held & BTN_UP ? -1 : held & BTN_DOWN ? 1 : 0;
}

/* ---- storage ---- */
settings_t g_settings = { .brightness = 80, .gb_palette = 0, .nes_scale = 0, .show_fps = 1 };
void settings_load(void) {}
void settings_save(void) {}
bool sd_ok(void) { return true; }
bool sd_init(void)
{
    const char *d[] = { SD_MOUNT, DIR_ROMS, DIR_ROMS "/nes", DIR_ROMS "/gb", DIR_SAVES, DIR_SAVES "/nes", DIR_SAVES "/gb" };
    for (int i = 0; i < 7; i++) mkdir(d[i], 0777);
    return true;
}

int main(void)
{
    srand(1);
    display_init();
    input_init();
    sd_init();
    launcher_run();
    return 0;
}
