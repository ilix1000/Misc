/*
 * rg_system.h - the two things the Retro-Go cores (smsplus, pce-go) need from
 * Retro-Go's system layer, so they build in Retro Pocket unchanged.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define RG_LOG_PRINTF 0
#define rg_system_log(level, ctx, ...) printf(__VA_ARGS__)

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
