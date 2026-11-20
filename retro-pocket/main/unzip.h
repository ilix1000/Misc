#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decompresses raw DEFLATE data (RFC 1951) into a buffer of known size.
 * Returns the number of bytes written, or -1 on corrupt input. */
long inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

uint32_t crc32_calc(const uint8_t *data, size_t len);

typedef struct {
    char name[128];      /* file name inside the zip */
    uint32_t comp_size, size, crc, local_offset;
    uint16_t method;     /* 0 = stored, 8 = deflate */
} zip_entry_t;

/* Finds the first file in the zip whose name ends with one of `exts`
 * (space separated, lowercase, e.g. ".nes .gb"). */
bool zip_find(const char *zip_path, const char *exts, zip_entry_t *out);

/* Extracts an entry into a newly allocated buffer (PSRAM when available).
 * Caller frees. Checks the CRC. */
uint8_t *zip_extract(const char *zip_path, const zip_entry_t *e);

/* true if `name` ends with one of the space separated extensions (any case). */
bool has_ext(const char *name, const char *exts);
