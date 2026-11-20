#pragma once
/*
 * bridge.h - how the web UI (and Wi-Fi events) talk to the app task.
 * The web server runs on core 0; everything that touches the screen or the
 * running game happens on the app task (core 1). Requests are parked here and
 * picked up by the launcher / emulators each frame.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Buttons held on the web remote control (BTN_* bits), merged into input. */
void bridge_set_remote_keys(uint32_t keys);
uint32_t bridge_remote_keys(void);

/* Ask the app to start a game (quits the running one first). */
void bridge_request_launch(const char *path);
bool bridge_take_launch(char *out, size_t n);

/* Running games poll this each frame and exit when it's true. */
void bridge_request_quit(void);
bool bridge_quit_requested(void);
void bridge_clear_quit(void);

/* Misc one-shot flags the launcher handles. */
enum { REQ_RESCAN = 1, REQ_APPLY_SETTINGS = 2, REQ_OTA_ONLINE = 4 };
void bridge_request(uint32_t flags);
uint32_t bridge_take_requests(void);

/* Pop-up messages shown by the UI ("Wi-Fi connected" etc). */
void bridge_toast(const char *msg);
bool bridge_take_toast(char *out, size_t n);

/* What's running right now (shown in the web UI). Empty = in the menu. */
void bridge_set_now_playing(const char *name);
void bridge_get_now_playing(char *out, size_t n);
