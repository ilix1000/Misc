#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { SYS_NONE = 0, SYS_NES, SYS_GB, SYS_COUNT } sys_t;

typedef struct {
    const char *path;   /* full path on the SD card (.nes/.gb/... or .zip) */
    const char *name;   /* display name: file name without extension */
} game_t;

/* Called during the scan so the UI can show progress. */
typedef void (*scan_progress_fn)(int files_seen, int games_found, const char *current);

/* Walks the whole SD card and sorts every game it finds into its system.
 * Zips are opened to see which system the ROM inside belongs to; that
 * answer is cached on the card so later boots are fast. */
void games_scan(scan_progress_fn progress);
int games_count(sys_t sys);
const game_t *games_get(sys_t sys, int index);

/* Loads a ROM into memory, unzipping if needed. Caller frees. */
uint8_t *rom_load(const char *path, size_t *size);
