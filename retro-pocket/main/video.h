#pragma once
/*
 * video.h - shared output path for emulators that draw 8-bit palette indices
 * (Master System, Game Gear, PC Engine): scaling to the panel in landscape or
 * portrait, and showing frames from core 0 while core 1 emulates.
 */
#include <stdbool.h>
#include <stdint.h>

/* Size of the picture that will be submitted (the source is scaled to the panel). */
void video_setup(int src_w, int src_h);

/* Hands a finished frame to core 0, which scales it and sends it to the
 * panel. palette: 256 colours, RGB565 big-endian (panel byte order).
 * Returns false (frame dropped) if core 0 is still busy with the last one;
 * the caller must not touch `src` until the next successful call. */
bool video_submit(const uint8_t *src, int pitch, const uint16_t *palette);

/* true while core 0 is still showing the last frame. */
bool video_busy(void);

/* Waits until core 0 is idle (before menus or exiting). */
void video_wait(void);

/* Frames shown in the last second. */
int video_fps(void);
