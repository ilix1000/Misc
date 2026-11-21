/*
 * emu_doom.c - Doom, on PrBoom (GPLv2) as ported to ESP32 by Retro-Go.
 *
 * Renders natively at the panel's resolution (160x128 in landscape), so there
 * is no scaling pass. Sound is not emulated (no speaker), which also saves CPU.
 *
 * WAD files: any .wad (or zipped .wad) on the SD card. IWADs (DOOM1.WAD
 * shareware, DOOM.WAD, DOOM2.WAD, FREEDOOM1/2.WAD...) start directly; PWADs
 * (mods / extra levels) are started on top of an IWAD found next to them or
 * in /roms/doom.
 *
 * Leaving Doom restarts the device back into the menu: PrBoom's main loop
 * never returns and its globals can't be reset (Retro-Go does the same).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "emulators.h"
#include "romscan.h"
#include "system.h"
#include "bridge.h"
#include "display.h"
#include "input.h"
#include "unzip.h"
#include "video.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define RETRO_GO 1
#include "doomtype.h"
#include "doomstat.h"
#include "doomdef.h"
#include "d_main.h"
#include "d_event.h"
#include "g_game.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "m_argv.h"
#include "r_draw.h"
#include "r_fps.h"
#include "s_sound.h"
#include "st_stuff.h"
#include "v_video.h"
#include "z_zone.h"
#include "w_wad.h"

#ifdef SIM
#include <unistd.h>
static void restart_device(void) { fprintf(stderr, "doom: quit -> restart\n"); exit(0); }
#else
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void restart_device(void) { video_wait(); esp_restart(); }
#endif

#define DIR_DOOM_SAVES DIR_SAVES "/doom"

/* ---------------- globals PrBoom expects ---------------- */
int snd_card = 0, mus_card = 0;     /* no sound hardware */
int snd_samplerate = 11025;
int current_palette = 0;

static uint16_t s_pal[256];          /* panel byte order */
static uint8_t *s_present;           /* copy of the frame being shown by core 0 */

/* ---------------- video ---------------- */

void I_StartFrame(void) {}
void I_UpdateNoBlit(void) {}
bool I_StartDisplay(void) { return true; }
void I_EndDisplay(void) {}

void I_FinishUpdate(void)
{
    /* Hand the frame to core 0 only if it's free, so rendering never waits
     * on the panel; a busy core 0 just means this frame isn't shown. */
    static bool setup;
    if (!setup) { video_setup(SCREENWIDTH, SCREENHEIGHT); setup = true; }
    if (!s_present || video_busy()) return;
    memcpy(s_present, screens[0].data, SCREENWIDTH * SCREENHEIGHT);
    video_submit(s_present, SCREENWIDTH, s_pal);
}

void I_SetPalette(int pal)
{
    uint16_t *p = V_BuildPalette(pal, 16);
    for (int i = 0; i < 256; i++) s_pal[i] = (uint16_t)(p[i] << 8 | p[i] >> 8);
    Z_Free(p);
    current_palette = pal;
}

void I_InitGraphics(void)
{
    for (int i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENWIDTH;
    }
    /* the main screen is drawn into constantly: keep it in internal RAM */
    screens[0].data = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!screens[0].data) screens[0].data = malloc(SCREENWIDTH * SCREENHEIGHT);
    screens[0].not_on_heap = true;
    s_present = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_present) s_present = malloc(SCREENWIDTH * SCREENHEIGHT);

    screens[4].width = SCREENWIDTH;
    screens[4].height = ST_SCALED_HEIGHT + 1;
    screens[4].byte_pitch = SCREENWIDTH;
}

/* ---------------- time ---------------- */

int I_GetTimeMS(void) { return (int)(esp_timer_get_time() / 1000); }
int I_GetTime(void) { return (int)((int64_t)I_GetTimeMS() * TICRATE * realtic_clock_rate / 100000); }

void I_uSleep(unsigned long usecs)
{
#ifdef SIM
    extern void vTaskDelay(uint32_t);
    vTaskDelay(usecs / 1000 + 1);
#else
    vTaskDelay(pdMS_TO_TICKS(usecs / 1000) + 1);
#endif
}

void I_SafeExit(int rc) { restart_device(); }
const char *I_DoomExeDir(void) { return DIR_DOOM_SAVES; }

/* ---------------- sound: none (no speaker) ---------------- */

void I_InitSound(void) {}
void I_ShutdownSound(void) {}
void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch) {}
int I_StartSound(int sfxid, int channel, int vol, int sep, int pitch, int priority) { return -1; }
void I_StopSound(int handle) {}
bool I_SoundIsPlaying(int handle) { return false; }
bool I_AnySoundStillPlaying(void) { return false; }
void I_PlaySong(int handle, int looping) {}
void I_PauseSong(int handle) {}
void I_ResumeSong(int handle) {}
void I_StopSong(int handle) {}
void I_UnRegisterSong(int handle) {}
int I_RegisterSong(const void *data, size_t len) { return 1; }
void I_SetMusicVolume(int volume) {}

/* ---------------- input ---------------- */

/* Each handheld button sends one or more Doom keys; Doom's menus listen to
 * the same codes as movement, so this works in menus and in game. */
static const struct { uint32_t mask; int *key; } keymap[] = {
    { BTN_UP, &key_up },        { BTN_DOWN, &key_down },
    { BTN_LEFT, &key_left },    { BTN_RIGHT, &key_right },
    { BTN_A, &key_fire },       { BTN_A, &key_enter },          /* fire / menu select */
    { BTN_B, &key_use },        { BTN_B, &key_strafe },         /* open doors; hold to strafe */
    { BTN_B, &key_backspace },                                  /* menu back */
    { BTN_START, &key_escape },                                 /* Doom's menu */
    { BTN_SELECT, &key_weapontoggle },                          /* next weapon */
};

void I_StartTic(void)
{
    static uint32_t prev;
    static int64_t menu_down_at;
    static bool menu_was_long;
    uint32_t keys = input_poll();

    if (bridge_quit_requested()) restart_device();

    /* stick click: tap = automap, hold 1 s = quit to the menu */
    uint32_t menu = keys & BTN_MENU;
    int64_t now = esp_timer_get_time();
    if (menu && !(prev & BTN_MENU)) { menu_down_at = now; menu_was_long = false; }
    if (menu && !menu_was_long && now - menu_down_at > 1000000) {
        menu_was_long = true;
        message_box("Leaving Doom...", "Restarting to the menu", 600);
        restart_device();
    }
    if (!menu && (prev & BTN_MENU) && !menu_was_long) {
        event_t ev = { .type = ev_keydown, .data1 = key_map };
        D_PostEvent(&ev);
        ev.type = ev_keyup;
        D_PostEvent(&ev);
    }

    uint32_t changed = (prev ^ keys) & ~BTN_MENU;
    for (size_t i = 0; changed && i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (!(changed & keymap[i].mask)) continue;
        event_t ev = { .type = (keys & keymap[i].mask) ? ev_keydown : ev_keyup, .data1 = *keymap[i].key };
        D_PostEvent(&ev);
    }
    prev = keys;
}

void I_Init(void)
{
    snd_channels = 0;
    snd_MusicVolume = 0;
    snd_SfxVolume = 0;
    autorun = 1;            /* small screen + stick: always run feels right */
}

/* ---------------- zipped WADs ---------------- */

bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data, size_t *size, uint32_t flags)
{
    zip_entry_t e;
    if (!zip_find(zip_path, ".wad", &e)) return false;
    uint8_t *d = zip_extract(zip_path, &e);
    if (!d) return false;
    *data = d;
    *size = e.size;
    return true;
}

/* Unzips a zipped WAD once into /saves/doom/cache so Doom can open it as a
 * file. Returns false if there's no .wad inside. */
static bool unzip_wad(const char *zip, char *out, size_t n)
{
    zip_entry_t e;
    if (!zip_find(zip, ".wad", &e)) return false;
    mkdir(DIR_DOOM_SAVES "/cache", 0777);
    snprintf(out, n, "%s/cache/%s", DIR_DOOM_SAVES, e.name);
    struct stat st;
    if (stat(out, &st) == 0 && (size_t)st.st_size == e.size) return true;   /* already done */
    message_box("Unpacking WAD...", e.name, 1);
    uint8_t *d = zip_extract(zip, &e);
    bool ok = d && file_save(out, d, e.size);
    free(d);
    return ok;
}

static bool is_iwad(const char *path)
{
    char hdr[4] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(hdr, 1, 4, f) == 4 && !memcmp(hdr, "IWAD", 4);
    fclose(f);
    return ok;
}

/* For a PWAD (mod), find a game to run it on: next to it, then /roms/doom. */
static bool find_iwad(const char *near, char *out, size_t n)
{
    static const char *const names[] = { "doom2.wad", "DOOM2.WAD", "plutonia.wad", "PLUTONIA.WAD", "tnt.wad",
        "TNT.WAD", "doom.wad", "DOOM.WAD", "doomu.wad", "DOOMU.WAD", "freedoom2.wad", "freedoom1.wad",
        "doom1.wad", "DOOM1.WAD" };
    char dir[300];
    snprintf(dir, sizeof(dir), "%s", near);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    const char *dirs[] = { dir, DIR_ROMS "/doom", DIR_ROMS, SD_MOUNT };
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++)
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            snprintf(out, n, "%s/%s", dirs[d], names[i]);
            if (is_iwad(out)) return true;
        }
    return false;
}

/* ---------------- start ---------------- */

static char s_iwad[300], s_pwad[300];
static const char *s_argv[10];

static void doom_main(void *arg)
{
    myargv = s_argv;
    myargc = 0;
    s_argv[myargc++] = "doom";
    s_argv[myargc++] = "-save";
    s_argv[myargc++] = DIR_DOOM_SAVES;
    s_argv[myargc++] = "-iwad";
    s_argv[myargc++] = s_iwad;
    if (s_pwad[0]) {
        s_argv[myargc++] = "-file";
        s_argv[myargc++] = s_pwad;
    }
    s_argv[myargc] = NULL;
    Z_Init();
    D_DoomMain();          /* never returns */
    restart_device();
}

void emu_doom_run(const char *path)
{
    char wad[300];
    mkdir(DIR_DOOM_SAVES, 0777);
    if (has_ext(path, ".zip")) {
        if (!unzip_wad(path, wad, sizeof(wad))) { message_box("No .wad in this zip", NULL, 0); return; }
    } else {
        snprintf(wad, sizeof(wad), "%s", path);
    }

    s_pwad[0] = 0;
    if (is_iwad(wad)) {
        snprintf(s_iwad, sizeof(s_iwad), "%s", wad);
    } else {
        snprintf(s_pwad, sizeof(s_pwad), "%s", wad);
        if (!find_iwad(wad, s_iwad, sizeof(s_iwad))) {
            message_box("This is a mod (PWAD)", "Put DOOM2/DOOM/FREEDOOM WAD next to it", 0);
            return;
        }
    }

    /* Doom draws at the panel's own resolution */
    SCREENWIDTH = LCD_WIDTH;
    SCREENHEIGHT = LCD_HEIGHT;

    bridge_set_now_playing("Doom");
#ifdef SIM
    doom_main(NULL);
#else
    /* Everything Doom allocates may live in PSRAM; internal RAM stays for
     * Wi-Fi, the screen buffers and Doom's main screen. Doom gets its own
     * task with a bigger stack, and this (app) task just waits. */
    heap_caps_malloc_extmem_enable(0);
    xTaskCreatePinnedToCore(doom_main, "doom", 48 * 1024, NULL, 5, NULL, 1);
    for (;;) vTaskDelay(portMAX_DELAY);
#endif
}
