/* Minimal Flipper "furi" compatibility layer: just enough of the Flipper OS
 * API for the Sub-GHz protocol decoders to compile and run on plain ESP-IDF. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <esp_log.h>

/* The Flipper's malloc() always returns zeroed memory and several decoders
 * rely on it (e.g. Honeywell never initialises its Manchester state). */
#define malloc(size) calloc(1, (size))

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif
#ifndef COUNT_OF
#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(x, upper, lower) (MIN(upper, MAX(x, lower)))
#endif
#define bit_read(value, bit)  (((value) >> (bit)) & 0x01)
#define bit_set(value, bit)   ((value) |= (1UL << (bit)))
#define bit_clear(value, bit) ((value) &= ~(1UL << (bit)))
#define bit_write(value, bit, bitvalue) (bitvalue ? bit_set(value, bit) : bit_clear(value, bit))
#ifndef FURI_PACKED
#define FURI_PACKED __attribute__((packed))
#endif
#define FuriWaitForever 0xFFFFFFFFU

void furi_shim_crash(const char* msg, const char* file, int line) __attribute__((noreturn));
#define furi_crash(...) furi_shim_crash("furi_crash", __FILE__, __LINE__)
#define furi_check(x, ...) \
    do {                   \
        if(!(x)) furi_shim_crash("check failed: " #x, __FILE__, __LINE__); \
    } while(0)
#define furi_assert(x, ...) furi_check(x)

/* Decoder logging is noisy; keep only errors/warnings at debug build. */
#define FURI_LOG_E(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_W(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_I(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_D(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_T(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)
#define FURI_LOG_RAW_D(fmt, ...)  do { } while(0)

/* ---- FuriString ---- */
typedef struct FuriString FuriString;
FuriString* furi_string_alloc(void);
FuriString* furi_string_alloc_set_str(const char* s);
void furi_string_free(FuriString* s);
void furi_string_reset(FuriString* s);
const char* furi_string_get_cstr(const FuriString* s);
size_t furi_string_size(const FuriString* s);
void furi_string_set_string(FuriString* s, const FuriString* src);
void furi_string_set_str(FuriString* s, const char* src);
void furi_string_set_n(FuriString* s, const FuriString* src, size_t start, size_t len);
void furi_string_cat_string(FuriString* s, const FuriString* src);
void furi_string_cat_str(FuriString* s, const char* src);
int furi_string_printf(FuriString* s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int furi_string_cat_printf(FuriString* s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int furi_string_cmp_str(const FuriString* s, const char* other);

/* Like the Flipper API: accept either a FuriString* or a C string */
#define furi_string_set(s, src) \
    _Generic((src), char*: furi_string_set_str, const char*: furi_string_set_str, default: furi_string_set_string)(s, src)
#define furi_string_cat(s, src) \
    _Generic((src), char*: furi_string_cat_str, const char*: furi_string_cat_str, default: furi_string_cat_string)(s, src)

/* ---- records / delays (not used for decoding) ---- */
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
void furi_delay_ms(uint32_t ms);
uint32_t furi_get_tick(void);

#ifdef __cplusplus
}
#endif
