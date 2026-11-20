/*
 * romscan.c - finds games anywhere on the SD card, zipped or not.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "romscan.h"
#include "unzip.h"
#include "system.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "scan";

#define MAX_GAMES   4000
#define POOL_SIZE   (1024 * 1024)   /* path + name strings */
#define MAX_DEPTH   6
#define CACHE_FILE  DIR_SAVES "/zipcache.txt"

static const char *const sys_exts[SYS_COUNT] = {
    [SYS_NES] = ".nes",
    [SYS_GB]  = ".gb .gbc .dmg",
};
#define ALL_ROM_EXTS ".nes .gb .gbc .dmg"

static game_t *s_games[SYS_COUNT];
static int s_count[SYS_COUNT];
static char *s_pool;
static size_t s_pool_used;

/* zip cache: what system the ROM inside each zip is, keyed by path+size+mtime */
typedef struct {
    const char *path;
    long size, mtime;
    uint8_t sys;
    bool used;
} zcache_t;
static zcache_t *s_zc;
static int s_zc_count, s_zc_cap, s_zc_sorted; /* [0, sorted) is sorted by path */
static bool s_zc_dirty;

static int s_files_seen, s_found;
static scan_progress_fn s_progress;
static int64_t s_last_progress;

static char *pool_dup(const char *s, size_t n)
{
    if (s_pool_used + n + 1 > POOL_SIZE) return NULL;
    char *p = s_pool + s_pool_used;
    memcpy(p, s, n);
    p[n] = 0;
    s_pool_used += n + 1;
    return p;
}

static sys_t sys_for_name(const char *name)
{
    for (int s = 1; s < SYS_COUNT; s++)
        if (has_ext(name, sys_exts[s])) return s;
    return SYS_NONE;
}

/* ---------------- zip cache ---------------- */

static int zc_cmp(const void *a, const void *b)
{
    return strcmp(((const zcache_t *)a)->path, ((const zcache_t *)b)->path);
}

static void zc_add(const char *path, long size, long mtime, uint8_t sys, bool used)
{
    if (s_zc_count == s_zc_cap) {
        int cap = s_zc_cap ? s_zc_cap * 2 : 256;
        zcache_t *n = realloc(s_zc, cap * sizeof(*n));
        if (!n) return;
        s_zc = n;
        s_zc_cap = cap;
    }
    char *p = pool_dup(path, strlen(path));
    if (!p) return;
    s_zc[s_zc_count++] = (zcache_t){ p, size, mtime, sys, used };
}

static void zc_load(void)
{
    FILE *f = fopen(CACHE_FILE, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        int sys, off = 0;
        long size, mtime;
        if (sscanf(line, "%d %ld %ld %n", &sys, &size, &mtime, &off) != 3 || !off) continue;
        line[strcspn(line, "\r\n")] = 0;
        zc_add(line + off, size, mtime, sys, false);
    }
    fclose(f);
    qsort(s_zc, s_zc_count, sizeof(*s_zc), zc_cmp);
    s_zc_sorted = s_zc_count;
}

static void zc_save(void)
{
    FILE *f = fopen(CACHE_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_zc_count; i++)
        if (s_zc[i].used) fprintf(f, "%d %ld %ld %s\n", s_zc[i].sys, s_zc[i].size, s_zc[i].mtime, s_zc[i].path);
    fclose(f);
}

static sys_t zip_system(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return SYS_NONE;
    zcache_t key = { .path = path };
    zcache_t *hit = s_zc_sorted ? bsearch(&key, s_zc, s_zc_sorted, sizeof(*s_zc), zc_cmp) : NULL;
    if (hit && hit->size == (long)st.st_size && hit->mtime == (long)st.st_mtime) {
        hit->used = true;
        return hit->sys;
    }
    zip_entry_t e;
    sys_t sys = zip_find(path, ALL_ROM_EXTS, &e) ? sys_for_name(e.name) : SYS_NONE;
    if (hit) {
        hit->size = st.st_size;
        hit->mtime = st.st_mtime;
        hit->sys = sys;
        hit->used = true;
    } else {
        /* appended unsorted; fine, we only look each path up once per scan */
        zc_add(path, st.st_size, st.st_mtime, sys, true);
    }
    s_zc_dirty = true;
    return sys;
}

/* ---------------- directory walk ---------------- */

static void add_game(sys_t sys, const char *path, const char *fname)
{
    if (s_count[sys] >= MAX_GAMES) return;
    const char *dot = strrchr(fname, '.');
    size_t nlen = dot ? (size_t)(dot - fname) : strlen(fname);
    const char *p = pool_dup(path, strlen(path));
    const char *n = pool_dup(fname, nlen);
    if (!p || !n) return;
    s_games[sys][s_count[sys]++] = (game_t){ p, n };
    s_found++;
}

static void walk(char *path, size_t len, int depth)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;   /* hidden files, macOS "._" junk */
        size_t nl = strlen(n);
        if (len + 1 + nl + 1 > 512) continue;
        path[len] = '/';
        memcpy(path + len + 1, n, nl + 1);

        if (e->d_type == DT_DIR) {
            bool skip = depth >= MAX_DEPTH || !strcasecmp(n, "System Volume Information") ||
                        (depth == 0 && !strcasecmp(n, "saves"));
            if (!skip) walk(path, len + 1 + nl, depth + 1);
        } else {
            s_files_seen++;
            sys_t sys = has_ext(n, ".zip") ? zip_system(path) : sys_for_name(n);
            if (sys != SYS_NONE) add_game(sys, path, n);
        }
        int64_t now = esp_timer_get_time();
        if (s_progress && now - s_last_progress > 100000) {
            s_last_progress = now;
            s_progress(s_files_seen, s_found, n);
        }
    }
    path[len] = 0;
    closedir(d);
}

/* Case-insensitive "natural" order: "Game 2" sorts before "Game 10". */
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)(ea - a) - (int)(eb - b);
            int c = strncmp(a, b, ea - a);
            if (c) return c;
            a = ea;
            b = eb;
            continue;
        }
        int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (c) return c;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int game_cmp(const void *a, const void *b)
{
    const game_t *x = a, *y = b;
    int c = natcmp(x->name, y->name);
    return c ? c : strcmp(x->path, y->path);
}

void games_scan(scan_progress_fn progress)
{
    if (!s_pool) {
        s_pool = heap_caps_malloc(POOL_SIZE, MALLOC_CAP_SPIRAM);
        for (int s = 1; s < SYS_COUNT; s++)
            s_games[s] = heap_caps_malloc(MAX_GAMES * sizeof(game_t), MALLOC_CAP_SPIRAM);
        if (!s_pool) return;
    }
    s_pool_used = 0;
    memset(s_count, 0, sizeof(s_count));
    s_zc_count = s_zc_sorted = 0;
    s_zc_dirty = false;
    s_files_seen = s_found = 0;
    s_progress = progress;
    s_last_progress = 0;
    if (!sd_ok()) return;

    int64_t t0 = esp_timer_get_time();
    zc_load();
    int cached = s_zc_count;
    static char path[512];
    strcpy(path, SD_MOUNT);
    walk(path, strlen(path), 0);
    for (int s = 1; s < SYS_COUNT; s++)
        if (s_games[s]) qsort(s_games[s], s_count[s], sizeof(game_t), game_cmp);
    /* drop cache rows for zips that are gone */
    for (int i = 0; i < cached && !s_zc_dirty; i++)
        if (!s_zc[i].used) s_zc_dirty = true;
    if (s_zc_dirty) zc_save();
    ESP_LOGI(TAG, "%d files, %d NES, %d GB in %d ms", s_files_seen, s_count[SYS_NES],
             s_count[SYS_GB], (int)((esp_timer_get_time() - t0) / 1000));
}

int games_count(sys_t sys)
{
    return sys > SYS_NONE && sys < SYS_COUNT ? s_count[sys] : 0;
}

const game_t *games_get(sys_t sys, int i)
{
    return i >= 0 && i < games_count(sys) ? &s_games[sys][i] : NULL;
}

uint8_t *rom_load(const char *path, size_t *size)
{
    if (!has_ext(path, ".zip")) return file_load(path, size);
    zip_entry_t e;
    if (!zip_find(path, ALL_ROM_EXTS, &e)) return NULL;
    uint8_t *data = zip_extract(path, &e);
    if (data && size) *size = e.size;
    return data;
}
