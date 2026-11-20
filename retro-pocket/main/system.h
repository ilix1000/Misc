#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Saved in flash as one blob. ONLY ADD NEW FIELDS AT THE END so settings
 * survive firmware updates (see nvs_get_blob_compat). */
typedef struct {
    int brightness;   /* 5..100 */
    int gb_palette;   /* index into GB palettes */
    int nes_scale;    /* 0 = fit whole screen, 1 = crop (sharper, cuts edges) */
    int show_fps;     /* 0/1 */
    int rotation;     /* ROT_* from display.h */
    int stick_rot;    /* joystick turned by 0/90/180/270 degrees */
    int swap_rb;      /* 0/1, swaps red and blue on the panel */
    int ui_anim;      /* 0/1, menu animations */
} settings_t;

extern settings_t g_settings;
void settings_load(void);
void settings_save(void);
#ifndef SIM
#include "nvs.h"
bool nvs_get_blob_compat(nvs_handle_t h, const char *key, void *dst, size_t dst_size);
#endif

bool sd_init(void);
bool sd_ok(void);

/* Reads a whole file into PSRAM. Caller frees with free(). */
uint8_t *file_load(const char *path, size_t *size);
bool file_save(const char *path, const void *data, size_t size);
/* Reads into an existing buffer; true only if exactly `size` bytes were read. */
bool file_read_into(const char *path, void *data, size_t size);
/* "/sd/roms/gb/Foo.gb" + ".sav" -> "/sd/saves/gb/Foo.sav" */
void save_path(char *out, size_t n, const char *rom_path, const char *sys, const char *ext);

typedef enum {
    MENU_RESUME,
    MENU_SAVE_STATE,
    MENU_LOAD_STATE,
    MENU_RESET,
    MENU_QUIT,
} ingame_action_t;

/* Pause menu drawn over the last game frame. `has_states` hides the
 * save/load state entries for systems that don't support them. */
ingame_action_t ingame_menu(const char *title, bool has_states);
/* Shows a message box until a button is pressed (or ms elapses if ms > 0). */
void message_box(const char *line1, const char *line2, int ms);

/* Frame pacing: call once per frame with the system's target frame time. */
typedef struct {
    int64_t next;
    int64_t fps_t0;
    int frames, fps;
} pacer_t;
void pacer_start(pacer_t *p);
/* Returns true if the caller is behind schedule and may skip drawing. */
bool pacer_wait(pacer_t *p, int frame_us);
void draw_fps(uint16_t *fb, int fps);
