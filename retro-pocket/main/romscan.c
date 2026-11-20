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
#include "lock.h"
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
    s_games[sys][s_count[sys]++] = (game_t){ p, n, sys };
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

/* ---------------- favourites & recently played ---------------- */

#define MAX_FAVS    200
#define MAX_RECENT  20
#define FAV_FILE    DIR_SAVES "/favorites.txt"
#define RECENT_FILE DIR_SAVES "/recent.txt"

typedef struct {
    char (*paths)[320];
    int count, max;
    const char *file;
    const game_t **resolved; /* entries still on the card */
    int resolved_count;
} plist_t;

static plist_t s_favs = { .max = MAX_FAVS, .file = FAV_FILE };
static plist_t s_recent = { .max = MAX_RECENT, .file = RECENT_FILE };

const game_t *games_find(const char *path)
{
    for (int s = 1; s < SYS_COUNT; s++)
        for (int i = 0; i < s_count[s]; i++)
            if (!strcmp(s_games[s][i].path, path)) return &s_games[s][i];
    return NULL;
}

static bool plist_alloc(plist_t *l)
{
    if (!l->paths) {
        l->paths = heap_caps_malloc(sizeof(*l->paths) * l->max, MALLOC_CAP_SPIRAM);
        l->resolved = heap_caps_malloc(sizeof(game_t *) * l->max, MALLOC_CAP_SPIRAM);
    }
    return l->paths && l->resolved;
}

static void plist_resolve(plist_t *l)
{
    l->resolved_count = 0;
    for (int i = 0; i < l->count; i++) {
        const game_t *g = games_find(l->paths[i]);
        if (g) l->resolved[l->resolved_count++] = g;
    }
}

static void plist_load(plist_t *l)
{
    if (!plist_alloc(l)) return;
    l->count = 0;
    FILE *f = fopen(l->file, "r");
    if (f) {
        while (l->count < l->max && fgets(l->paths[l->count], sizeof(l->paths[0]), f)) {
            l->paths[l->count][strcspn(l->paths[l->count], "\r\n")] = 0;
            if (l->paths[l->count][0]) l->count++;
        }
        fclose(f);
    }
    plist_resolve(l);
}

static void plist_save(plist_t *l)
{
    FILE *f = fopen(l->file, "w");
    if (!f) return;
    for (int i = 0; i < l->count; i++) fprintf(f, "%s\n", l->paths[i]);
    fclose(f);
}

static int plist_index(plist_t *l, const char *path)
{
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->paths[i], path)) return i;
    return -1;
}

static void plist_remove(plist_t *l, int i)
{
    memmove(l->paths[i], l->paths[i + 1], sizeof(l->paths[0]) * (l->count - i - 1));
    l->count--;
}

static void lists_load(void)
{
    plist_load(&s_favs);
    plist_load(&s_recent);
}

bool lib_is_fav(const char *path)
{
    lib_lock();
    bool r = s_favs.paths && plist_index(&s_favs, path) >= 0;
    lib_unlock();
    return r;
}

void lib_set_fav(const char *path, bool fav)
{
    lib_lock();
    if (plist_alloc(&s_favs) && strlen(path) < sizeof(s_favs.paths[0])) {
        int i = plist_index(&s_favs, path);
        if (fav && i < 0 && s_favs.count < s_favs.max) strcpy(s_favs.paths[s_favs.count++], path);
        if (!fav && i >= 0) plist_remove(&s_favs, i);
        plist_save(&s_favs);
        plist_resolve(&s_favs);
    }
    lib_unlock();
}

void lib_add_recent(const char *path)
{
    lib_lock();
    if (plist_alloc(&s_recent) && strlen(path) < sizeof(s_recent.paths[0])) {
        int i = plist_index(&s_recent, path);
        if (i >= 0) plist_remove(&s_recent, i);
        if (s_recent.count == s_recent.max) s_recent.count--;
        memmove(s_recent.paths[1], s_recent.paths[0], sizeof(s_recent.paths[0]) * s_recent.count);
        strcpy(s_recent.paths[0], path);
        s_recent.count++;
        plist_save(&s_recent);
        plist_resolve(&s_recent);
    }
    lib_unlock();
}

int lib_fav_count(void) { return s_favs.resolved_count; }
int lib_recent_count(void) { return s_recent.resolved_count; }
const game_t *lib_fav_get(int i) { return i >= 0 && i < s_favs.resolved_count ? s_favs.resolved[i] : NULL; }
const game_t *lib_recent_get(int i) { return i >= 0 && i < s_recent.resolved_count ? s_recent.resolved[i] : NULL; }

const char *sys_name(int sys)
{
    switch (sys) {
        case SYS_NES: return "NES";
        case SYS_GB: return "Game Boy";
        default: return "?";
    }
}

void games_scan(scan_progress_fn progress)
{
    if (!s_pool) {
        s_pool = heap_caps_malloc(POOL_SIZE, MALLOC_CAP_SPIRAM);
        for (int s = 1; s < SYS_COUNT; s++)
            s_games[s] = heap_caps_malloc(MAX_GAMES * sizeof(game_t), MALLOC_CAP_SPIRAM);
        if (!s_pool) return;
    }
    lib_lock();
    s_pool_used = 0;
    memset(s_count, 0, sizeof(s_count));
    s_favs.resolved_count = s_recent.resolved_count = 0; /* they point into the old lists */
    lib_unlock();
    s_zc_count = s_zc_sorted = 0;
    s_zc_dirty = false;
    s_files_seen = s_found = 0;
    s_progress = progress;
    s_last_progress = 0;
    if (!sd_ok()) return;

    lib_lock();
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
    lists_load();
    lib_unlock();
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
