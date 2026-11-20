/*
 * game_maze.c - "Maze Runner 3D", an original built-in raycaster game.
 *
 * Find the glowing green exit. Every level the maze gets bigger.
 * Stick up/down = walk, stick left/right = turn (analog), A = sprint,
 * B = strafe mode while held, SELECT = map, joystick click = pause.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "emulators.h"
#include "system.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define MAX_CELLS 31                 /* maze grid incl. walls, odd */
#define W LCD_WIDTH
#define H LCD_HEIGHT

static uint8_t map[MAX_CELLS][MAX_CELLS]; /* 0 empty, 1..3 wall styles, 9 exit */
static int msize;
static float px, py, ang;
static float zbuf[W];

static void gen_maze(int cells)
{
    msize = cells * 2 + 1;
    for (int y = 0; y < msize; y++)
        for (int x = 0; x < msize; x++) map[y][x] = 1 + (esp_random() % 3);

    /* iterative depth-first carve */
    static uint8_t stack[(MAX_CELLS / 2) * (MAX_CELLS / 2)][2];
    int sp = 0;
    stack[sp][0] = 0; stack[sp][1] = 0; sp++;
    map[1][1] = 0;
    while (sp) {
        int cx = stack[sp - 1][0], cy = stack[sp - 1][1];
        int dirs[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} }, n = 0, opts[4];
        for (int i = 0; i < 4; i++) {
            int nx = cx + dirs[i][0], ny = cy + dirs[i][1];
            if (nx >= 0 && ny >= 0 && nx < cells && ny < cells && map[ny * 2 + 1][nx * 2 + 1])
                opts[n++] = i;
        }
        if (!n) { sp--; continue; }
        int d = opts[esp_random() % n];
        int nx = cx + dirs[d][0], ny = cy + dirs[d][1];
        map[cy * 2 + 1 + dirs[d][1]][cx * 2 + 1 + dirs[d][0]] = 0;
        map[ny * 2 + 1][nx * 2 + 1] = 0;
        stack[sp][0] = nx; stack[sp][1] = ny; sp++;
    }
    /* knock out a few extra walls so it isn't a single corridor */
    for (int i = 0; i < cells; i++) {
        int x = 1 + esp_random() % (msize - 2), y = 1 + esp_random() % (msize - 2);
        if ((x + y) % 2 == 1) map[y][x] = 0;
    }
    map[msize - 2][msize - 1] = 9; /* exit door on the east wall, bottom right */
    px = 1.5f; py = 1.5f;
    ang = map[1][2] == 0 ? 0.0f : (float)M_PI / 2;
}

static inline uint16_t shade(int r, int g, int b, float k)
{
    if (k > 1) k = 1;
    return RGB((int)(r * k), (int)(g * k), (int)(b * k));
}

static void render(uint16_t *fb, float t)
{
    float dx = cosf(ang), dy = sinf(ang);
    float plx = -dy * 0.66f, ply = dx * 0.66f;

    /* sky / floor gradients */
    for (int y = 0; y < H / 2; y++) {
        uint16_t c = shade(40, 50, 110, 0.35f + 0.65f * (1.0f - (float)y / (H / 2)));
        for (int x = 0; x < W; x++) fb[y * W + x] = c;
    }
    for (int y = H / 2; y < H; y++) {
        uint16_t c = shade(90, 70, 50, 0.15f + 0.85f * (float)(y - H / 2) / (H / 2));
        for (int x = 0; x < W; x++) fb[y * W + x] = c;
    }

    for (int x = 0; x < W; x++) {
        float cam = 2.0f * x / W - 1.0f;
        float rx = dx + plx * cam, ry = dy + ply * cam;
        int mx = (int)px, my = (int)py;
        float ddx = fabsf(1.0f / rx), ddy = fabsf(1.0f / ry);
        int stx = rx < 0 ? -1 : 1, sty = ry < 0 ? -1 : 1;
        float sdx = (rx < 0 ? px - mx : mx + 1.0f - px) * ddx;
        float sdy = (ry < 0 ? py - my : my + 1.0f - py) * ddy;
        int side = 0, cell = 0;
        for (int i = 0; i < 64; i++) {
            if (sdx < sdy) { sdx += ddx; mx += stx; side = 0; }
            else           { sdy += ddy; my += sty; side = 1; }
            if (mx < 0 || my < 0 || mx >= msize || my >= msize) { cell = 1; break; }
            if ((cell = map[my][mx])) break;
        }
        float dist = side == 0 ? sdx - ddx : sdy - ddy;
        if (dist < 0.05f) dist = 0.05f;
        zbuf[x] = dist;
        int lh = (int)(H / dist);
        int y0 = H / 2 - lh / 2, y1 = H / 2 + lh / 2;

        /* where along the wall the ray hit -> simple procedural "texture" */
        float wx = side == 0 ? py + dist * ry : px + dist * rx;
        wx -= floorf(wx);
        int tx = (int)(wx * 16);

        float fog = 1.6f / (1.0f + dist * 0.35f);
        if (side) fog *= 0.75f;
        int r, g, b;
        switch (cell) {
            case 9:  r = 60;  g = 255; b = 120; fog *= 0.8f + 0.2f * sinf(t * 6); break;
            case 2:  r = 200; g = 120; b = 80;  break;  /* brick */
            case 3:  r = 120; g = 160; b = 200; break;  /* stone */
            default: r = 170; g = 170; b = 180; break;
        }
        int ys = y0 < 0 ? 0 : y0, ye = y1 > H ? H : y1;
        for (int y = ys; y < ye; y++) {
            int ty = (y - y0) * 16 / (lh ? lh : 1);
            float k = fog;
            if (cell == 2) { /* brick rows with offset joints */
                int row = ty / 4;
                if (ty % 4 == 0 || ((tx + (row & 1) * 8) % 16) == 0) k *= 0.55f;
            } else if (cell == 3) {
                if (((tx ^ ty) & 8)) k *= 0.85f;
            } else if (cell != 9) {
                if (tx == 0 || tx == 15) k *= 0.6f;
            }
            fb[y * W + x] = shade(r, g, b, k);
        }
    }
}

static void draw_map(uint16_t *fb)
{
    int s = (H - 8) / msize;
    if (s < 2) s = 2;
    int ox = (W - msize * s) / 2, oy = (H - msize * s) / 2;
    gfx_dim(fb);
    for (int y = 0; y < msize; y++)
        for (int x = 0; x < msize; x++)
            if (map[y][x])
                gfx_rect(fb, ox + x * s, oy + y * s, s, s, map[y][x] == 9 ? C_GREEN : C_DIM);
    gfx_rect(fb, ox + (int)(px * s) - 1, oy + (int)(py * s) - 1, 3, 3, C_ACCENT);
}

static bool solid(float x, float y)
{
    int mx = (int)x, my = (int)y;
    if (mx < 0 || my < 0 || mx >= msize || my >= msize) return true;
    return map[my][mx] != 0 && map[my][mx] != 9;
}

void game_maze_run(const char *unused)
{
    int level = 1;
    gen_maze(4);
    pacer_t pacer;
    pacer_start(&pacer);
    int64_t start = esp_timer_get_time(), last = start;
    char hud[32];

    for (;;) {
        uint32_t k = input_poll();
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        if (input_pressed() & BTN_MENU) {
            ingame_action_t a = ingame_menu("Maze Runner 3D", false);
            if (a == MENU_QUIT) return;
            if (a == MENU_RESET) { level = 1; gen_maze(4); start = esp_timer_get_time(); }
            last = esp_timer_get_time();
            pacer_start(&pacer);
            continue;
        }

        float jx, jy;
        input_joystick(&jx, &jy);
        if (fabsf(jx) < 0.12f) jx = 0;
        if (fabsf(jy) < 0.12f) jy = 0;
        float speed = (k & BTN_A ? 3.6f : 2.0f) * dt;
        float fwd = -jy * speed, strafe = 0;
        if (k & BTN_B) strafe = jx * speed;               /* B held: sidestep */
        else ang += jx * 2.6f * dt;                        /* turn */

        float dx = cosf(ang), dy = sinf(ang);
        float mvx = dx * fwd - dy * strafe, mvy = dy * fwd + dx * strafe;
        const float r = 0.22f; /* player radius */
        if (!solid(px + mvx + (mvx > 0 ? r : -r), py)) px += mvx;
        if (!solid(px, py + mvy + (mvy > 0 ? r : -r))) py += mvy;

        bool at_exit = false;
        for (int i = 0; i < 4; i++) {
            int cx = (int)(px + (i == 0 ? 0.3f : i == 1 ? -0.3f : 0));
            int cy = (int)(py + (i == 2 ? 0.3f : i == 3 ? -0.3f : 0));
            if (cx >= 0 && cy >= 0 && cx < msize && cy < msize && map[cy][cx] == 9) at_exit = true;
        }

        uint16_t *fb = display_fb();
        render(fb, (now - start) / 1e6f);
        if (k & BTN_SELECT) draw_map(fb);
        snprintf(hud, sizeof(hud), "LV %d  %ds", level, (int)((now - start) / 1000000));
        gfx_text(fb, W - (int)strlen(hud) * CHAR_W - 2, 2, hud, C_TEXT, 0);
        if (g_settings.show_fps) draw_fps(fb, pacer.fps);
        display_present();

        if (at_exit) {
            char l1[32], l2[32];
            snprintf(l1, sizeof(l1), "Level %d cleared!", level);
            snprintf(l2, sizeof(l2), "Time: %ds", (int)((now - start) / 1000000));
            message_box(l1, l2, 0);
            level++;
            int cells = 3 + level;
            if (cells > MAX_CELLS / 2) cells = MAX_CELLS / 2;
            gen_maze(cells);
            start = last = esp_timer_get_time();
            pacer_start(&pacer);
        }
        pacer_wait(&pacer, 16666);
    }
}
