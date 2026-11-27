/* Runtime pieces of the furi compatibility layer + Sub-GHz keystore stub. */
#include <furi.h>
#include <furi_hal.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include "subghz_keystore.h"
#include "subghz_keystore_i.h"

void furi_shim_crash(const char* msg, const char* file, int line) {
    printf("FURI CRASH: %s at %s:%d\n", msg, file, line);
    abort();
}

struct FuriString {
    char* buf;
    size_t len, cap;
};

static void fs_reserve(FuriString* s, size_t need) {
    if(need + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 32;
    while(cap < need + 1) cap *= 2;
    s->buf = realloc(s->buf, cap);
    furi_check(s->buf);
    s->cap = cap;
}

FuriString* furi_string_alloc(void) {
    FuriString* s = calloc(1, sizeof(FuriString));
    furi_check(s);
    fs_reserve(s, 0);
    s->buf[0] = 0;
    return s;
}
FuriString* furi_string_alloc_set_str(const char* str) {
    FuriString* s = furi_string_alloc();
    furi_string_set_str(s, str);
    return s;
}
void furi_string_free(FuriString* s) {
    if(!s) return;
    free(s->buf);
    free(s);
}
void furi_string_reset(FuriString* s) {
    s->len = 0;
    s->buf[0] = 0;
}
const char* furi_string_get_cstr(const FuriString* s) {
    return s->buf;
}
size_t furi_string_size(const FuriString* s) {
    return s->len;
}
void furi_string_set_str(FuriString* s, const char* src) {
    size_t n = strlen(src);
    fs_reserve(s, n);
    memmove(s->buf, src, n + 1);
    s->len = n;
}
void furi_string_set_string(FuriString* s, const FuriString* src) {
    furi_string_set_str(s, src->buf);
}
void furi_string_set_n(FuriString* s, const FuriString* src, size_t start, size_t len) {
    if(start > src->len) start = src->len;
    if(len > src->len - start) len = src->len - start;
    char* tmp = malloc(len + 1);
    memcpy(tmp, src->buf + start, len);
    tmp[len] = 0;
    furi_string_set_str(s, tmp);
    free(tmp);
}
void furi_string_cat_str(FuriString* s, const char* src) {
    size_t n = strlen(src);
    fs_reserve(s, s->len + n);
    memcpy(s->buf + s->len, src, n + 1);
    s->len += n;
}
void furi_string_cat_string(FuriString* s, const FuriString* src) {
    furi_string_cat_str(s, src->buf);
}
static int fs_vcat(FuriString* s, const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if(n < 0) return n;
    fs_reserve(s, s->len + n);
    vsnprintf(s->buf + s->len, n + 1, fmt, ap);
    s->len += n;
    return n;
}
int furi_string_printf(FuriString* s, const char* fmt, ...) {
    furi_string_reset(s);
    va_list ap;
    va_start(ap, fmt);
    int r = fs_vcat(s, fmt, ap);
    va_end(ap);
    return r;
}
int furi_string_cat_printf(FuriString* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = fs_vcat(s, fmt, ap);
    va_end(ap);
    return r;
}
int furi_string_cmp_str(const FuriString* s, const char* other) {
    return strcmp(s->buf, other);
}

void* furi_record_open(const char* name) {
    (void)name;
    return NULL;
}
void furi_record_close(const char* name) {
    (void)name;
}
void furi_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
uint32_t furi_get_tick(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void furi_hal_rtc_get_datetime(DateTime* dt) {
    memset(dt, 0, sizeof(*dt));
    dt->year = 2000;
    dt->month = 1;
    dt->day = 1;
}
uint32_t datetime_datetime_to_timestamp(DateTime* dt) {
    (void)dt;
    return (uint32_t)(esp_timer_get_time() / 1000000);
}
int32_t furi_hal_subghz_get_rolling_counter_mult(void) {
    return 1;
}

/* ---- Keystore: no manufacturer key files on this board ----
 * KeeLoq-family remotes are still recognised and show their fixed part;
 * the encrypted hopping part just stays "Unknown" (same as a Flipper with
 * no keys loaded). */
SubGhzKeystore* subghz_keystore_alloc(void) {
    SubGhzKeystore* k = calloc(1, sizeof(SubGhzKeystore));
    SubGhzKeyArray_init(k->data);
    return k;
}
void subghz_keystore_free(SubGhzKeystore* k) {
    SubGhzKeyArray_clear(k->data);
    free(k);
}
bool subghz_keystore_load(SubGhzKeystore* k, const char* filename) {
    (void)k;
    (void)filename;
    return false;
}
bool subghz_keystore_save(SubGhzKeystore* k, const char* filename, uint8_t* iv) {
    (void)k;
    (void)filename;
    (void)iv;
    return false;
}
SubGhzKeyArray_t* subghz_keystore_get_data(SubGhzKeystore* k) {
    return &k->data;
}
bool subghz_keystore_raw_encrypted_save(const char* in, const char* out, uint8_t* iv) {
    (void)in;
    (void)out;
    (void)iv;
    return false;
}
bool subghz_keystore_raw_get_data(const char* file_name, size_t offset, uint8_t* data, size_t len) {
    (void)file_name;
    (void)offset;
    memset(data, 0, len);
    return false;
}
void subghz_keystore_reset_kl(SubGhzKeystore* k) {
    (void)k;
}
