#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Values are stored in the zip cache and used by the web UI: only append. */
typedef enum { SYS_NONE = 0, SYS_NES, SYS_GB, SYS_SMS, SYS_GG, SYS_PCE, SYS_DOOM, SYS_COUNT } sys_t;

typedef struct {
    const char *path;   /* full path on the SD card (.nes/.gb/... or .zip) */
    const char *name;   /* display name: file name without extension */
    uint8_t sys;        /* sys_t */
} game_t;

/* Called during the scan so the UI can show progress. */
typedef void (*scan_progress_fn)(int files_seen, int games_found, const char *current);

/* Walks the whole SD card and sorts every game it finds into its system.
 * Zips are opened to see which system the ROM inside belongs to; that
 * answer is cached on the card so later boots are fast. */
void games_scan(scan_progress_fn progress);
int games_count(sys_t sys);
const game_t *games_get(sys_t sys, int index);

/* Finds a scanned game by its path (NULL if it's not on the card). */
const game_t *games_find(const char *path);

/* Favourites (stored in /saves/favorites.txt) and recently played
 * (/saves/recent.txt, newest first). Only games still on the card count. */
bool lib_is_fav(const char *path);
void lib_set_fav(const char *path, bool fav);
int lib_fav_count(void);
const game_t *lib_fav_get(int i);
void lib_add_recent(const char *path);
int lib_recent_count(void);
const game_t *lib_recent_get(int i);

/* Short system name for a sys_t ("NES", "Game Boy"). */
const char *sys_name(int sys);

/* Loads a ROM into memory, unzipping if needed. Caller frees. */
uint8_t *rom_load(const char *path, size_t *size);
