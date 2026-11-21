/*
 * rg_system.h - the two things the Retro-Go cores (smsplus, pce-go) need from
 * Retro-Go's system layer, so they build in Retro Pocket unchanged.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#define RG_LOG_PRINTF 0
#define rg_system_log(level, ctx, ...) printf(__VA_ARGS__)
#define rg_system_vlog(level, ctx, fmt, ap) vprintf(fmt, ap)
#include <stdlib.h>
#define RG_PANIC(x) do { printf("PANIC: %s\n", (x)); abort(); } while (0)

/* true if path ends in "." + ext (any case) */
static inline bool rg_extension_match(const char *path, const char *ext)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot && strcasecmp(dot + 1, ext) == 0;
}

/* Implemented by the firmware (emu_doom.c) with its own unzipper: extracts the
 * first file of the zip into a new buffer. */
bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data, size_t *size, uint32_t flags);

/* Standard CRC-32 (zip/PNG), continuing from `crc` like the ESP ROM's
 * crc32_le. Used by the cores to look up per-game fixes. */
static inline uint32_t rg_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}
