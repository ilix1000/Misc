#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Where online updates come from. Defaults point at the project's GitHub repo;
 * change them (and add a token for a private repo) in the web UI. */
typedef struct {
    char repo[64];     /* "owner/name" */
    char branch[64];
    char path[96];     /* folder holding version.txt and retro-pocket.bin */
    char token[100];   /* GitHub token, only needed for private repos */
} ota_source_t;

void ota_source_get(ota_source_t *out);
void ota_source_set(const ota_source_t *src);

/* Call once the new firmware has booted fine, or the bootloader rolls back. */
void ota_mark_valid(void);

typedef enum { OTA_IDLE, OTA_CHECKING, OTA_DOWNLOADING, OTA_DONE, OTA_FAILED } ota_phase_t;

typedef struct {
    ota_phase_t phase;
    int percent;           /* while downloading */
    char latest[24];       /* version found online ("" if unknown) */
    char message[64];      /* human readable status / error */
} ota_status_t;

void ota_status(ota_status_t *out);

/* Online: check for / install the newest release in the background.
 * install=false only checks. Progress via ota_status(). */
void ota_online_start(bool install);
/* 1 = newer version available, 0 = up to date, -1 = unknown/error. */
int ota_update_available(void);

/* Upload: called by the web server with the .bin streamed in chunks. */
bool ota_upload_begin(size_t size);
bool ota_upload_write(const void *data, size_t len);
bool ota_upload_end(void);   /* on success: reboot to apply */
void ota_upload_abort(void);

/* Restarts in ~1 s (lets HTTP replies go out). */
void ota_reboot_soon(void);
