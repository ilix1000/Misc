#include <string.h>
#include "bridge.h"
#include "lock.h"
#include "esp_timer.h"

static volatile uint32_t s_keys;
static volatile int64_t s_keys_at;
static volatile bool s_quit;
static volatile uint32_t s_reqs;
static char s_launch[320];
static char s_toast[4][64];
static int s_toast_head, s_toast_count;
static char s_now[96];

void bridge_set_remote_keys(uint32_t k) { s_keys = k; s_keys_at = esp_timer_get_time(); }
/* The web page re-sends its buttons several times a second; if it goes quiet
 * (tab closed, phone locked) we let go of everything instead of sticking. */
uint32_t bridge_remote_keys(void)
{
    return s_keys && esp_timer_get_time() - s_keys_at < 1000000 ? s_keys : 0;
}

void bridge_request_launch(const char *path)
{
    lib_lock();
    strncpy(s_launch, path, sizeof(s_launch) - 1);
    s_launch[sizeof(s_launch) - 1] = 0;
    s_quit = true; /* leave whatever is running */
    lib_unlock();
}

bool bridge_take_launch(char *out, size_t n)
{
    lib_lock();
    bool ok = s_launch[0] != 0;
    if (ok) {
        strncpy(out, s_launch, n - 1);
        out[n - 1] = 0;
        s_launch[0] = 0;
    }
    lib_unlock();
    return ok;
}

void bridge_request_quit(void) { s_quit = true; }
bool bridge_quit_requested(void) { return s_quit; }
void bridge_clear_quit(void) { s_quit = false; }

void bridge_request(uint32_t f)
{
    lib_lock();
    s_reqs |= f;
    lib_unlock();
}

uint32_t bridge_take_requests(void)
{
    lib_lock();
    uint32_t r = s_reqs;
    s_reqs = 0;
    lib_unlock();
    return r;
}

void bridge_toast(const char *msg)
{
    lib_lock();
    if (s_toast_count == 4) { s_toast_head = (s_toast_head + 1) % 4; s_toast_count--; }
    int i = (s_toast_head + s_toast_count) % 4;
    strncpy(s_toast[i], msg, sizeof(s_toast[i]) - 1);
    s_toast[i][sizeof(s_toast[i]) - 1] = 0;
    s_toast_count++;
    lib_unlock();
}

bool bridge_take_toast(char *out, size_t n)
{
    lib_lock();
    bool ok = s_toast_count > 0;
    if (ok) {
        strncpy(out, s_toast[s_toast_head], n - 1);
        out[n - 1] = 0;
        s_toast_head = (s_toast_head + 1) % 4;
        s_toast_count--;
    }
    lib_unlock();
    return ok;
}

void bridge_set_now_playing(const char *name)
{
    lib_lock();
    strncpy(s_now, name ? name : "", sizeof(s_now) - 1);
    s_now[sizeof(s_now) - 1] = 0;
    lib_unlock();
}

void bridge_get_now_playing(char *out, size_t n)
{
    lib_lock();
    strncpy(out, s_now, n - 1);
    out[n - 1] = 0;
    lib_unlock();
}

/* ---- the lock itself ---- */
#ifdef SIM
static pthread_mutex_t s_mtx;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static void mtx_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_mtx, &a);
}
void lib_lock_init(void) { pthread_once(&s_once, mtx_init); }
void lib_lock(void) { pthread_once(&s_once, mtx_init); pthread_mutex_lock(&s_mtx); }
void lib_unlock(void) { pthread_mutex_unlock(&s_mtx); }
#else
static SemaphoreHandle_t s_mtx;
void lib_lock_init(void) { if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex(); }
void lib_lock(void) { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void lib_unlock(void) { xSemaphoreGiveRecursive(s_mtx); }
#endif
