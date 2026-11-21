/*
 * game_city.c - "Pocket City", an original built-in open-world driving game.
 *
 * A procedurally built city with traffic, pedestrians, police chases and
 * three kinds of jobs (taxi, race, delivery). Drawn with a tiny flat-shaded
 * 3D renderer: convex polygons, near-plane clipping, painter's sort, fog.
 *
 * Controls
 *   On foot: stick = walk / turn, B = run, A = get into the nearest car
 *   In a car: stick up = gas, down = brake/reverse, left/right = steer,
 *             B = handbrake (drift), A = get out (when slow)
 *   SELECT = camera (behind / first person), START = city map,
 *   joystick click = pause menu
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "emulators.h"
#include "i18n.h"
#include "system.h"
#include "board.h"
#include "bridge.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define W LCD_WIDTH
#define H LCD_HEIGHT
#define PI_F 3.14159265f

/* ---------------- world layout ---------------- */

#define N       12                    /* blocks per side */
#define B       40.0f                 /* block pitch (road + lot) */
#define R       12.0f                 /* road width */
#define INSET   3.0f                  /* sidewalk around buildings */
#define WORLD   (N * B + R)
#define DRAW_DIST 150.0f
#define FOG0    45.0f
#define FOG1    145.0f
#define NEAR    0.15f
#define LANE    2.6f                  /* lane centre from road centre */

static uint8_t lot_h[N][N];           /* building height, 0 = park */
static uint8_t lot_c[N][N];           /* palette index */
static const uint8_t pal[][3] = {
    {190, 182, 165}, {172, 98, 82}, {122, 136, 152}, {212, 200, 168},
    {98, 104, 116}, {182, 160, 122}, {150, 170, 160}, {200, 150, 120},
};
#define STATION_I 2
#define STATION_J 2

static inline float road_c(int k) { return k * B + R * 0.5f; }

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

static float frand(void) { return (esp_random() & 0xFFFFFF) / (float)0x1000000; }

static void build_city(void)
{
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            uint32_t h = hash3(i, j, 7);
            float dc = hypotf(i - (N - 1) * 0.5f, j - (N - 1) * 0.5f);
            int ht = 8 + h % 10;
            if (dc < 2.5f) ht += 18 + (h >> 8) % 22;       /* downtown towers */
            else if (dc < 4.5f) ht += (h >> 8) % 14;
            bool park = (h >> 16) % 100 < (dc < 2.5f ? 4 : 13);
            lot_h[i][j] = park ? 0 : ht;
            lot_c[i][j] = (h >> 20) % (sizeof(pal) / 3);
        }
    lot_h[STATION_I][STATION_J] = 11;
    lot_c[STATION_I][STATION_J] = 2;
}

/* true inside a building (with margin r) or outside the city */
static bool solid(float x, float z, float r)
{
    if (x < r || z < r || x > WORLD - r || z > WORLD - r) return true;
    int i = (int)(x / B), j = (int)(z / B);
    if (i >= N || j >= N) return false;
    if (!lot_h[i][j]) return false;
    float lx = x - i * B, lz = z - j * B;
    return lx > R + INSET - r && lx < B - INSET + r && lz > R + INSET - r && lz < B - INSET + r;
}

static int tile_kind(float x, float z) /* 0 road, 1 sidewalk, 2 building, 3 park, -1 outside */
{
    if (x < 0 || z < 0 || x >= WORLD || z >= WORLD) return -1;
    int i = (int)(x / B), j = (int)(z / B);
    float lx = x - i * B, lz = z - j * B;
    if (lx < R || lz < R || i >= N || j >= N) return 0;
    if (!lot_h[i][j]) return 3;
    if (lx > R + INSET && lx < B - INSET && lz > R + INSET && lz < B - INSET) return 2;
    return 1;
}

/* ---------------- entities ---------------- */

enum { K_NONE, K_TRAFFIC, K_POLICE, K_PARKED, K_PLAYER, K_WRECK };
enum { M_COMPACT, M_SPORTS, M_TRUCK, M_POLICE, M_TAXI };

typedef struct {
    float x, z, a, vx, vz;
    float dmg, stop, stuck, rev, calm;
    float wpx, wpz;
    uint8_t kind, model, r, g, b;
    int8_t dir, lane, nk;
} car_t;

typedef struct {
    int8_t i, j;
    float s, spd, down;
    uint8_t r, g, b;
} ped_t;

#define MAX_CARS 48
#define N_TRAFFIC 16
#define N_PARKED 22
#define N_PEDS 26
static car_t cars[MAX_CARS];
static ped_t peds[N_PEDS];

static const uint8_t car_cols[][3] = {
    {200, 40, 40}, {40, 90, 200}, {230, 230, 235}, {40, 40, 48}, {60, 160, 80},
    {230, 140, 30}, {150, 60, 160}, {160, 165, 175}, {120, 30, 30}, {30, 140, 170},
};

/* player */
static bool on_foot;
static int pcar = -1;
static float px, pz, pa;        /* on-foot position / heading */
static int cam_mode;            /* 0 behind, 1 first person */
static float cam_yaw;

/* progress */
typedef struct { uint32_t magic; int cash; int jobs; } city_save_t;
#define SAVE_MAGIC 0x43495459
static int cash, jobs_done;
static int stars;
static float seen_t, bust_t, star_flash;

/* messages */
static char msg_big[24], msg_small[48];
static float msg_t;

static void say(const char *big, const char *small, float t)
{
    snprintf(msg_big, sizeof(msg_big), "%s", big ? big : "");
    snprintf(msg_small, sizeof(msg_small), "%s", small ? small : "");
    msg_t = t;
}

/* ---------------- missions ---------------- */

enum { MS_NONE, MS_TAXI, MS_RACE, MS_DELIVER };
static int mtype, mcp, mcount, mcar = -1;
static float mx[8], mz[8], mtime;
static float start_x[3], start_z[3];
static float mission_cool;

/* ---------------- camera & rasterizer ---------------- */

typedef struct { float x, y, z; } v3;
static float cam_x, cam_y, cam_z, cyaw_c, cyaw_s, cp_c, cp_s, focal, half_w, half_h;
static uint16_t *s_fb;
static float s_light, s_night;
static uint8_t fog_r, fog_g, fog_b;

static inline v3 to_cam(float x, float y, float z)
{
    float dx = x - cam_x, dy = y - cam_y, dz = z - cam_z;
    float rx = -dx * cyaw_s + dz * cyaw_c;
    float fz = dx * cyaw_c + dz * cyaw_s;
    v3 o = { rx, dy * cp_c + fz * cp_s, fz * cp_c - dy * cp_s };
    return o;
}

static void fill_convex(const float *xs, const float *ys, int n, uint16_t col)
{
    float miny = ys[0], maxy = ys[0], minx = xs[0], maxx = xs[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
    }
    if (maxx < 0 || minx > W || maxy < 0 || miny > H) return;
    int y0 = (int)ceilf(miny - 0.5f), y1 = (int)ceilf(maxy - 0.5f) - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > H - 1) y1 = H - 1;
    float slope[12];
    for (int i = 0; i < n; i++) {
        int k = i + 1 == n ? 0 : i + 1;
        float dy = ys[k] - ys[i];
        slope[i] = dy != 0 ? (xs[k] - xs[i]) / dy : 0;
    }
    for (int y = y0; y <= y1; y++) {
        float sy = y + 0.5f, xl = 1e9f, xr = -1e9f;
        for (int i = 0; i < n; i++) {
            int k = i + 1 == n ? 0 : i + 1;
            float ya = ys[i], yb = ys[k];
            if ((sy >= ya && sy < yb) || (sy >= yb && sy < ya)) {
                float x = xs[i] + (sy - ya) * slope[i];
                if (x < xl) xl = x;
                if (x > xr) xr = x;
            }
        }
        if (xl > xr) continue;
        int a = (int)ceilf(xl - 0.5f), b = (int)ceilf(xr - 0.5f);
        if (a < 0) a = 0;
        if (b > W) b = W;
        uint16_t *row = s_fb + y * W;
        for (int x = a; x < b; x++) row[x] = col;
    }
}

static void draw_poly(const v3 *in, int n, uint16_t col)
{
    v3 c[12];
    int m = 0;
    for (int i = 0; i < n && m < 11; i++) {
        v3 a = in[i], b = in[i + 1 == n ? 0 : i + 1];
        bool ain = a.z >= NEAR, bin = b.z >= NEAR;
        if (ain) c[m++] = a;
        if (ain != bin) {
            float t = (NEAR - a.z) / (b.z - a.z);
            v3 p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR };
            c[m++] = p;
        }
    }
    if (m < 3) return;
    float xs[12], ys[12];
    for (int i = 0; i < m; i++) {
        float iz = focal / c[i].z;
        xs[i] = half_w + c[i].x * iz;
        ys[i] = half_h - c[i].y * iz;
    }
    fill_convex(xs, ys, m, col);
}

static inline uint16_t shade(int r, int g, int b, float k, float fog)
{
    if (k > 1.2f) k = 1.2f;
    float a = 1 - fog;
    int rr = (int)(r * k * a + fog_r * fog), gg = (int)(g * k * a + fog_g * fog), bb = (int)(b * k * a + fog_b * fog);
    if (rr > 255) rr = 255;
    if (gg > 255) gg = 255;
    if (bb > 255) bb = 255;
    return RGB(rr, gg, bb);
}

static inline float fog_at(float d)
{
    float f = (d - FOG0) / (FOG1 - FOG0);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* Oriented box: centre (x,z), half sizes along heading (hl) and across (hw),
 * from y0 to y1. Draws the visible sides and the top. */
static void draw_box(float x, float z, float a, float hl, float hw, float y0, float y1,
                     int r, int g, int b, float fog, float emissive)
{
    float ca = cosf(a), sa = sinf(a);
    float fx = ca * hl, fz = sa * hl, rx = -sa * hw, rz = ca * hw;
    float bx[4] = { x + fx + rx, x + fx - rx, x - fx - rx, x - fx + rx };
    float bz[4] = { z + fz + rz, z + fz - rz, z - fz - rz, z - fz + rz };
    v3 lo[4], hi[4];
    for (int i = 0; i < 4; i++) { lo[i] = to_cam(bx[i], y0, bz[i]); hi[i] = to_cam(bx[i], y1, bz[i]); }
    /* side normals: 0-1 front(+f), 1-2 right(-r), 2-3 back, 3-0 left(+r) */
    float nx[4] = { ca, sa, -ca, -sa }, nz[4] = { sa, -ca, -sa, ca };
    for (int i = 0; i < 4; i++) {
        int k = (i + 1) & 3;
        float mx_ = (bx[i] + bx[k]) * 0.5f, mz_ = (bz[i] + bz[k]) * 0.5f;
        if ((cam_x - mx_) * nx[i] + (cam_z - mz_) * nz[i] <= 0) continue;
        float l = emissive > 0 ? emissive : s_light * (0.64f + 0.3f * (nx[i] * 0.6f + nz[i] * 0.8f));
        v3 q[4] = { lo[i], lo[k], hi[k], hi[i] };
        draw_poly(q, 4, shade(r, g, b, l, fog));
    }
    if (cam_y > y1) {
        v3 q[4] = { hi[0], hi[1], hi[2], hi[3] };
        draw_poly(q, 4, shade(r, g, b, emissive > 0 ? emissive : s_light * 1.02f, fog));
    }
}

static void ground_quad(float x0, float z0, float x1, float z1, float y, uint16_t col)
{
    v3 q[4] = { to_cam(x0, y, z0), to_cam(x1, y, z0), to_cam(x1, y, z1), to_cam(x0, y, z1) };
    draw_poly(q, 4, col);
}

/* ---------------- drawable models ---------------- */

static void draw_building(int i, int j, float fog, float dist)
{
    float x0 = i * B + R + INSET, x1 = (i + 1) * B - INSET;
    float z0 = j * B + R + INSET, z1 = (j + 1) * B - INSET;
    float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f, hs = (x1 - x0) * 0.5f;
    float h = lot_h[i][j];
    const uint8_t *c = pal[lot_c[i][j]];
    draw_box(cx, cz, 0, hs, hs, 0, h, c[0], c[1], c[2], fog, 0);
    if (i == STATION_I && j == STATION_J) /* police sign stripe */
        draw_box(cx, cz, 0, hs + 0.05f, hs + 0.05f, h - 2.2f, h - 1.2f, 40, 70, 200, fog, 0);
    if (dist > 85) return;
    /* window bands, one per floor on each visible wall */
    static const float nxs[4] = { 1, 0, -1, 0 }, nzs[4] = { 0, 1, 0, -1 };
    for (int f = 0; f < 4; f++) {
        float fx = cx + nxs[f] * (hs + 0.06f), fz = cz + nzs[f] * (hs + 0.06f);
        if ((cam_x - fx) * nxs[f] + (cam_z - fz) * nzs[f] <= 0) continue;
        float tx = -nzs[f], tz = nxs[f], L = hs - 1.4f;
        float k = s_light * (0.64f + 0.3f * (nxs[f] * 0.6f + nzs[f] * 0.8f));
        int floor_n = 0;
        for (float y = 1.6f; y + 1.7f < h - 0.8f; y += 3.6f, floor_n++) {
            uint16_t col;
            if (s_night > 0.5f && hash3(i * N + j, f, floor_n) % 3)
                col = shade(240, 205, 120, 1.0f, fog);
            else
                col = shade(60 + c[0] / 5, 80 + c[1] / 6, 110 + c[2] / 6, k * 0.9f, fog);
            v3 q[4] = { to_cam(fx + tx * L, y, fz + tz * L), to_cam(fx - tx * L, y, fz - tz * L),
                        to_cam(fx - tx * L, y + 1.7f, fz - tz * L), to_cam(fx + tx * L, y + 1.7f, fz + tz * L) };
            draw_poly(q, 4, col);
        }
    }
}

static void draw_tree(float x, float z, float fog)
{
    draw_box(x, z, 0, 0.3f, 0.3f, 0, 2.2f, 110, 75, 45, fog, 0);
    draw_box(x, z, 0.6f, 1.5f, 1.5f, 2.0f, 5.0f, 50, 130, 55, fog, 0);
}

static void draw_car(const car_t *c, float fog, float t)
{
    float ca = cosf(c->a), sa = sinf(c->a);
    int r = c->r, g = c->g, b = c->b;
    if (c->kind == K_WRECK) { r = 45; g = 42; b = 40; }
    /* shadow */
    {
        float hl = c->model == M_TRUCK ? 3.3f : 2.3f, hw = 1.2f;
        float fx = ca * hl, fz = sa * hl, rx = -sa * hw, rz = ca * hw;
        v3 q[4] = { to_cam(c->x + fx + rx, 0.03f, c->z + fz + rz), to_cam(c->x + fx - rx, 0.03f, c->z + fz - rz),
                    to_cam(c->x - fx - rx, 0.03f, c->z - fz - rz), to_cam(c->x - fx + rx, 0.03f, c->z - fz + rz) };
        draw_poly(q, 4, shade(25, 25, 28, s_light, fog));
    }
    draw_box(c->x, c->z, c->a, 1.7f, 0.85f, 0.0f, 0.35f, 20, 20, 22, fog, 0); /* wheels/chassis */
    switch (c->model) {
        case M_TRUCK:
            draw_box(c->x + ca * 2.2f, c->z + sa * 2.2f, c->a, 0.9f, 1.05f, 0.3f, 2.2f, r, g, b, fog, 0);
            draw_box(c->x + ca * 2.75f, c->z + sa * 2.75f, c->a, 0.4f, 0.95f, 1.2f, 2.0f, 60, 75, 100, fog, 0);
            draw_box(c->x - ca * 0.8f, c->z - sa * 0.8f, c->a, 2.2f, 1.15f, 0.4f, 3.0f, 225, 225, 220, fog, 0);
            break;
        case M_SPORTS:
            draw_box(c->x, c->z, c->a, 2.2f, 1.0f, 0.25f, 0.85f, r, g, b, fog, 0);
            draw_box(c->x - ca * 0.3f, c->z - sa * 0.3f, c->a, 0.95f, 0.85f, 0.85f, 1.3f, 50, 60, 80, fog, 0);
            break;
        default:
            draw_box(c->x, c->z, c->a, 2.0f, 0.95f, 0.3f, 1.05f, r, g, b, fog, 0);
            draw_box(c->x - ca * 0.2f, c->z - sa * 0.2f, c->a, 1.1f, 0.88f, 1.05f, 1.7f, 55, 65, 85, fog, 0);
            if (c->model == M_POLICE) {
                bool f = fmodf(t * 4, 1) < 0.5f;
                draw_box(c->x - ca * 0.2f + -sa * 0.4f, c->z - sa * 0.2f + ca * 0.4f, c->a, 0.25f, 0.35f, 1.7f, 1.95f,
                         f ? 255 : 60, 30, f ? 40 : 60, fog, 1.0f);
                draw_box(c->x - ca * 0.2f - -sa * 0.4f, c->z - sa * 0.2f - ca * 0.4f, c->a, 0.25f, 0.35f, 1.7f, 1.95f,
                         40, f ? 60 : 90, f ? 90 : 255, fog, 1.0f);
            } else if (c->model == M_TAXI) {
                draw_box(c->x - ca * 0.2f, c->z - sa * 0.2f, c->a, 0.3f, 0.45f, 1.7f, 2.0f, 250, 240, 160, fog, 0);
            }
            break;
    }
    if (c->kind != K_WRECK && c->kind != K_PARKED) { /* headlights */
        float hl = c->model == M_TRUCK ? 3.15f : c->model == M_SPORTS ? 2.2f : 2.0f;
        float e = s_night > 0.4f ? 1.25f : 0;
        for (int s = -1; s <= 1; s += 2)
            draw_box(c->x + ca * hl - sa * 0.6f * s, c->z + sa * hl + ca * 0.6f * s, c->a, 0.06f, 0.2f, 0.55f, 0.8f,
                     250, 245, 200, fog, e);
    }
}

static void ped_pos(const ped_t *p, float *x, float *z, float *a)
{
    float x0 = p->i * B + R + 1.4f, z0 = p->j * B + R + 1.4f, L = B - R - 2.8f;
    float s = fmodf(p->s, 4 * L);
    if (s < 0) s += 4 * L;
    int e = (int)(s / L);
    float u = s - e * L;
    switch (e) {
        case 0:  *x = x0 + u; *z = z0; *a = 0; break;
        case 1:  *x = x0 + L; *z = z0 + u; *a = PI_F / 2; break;
        case 2:  *x = x0 + L - u; *z = z0 + L; *a = PI_F; break;
        default: *x = x0; *z = z0 + L - u; *a = -PI_F / 2; break;
    }
    if (p->spd < 0) *a += PI_F;
}

static void draw_person(float x, float z, float a, int r, int g, int b, float fog, bool down, float walk)
{
    if (down) {
        draw_box(x, z, a, 0.9f, 0.3f, 0, 0.3f, r, g, b, fog, 0);
        return;
    }
    float sw = sinf(walk) * 0.25f, ca = cosf(a), sa = sinf(a);
    draw_box(x + ca * sw - sa * 0.13f, z + sa * sw + ca * 0.13f, a, 0.12f, 0.1f, 0, 0.85f, 40, 45, 70, fog, 0);
    draw_box(x - ca * sw + sa * 0.13f, z - sa * sw - ca * 0.13f, a, 0.12f, 0.1f, 0, 0.85f, 40, 45, 70, fog, 0);
    draw_box(x, z, a, 0.18f, 0.3f, 0.85f, 1.5f, r, g, b, fog, 0);
    draw_box(x, z, a, 0.13f, 0.13f, 1.5f, 1.8f, 225, 180, 140, fog, 0);
}

static void draw_pillar(float x, float z, int r, int g, int b, float fog, float t)
{
    float k = 0.85f + 0.25f * sinf(t * 5);
    draw_box(x, z, t * 0.8f, 0.9f, 0.9f, 0, 22, r, g, b, fog * 0.4f, k);
}

/* ---------------- frame rendering ---------------- */

typedef struct { float d; uint8_t type; uint8_t a, b; } drawable_t;
enum { D_BUILDING, D_TREE, D_CAR, D_PED, D_PLAYER, D_PILLAR_START, D_PILLAR_TARGET };
#define MAX_DRAW 260
static drawable_t dl[MAX_DRAW];
static int dn;

static int dl_cmp(const void *a, const void *b)
{
    float d = ((const drawable_t *)b)->d - ((const drawable_t *)a)->d;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* adds an object if it is roughly inside the view */
static void dl_add(float x, float z, float rad, uint8_t type, uint8_t a, uint8_t b)
{
    if (dn >= MAX_DRAW) return;
    float dx = x - cam_x, dz = z - cam_z;
    float fz = dx * cyaw_c + dz * cyaw_s;
    if (fz < -rad) return;
    float rx = -dx * cyaw_s + dz * cyaw_c;
    if (fabsf(rx) > (fz + rad) * (half_w / focal) * 1.2f + rad) return;
    float d2 = dx * dx + dz * dz;
    if (d2 > (DRAW_DIST + rad) * (DRAW_DIST + rad)) return;
    dl[dn].d = d2; dl[dn].type = type; dl[dn].a = a; dl[dn].b = b;
    dn++;
}

static void player_pos(float *x, float *z)
{
    if (on_foot || pcar < 0) { *x = px; *z = pz; }
    else { *x = cars[pcar].x; *z = cars[pcar].z; }
}

static void sky_and_ground(float t)
{
    /* sky colors from time of day */
    float day = s_light;
    int tr = (int)(15 + 95 * day), tg = (int)(20 + 150 * day), tb = (int)(48 + 190 * day);
    float dusk = 1 - fabsf(day - 0.55f) / 0.25f;
    if (dusk < 0) dusk = 0;
    fog_r = (uint8_t)(30 + 150 * day + 60 * dusk);
    fog_g = (uint8_t)(34 + 165 * day + 10 * dusk);
    fog_b = (uint8_t)(60 + 165 * day - 30 * dusk);
    float hz = half_h - focal * (cp_s / cp_c);  /* horizon row */
    for (int y = 0; y < H; y++) {
        uint16_t col, *row = s_fb + y * W;
        if (y + 0.5f < hz) {
            float k = (y + 0.5f) / (hz > 1 ? hz : 1);
            col = RGB((int)(tr + (fog_r - tr) * k), (int)(tg + (fog_g - tg) * k), (int)(tb + (fog_b - tb) * k));
        } else {
            /* distance to the ground seen through this row */
            float v = (half_h - (y + 0.5f)) / focal;
            float cy = v * cp_c - cp_s, cz = v * cp_s + cp_c;
            float d = cy < -1e-4f ? cam_y / -cy * cz : 1e6f;
            col = shade(64, 64, 70, s_light, fog_at(d));
        }
        for (int x = 0; x < W; x++) row[x] = col;
    }
}

static void render(float t)
{
    sky_and_ground(t);
    /* ground: sidewalks and parks around the camera */
    int ci = (int)(cam_x / B), cj = (int)(cam_z / B);
    int rad = (int)(DRAW_DIST / B) + 1;
    for (int j = cj - rad; j <= cj + rad; j++)
        for (int i = ci - rad; i <= ci + rad; i++) {
            if (i < 0 || j < 0 || i >= N || j >= N) continue;
            float x0 = i * B + R, z0 = j * B + R, x1 = x0 + B - R, z1 = z0 + B - R;
            float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
            float dx = cx - cam_x, dz = cz - cam_z, d = sqrtf(dx * dx + dz * dz);
            if (d > DRAW_DIST + 20) continue;
            if (dx * cyaw_c + dz * cyaw_s < -22) continue;
            float f = fog_at(d);
            if (lot_h[i][j]) {
                ground_quad(x0, z0, x1, z1, 0.02f, shade(150, 148, 142, s_light, f));
            } else {
                ground_quad(x0, z0, x1, z1, 0.02f, shade(150, 148, 142, s_light, f));
                ground_quad(x0 + 1.5f, z0 + 1.5f, x1 - 1.5f, z1 - 1.5f, 0.04f, shade(72, 140, 62, s_light, f));
            }
            /* dashed centre lines on the two roads touching this lot (near only) */
            if (d < 60) {
                uint16_t lc = shade(220, 200, 90, s_light, f);
                for (int k = 0; k < 3; k++) {
                    float u = x0 + 4 + k * 9;
                    ground_quad(u, j * B + R * 0.5f - 0.15f, u + 4, j * B + R * 0.5f + 0.15f, 0.03f, lc);
                    u = z0 + 4 + k * 9;
                    ground_quad(i * B + R * 0.5f - 0.15f, u, i * B + R * 0.5f + 0.15f, u + 4, 0.03f, lc);
                }
            }
        }

    /* objects */
    dn = 0;
    for (int j = cj - rad; j <= cj + rad; j++)
        for (int i = ci - rad; i <= ci + rad; i++) {
            if (i < 0 || j < 0 || i >= N || j >= N) continue;
            float cx = i * B + R + (B - R) * 0.5f, cz = j * B + R + (B - R) * 0.5f;
            if (lot_h[i][j]) dl_add(cx, cz, 20, D_BUILDING, i, j);
            else
                for (int k = 0; k < 4; k++) {
                    float ox = (k & 1 ? 6.5f : -6.5f), oz = (k & 2 ? 6.5f : -6.5f);
                    dl_add(cx + ox, cz + oz, 3, D_TREE, i * N + j, k);
                }
        }
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].kind) continue;
        if (i == pcar && !on_foot && cam_mode == 1) continue;
        dl_add(cars[i].x, cars[i].z, 4, D_CAR, i, 0);
    }
    for (int i = 0; i < N_PEDS; i++) {
        float x, z, a;
        ped_pos(&peds[i], &x, &z, &a);
        dl_add(x, z, 1.5f, D_PED, i, 0);
    }
    if (on_foot && cam_mode == 0) dl_add(px, pz, 1.5f, D_PLAYER, 0, 0);
    if (mtype == MS_NONE) {
        for (int i = 0; i < 3; i++) dl_add(start_x[i], start_z[i], 2, D_PILLAR_START, i, 0);
    } else if (mcp < mcount) {
        dl_add(mx[mcp], mz[mcp], 2, D_PILLAR_TARGET, 0, 0);
    }
    qsort(dl, dn, sizeof(dl[0]), dl_cmp);

    for (int n = 0; n < dn; n++) {
        drawable_t *e = &dl[n];
        float d = sqrtf(e->d), f = fog_at(d);
        switch (e->type) {
            case D_BUILDING: draw_building(e->a, e->b, f, d); break;
            case D_TREE: {
                int i = e->a / N, j = e->a % N;
                float cx = i * B + R + (B - R) * 0.5f, cz = j * B + R + (B - R) * 0.5f;
                draw_tree(cx + (e->b & 1 ? 6.5f : -6.5f), cz + (e->b & 2 ? 6.5f : -6.5f), f);
                break;
            }
            case D_CAR: draw_car(&cars[e->a], f, t); break;
            case D_PED: {
                ped_t *p = &peds[e->a];
                float x, z, a;
                ped_pos(p, &x, &z, &a);
                draw_person(x, z, a, p->r, p->g, p->b, f, p->down > 0, p->s * 2.2f);
                break;
            }
            case D_PLAYER: draw_person(px, pz, pa, 60, 170, 230, f, false, t * 9); break;
            case D_PILLAR_START: {
                static const uint8_t pc[3][3] = { {250, 210, 40}, {240, 60, 60}, {60, 220, 110} };
                draw_pillar(start_x[e->a], start_z[e->a], pc[e->a][0], pc[e->a][1], pc[e->a][2], f, t);
                break;
            }
            case D_PILLAR_TARGET: draw_pillar(mx[mcp], mz[mcp], 250, 230, 60, f, t); break;
        }
    }
}

/* ---------------- simulation ---------------- */

static float wrap_pi(float a)
{
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

static float car_hl(const car_t *c) { return c->model == M_TRUCK ? 3.2f : 2.1f; }

/* Arcade car physics with grip/drift and building collisions. */
static void car_physics(car_t *c, float throttle, float steer, bool brake_hand, float dt)
{
    float ca = cosf(c->a), sa = sinf(c->a);
    float vf = c->vx * ca + c->vz * sa, vr = -c->vx * sa + c->vz * ca;
    float acc = c->model == M_SPORTS ? 19 : c->model == M_TRUCK ? 8 : c->model == M_POLICE ? 17 : 13;
    float vmax = c->model == M_SPORTS ? 42 : c->model == M_TRUCK ? 24 : c->model == M_POLICE ? 36 : 32;
    if (c->kind == K_POLICE && stars >= 4) { acc *= 1.15f; vmax *= 1.1f; }
    if (throttle > 0.05f) {
        if (vf < -0.5f) vf += 30 * throttle * dt;
        else if (vf < vmax) vf += acc * throttle * dt;
    } else if (throttle < -0.05f) {
        if (vf > 0.5f) vf += 30 * throttle * dt;
        else if (vf > -9) vf += 9 * throttle * dt;
    } else {
        float d = 4 * dt;
        vf = fabsf(vf) < d ? 0 : vf - (vf > 0 ? d : -d);
    }
    vf -= vf * 0.12f * dt;
    if (brake_hand) vf -= vf * 1.1f * dt;
    vr *= expf(-(brake_hand ? 1.4f : 8.0f) * dt);
    float grip = vf / 7;
    if (grip > 1) grip = 1;
    if (grip < -1) grip = -1;
    c->a = wrap_pi(c->a + steer * (brake_hand ? 2.9f : 2.1f) * grip / (1 + fabsf(vf) / 45) * dt);
    ca = cosf(c->a); sa = sinf(c->a);
    c->vx = ca * vf - sa * vr;
    c->vz = sa * vf + ca * vr;

    float rad = c->model == M_TRUCK ? 1.9f : 1.5f, hit = 0;
    float nx = c->x + c->vx * dt;
    if (solid(nx, c->z, rad)) { hit += fabsf(c->vx); c->vx *= -0.3f; }
    else c->x = nx;
    float nz = c->z + c->vz * dt;
    if (solid(c->x, nz, rad)) { hit += fabsf(c->vz); c->vz *= -0.3f; }
    else c->z = nz;
    if (hit > 7) c->dmg += (hit - 7) * 1.1f;
}

static void car_speed(const car_t *c, float *vf)
{
    *vf = c->vx * cosf(c->a) + c->vz * sinf(c->a);
}

static void set_car_look(car_t *c, int model)
{
    c->model = model;
    if (model == M_POLICE) { c->r = 235; c->g = 235; c->b = 240; }
    else if (model == M_TAXI) { c->r = 245; c->g = 195; c->b = 30; }
    else {
        const uint8_t *k = car_cols[esp_random() % (sizeof(car_cols) / 3)];
        c->r = k[0]; c->g = k[1]; c->b = k[2];
    }
}

static int random_model(void)
{
    int r = esp_random() % 10;
    return r < 5 ? M_COMPACT : r < 7 ? M_SPORTS : r < 9 ? M_TRUCK : M_TAXI;
}

/* places a car on a lane, roughly `dmin..dmax` away from (ox,oz) */
static void place_on_lane(car_t *c, float ox, float oz, float dmin, float dmax)
{
    for (int tries = 0; tries < 30; tries++) {
        float ang = frand() * 2 * PI_F, d = dmin + frand() * (dmax - dmin);
        float x = ox + cosf(ang) * d, z = oz + sinf(ang) * d;
        if (x < R || z < R || x > WORLD - R || z > WORLD - R) continue;
        int dir = esp_random() & 3;
        if (dir == 0 || dir == 2) {
            int m = (int)lroundf((z - R * 0.5f) / B);
            if (m < 0) m = 0;
            if (m > N) m = N;
            c->lane = m;
            c->z = road_c(m) + (dir == 0 ? LANE : -LANE);
            c->x = x;
            c->nk = dir == 0 ? (int)floorf((x - R * 0.5f) / B) + 1 : (int)ceilf((x - R * 0.5f) / B) - 1;
        } else {
            int m = (int)lroundf((x - R * 0.5f) / B);
            if (m < 0) m = 0;
            if (m > N) m = N;
            c->lane = m;
            c->x = road_c(m) + (dir == 1 ? -LANE : LANE);
            c->z = z;
            c->nk = dir == 1 ? (int)floorf((z - R * 0.5f) / B) + 1 : (int)ceilf((z - R * 0.5f) / B) - 1;
        }
        if (c->nk < 0 || c->nk > N) continue;
        c->dir = dir;
        c->a = dir * PI_F / 2;
        c->vx = c->vz = 0;
        c->wpx = c->wpz = 0;
        return;
    }
}

static void spawn_traffic(car_t *c, float ox, float oz, float dmin, float dmax)
{
    memset(c, 0, sizeof(*c));
    c->kind = K_TRAFFIC;
    set_car_look(c, random_model());
    place_on_lane(c, ox, oz, dmin, dmax);
}

static void spawn_parked(car_t *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = K_PARKED;
    set_car_look(c, random_model());
    int i = esp_random() % N, j = esp_random() % N, side = esp_random() & 3;
    float u = R + 4 + frand() * (B - R - 8);
    switch (side) { /* along a curb, facing along the road */
        case 0:  c->x = i * B + u; c->z = j * B + R - 1.1f; c->a = 0; break;
        case 1:  c->x = i * B + u; c->z = (j + 1) * B + 1.1f; c->a = PI_F; break;
        case 2:  c->z = j * B + u; c->x = i * B + R - 1.1f; c->a = PI_F / 2; break;
        default: c->z = j * B + u; c->x = (i + 1) * B + 1.1f; c->a = -PI_F / 2; break;
    }
}

static void spawn_ped(ped_t *p, float ox, float oz)
{
    int ci = (int)(ox / B), cj = (int)(oz / B);
    p->i = ci + (int)(esp_random() % 7) - 3;
    p->j = cj + (int)(esp_random() % 7) - 3;
    if (p->i < 0) p->i = 0;
    if (p->j < 0) p->j = 0;
    if (p->i >= N) p->i = N - 1;
    if (p->j >= N) p->j = N - 1;
    p->s = frand() * 100;
    p->spd = (0.9f + frand() * 0.8f) * (esp_random() & 1 ? 1 : -1);
    p->down = 0;
    const uint8_t *k = car_cols[esp_random() % (sizeof(car_cols) / 3)];
    p->r = k[0]; p->g = k[1]; p->b = k[2];
}

static void traffic_step(car_t *c, int self, float dt, float plx, float plz)
{
    float ca = cosf(c->a), sa = sinf(c->a);
    /* stop for anything in front */
    float want = 10 + (self % 4) * 1.5f;
    if (c->stop > 0) { c->stop -= dt; want = 0; }
    for (int i = 0; i < MAX_CARS && want > 0; i++) {
        if (i == self || !cars[i].kind) continue;
        float dx = cars[i].x - c->x, dz = cars[i].z - c->z;
        float f = dx * ca + dz * sa, l = -dx * sa + dz * ca;
        if (f > 0 && f < 8 && fabsf(l) < 1.6f) want = 0;
    }
    if (on_foot) {
        float dx = plx - c->x, dz = plz - c->z;
        float f = dx * ca + dz * sa, l = -dx * sa + dz * ca;
        if (f > 0 && f < 7 && fabsf(l) < 2) want = 0;
    }
    float v = sqrtf(c->vx * c->vx + c->vz * c->vz);
    v += (want > v ? 6 : -18) * dt;
    if (v < 0) v = 0;
    c->vx = ca * v; c->vz = sa * v;
    c->x += c->vx * dt;
    c->z += c->vz * dt;

    /* keep in lane */
    if (c->dir == 0 || c->dir == 2) c->z += (road_c(c->lane) + (c->dir == 0 ? LANE : -LANE) - c->z) * 4 * dt;
    else c->x += (road_c(c->lane) + (c->dir == 1 ? -LANE : LANE) - c->x) * 4 * dt;

    /* intersections */
    float u = (c->dir == 0 || c->dir == 2) ? c->x : c->z;
    bool cross = (c->dir == 0 || c->dir == 1) ? u >= road_c(c->nk) : u <= road_c(c->nk);
    if (cross) {
        int opts[5], n = 0;
        int cand[3] = { c->dir, (c->dir + 1) & 3, (c->dir + 3) & 3 };
        for (int k = 0; k < 3; k++) {
            int d = cand[k];
            int nidx = d == c->dir ? c->nk + (d < 2 ? 1 : -1) : c->lane + (d < 2 ? 1 : -1);
            if (nidx < 0 || nidx > N) continue;
            opts[n++] = d;
            if (k == 0) { opts[n++] = d; opts[n++] = d; } /* prefer going straight */
        }
        int nd = n ? opts[esp_random() % n] : (c->dir + 2) & 3;
        if (nd == c->dir) {
            c->nk += nd < 2 ? 1 : -1;
        } else {
            int cross_idx = c->nk;
            int new_lane = cross_idx;
            int nk = c->lane + (nd < 2 ? 1 : -1);
            if ((nd & 1) == (c->dir & 1)) { /* U-turn */
                new_lane = c->lane;
                nk = c->nk + (nd < 2 ? 1 : -1);
            }
            c->lane = new_lane;
            c->nk = nk;
            c->dir = nd;
            c->a = nd * PI_F / 2;
        }
    }
}

static void add_stars(int n)
{
    stars += n;
    if (stars > 5) stars = 5;
    seen_t = 0;
    star_flash = 1.5f;
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_CARS; i++) if (!cars[i].kind) return i;
    return -1;
}

static void police_step(car_t *c, float dt, float plx, float plz, float pspeed)
{
    if (stars == 0) {
        c->calm += dt;
        car_physics(c, 0, 0, true, dt);
        return;
    }
    c->calm = 0;
    float dx = plx - c->x, dz = plz - c->z, d = sqrtf(dx * dx + dz * dz);
    float tx = plx, tz = plz;
    if (d > 26) {
        /* navigate the road grid: head for the intersection that gets closer */
        int ki = (int)lroundf((c->x - R * 0.5f) / B), kj = (int)lroundf((c->z - R * 0.5f) / B);
        if (c->wpx == 0 && c->wpz == 0) { c->wpx = road_c(ki); c->wpz = road_c(kj); }
        if (hypotf(c->wpx - c->x, c->wpz - c->z) < 7) {
            /* at a junction: pick the neighbouring junction nearest the player */
            static const int nb[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
            float best = 1e9f;
            for (int k = 0; k < 4; k++) {
                int ni = ki + nb[k][0], nj = kj + nb[k][1];
                if (ni < 0 || nj < 0 || ni > N || nj > N) continue;
                float dd = hypotf(plx - road_c(ni), plz - road_c(nj));
                if (dd < best) { best = dd; c->wpx = road_c(ni); c->wpz = road_c(nj); }
            }
        }
        tx = c->wpx; tz = c->wpz;
    }
    float want = atan2f(tz - c->z, tx - c->x);
    float diff = wrap_pi(want - c->a);
    float steer = diff * 2.5f;
    if (steer > 1) steer = 1;
    if (steer < -1) steer = -1;
    float vf;
    car_speed(c, &vf);
    float throttle = fabsf(diff) > 1.6f && vf > 8 ? -0.6f : 1.0f;
    if (d < 8 && pspeed < 4) throttle = -0.5f;    /* box the player in */
    if (c->rev > 0) {
        c->rev -= dt;
        throttle = -1;
        steer = -steer;
    } else if (fabsf(vf) < 1.5f && throttle > 0) {
        c->stuck += dt;
        if (c->stuck > 1.2f) { c->rev = 0.9f; c->stuck = 0; }
    } else c->stuck = 0;
    car_physics(c, throttle, steer, fabsf(diff) > 0.9f && vf > 14, dt);
}

static void spawn_police(float plx, float plz)
{
    int s = free_slot();
    if (s < 0) return;
    car_t *c = &cars[s];
    memset(c, 0, sizeof(*c));
    c->kind = K_POLICE;
    set_car_look(c, M_POLICE);
    place_on_lane(c, plx, plz, 75, 110);
    c->a = atan2f(plz - c->z, plx - c->x);
}

/* car-vs-car bumps for player/police against everything */
static void collide_cars(float dt)
{
    for (int i = 0; i < MAX_CARS; i++) {
        car_t *a = &cars[i];
        if (a->kind != K_PLAYER && a->kind != K_POLICE) continue;
        for (int j = 0; j < MAX_CARS; j++) {
            car_t *b = &cars[j];
            if (j == i || !b->kind) continue;
            if ((b->kind == K_PLAYER || b->kind == K_POLICE) && j < i) continue;
            float dx = b->x - a->x, dz = b->z - a->z, d2 = dx * dx + dz * dz;
            float rr = (car_hl(a) + car_hl(b)) * 0.62f;
            if (d2 >= rr * rr || d2 < 1e-4f) continue;
            float d = sqrtf(d2), nx = dx / d, nz = dz / d, push = (rr - d) * 0.5f;
            bool b_moves = b->kind == K_PLAYER || b->kind == K_POLICE;
            a->x -= nx * push * (b_moves ? 1 : 1.4f);
            a->z -= nz * push * (b_moves ? 1 : 1.4f);
            if (b_moves || b->kind != K_WRECK) {
                if (!solid(b->x + nx * push, b->z + nz * push, 1.5f)) { b->x += nx * push; b->z += nz * push; }
            }
            float rel = (a->vx - b->vx) * nx + (a->vz - b->vz) * nz;
            if (rel > 0) {
                a->vx -= nx * rel * 0.7f; a->vz -= nz * rel * 0.7f;
                if (b_moves) { b->vx += nx * rel * 0.6f; b->vz += nz * rel * 0.6f; }
                if (b->kind == K_TRAFFIC) b->stop = 2.5f;
                if (rel > 8) { a->dmg += rel * 0.8f; b->dmg += rel * 0.8f; }
                bool player_hit_cop = (a->kind == K_PLAYER && b->kind == K_POLICE) ||
                                      (a->kind == K_POLICE && b->kind == K_PLAYER);
                if (player_hit_cop && rel > 7 && stars < 2) add_stars(stars ? 1 : 2);
            }
        }
    }
    (void)dt;
}

/* ---------------- missions ---------------- */

static void random_road_point(float ox, float oz, float dmin, float dmax, float *x, float *z)
{
    for (int t = 0; t < 60; t++) {
        float rx, rz;
        if (esp_random() & 1) { rx = road_c(esp_random() % (N + 1)); rz = R + frand() * (WORLD - 2 * R); }
        else { rz = road_c(esp_random() % (N + 1)); rx = R + frand() * (WORLD - 2 * R); }
        float d = hypotf(rx - ox, rz - oz);
        if (d >= dmin && d <= dmax) { *x = rx; *z = rz; return; }
    }
    *x = road_c(N / 2); *z = road_c(N / 2);
}

static void save_progress(void)
{
    city_save_t s = { SAVE_MAGIC, cash, jobs_done };
    FILE *f = fopen(DIR_SAVES "/city.sav", "wb");
    if (f) { fwrite(&s, sizeof(s), 1, f); fclose(f); }
}

static void load_progress(void)
{
    city_save_t s;
    cash = 0; jobs_done = 0;
    FILE *f = fopen(DIR_SAVES "/city.sav", "rb");
    if (f) {
        if (fread(&s, sizeof(s), 1, f) == 1 && s.magic == SAVE_MAGIC) { cash = s.cash; jobs_done = s.jobs; }
        fclose(f);
    }
}

static void mission_end(bool ok, int reward)
{
    char m[40];
    if (ok) {
        cash += reward;
        jobs_done++;
        snprintf(m, sizeof(m), "+$%d", reward);
        say(T("JOB DONE!"), m, 3);
        save_progress();
    } else {
        say(T("JOB FAILED"), NULL, 2.5f);
    }
    mtype = MS_NONE;
    mcar = -1;
    mission_cool = 4;
}

static void mission_start(int type, float plx, float plz)
{
    float total = 0, ox = plx, oz = plz;
    mtype = type;
    mcp = 0;
    switch (type) {
        case MS_TAXI:
            mcount = 1;
            random_road_point(plx, plz, 120, 260, &mx[0], &mz[0]);
            total = hypotf(mx[0] - plx, mz[0] - plz);
            mtime = total / 10 + 20;
            say(T("TAXI"), T("Drive the passenger there"), 2.5f);
            break;
        case MS_RACE:
            mcount = 6;
            for (int i = 0; i < mcount; i++) {
                random_road_point(ox, oz, 60, 110, &mx[i], &mz[i]);
                total += hypotf(mx[i] - ox, mz[i] - oz);
                ox = mx[i]; oz = mz[i];
            }
            mtime = total / 14 + 12;
            say(T("RACE"), T("Hit every checkpoint"), 2.5f);
            break;
        case MS_DELIVER: {
            mcount = 1;
            random_road_point(plx, plz, 160, 320, &mx[0], &mz[0]);
            total = hypotf(mx[0] - plx, mz[0] - plz);
            mtime = total / 9 + 30;
            int s = free_slot();
            if (s >= 0) {
                car_t *c = &cars[s];
                memset(c, 0, sizeof(*c));
                c->kind = K_PARKED;
                set_car_look(c, M_SPORTS);
                c->r = 250; c->g = 120; c->b = 20;
                c->x = start_x[2] + 4; c->z = start_z[2] + 4;
                if (solid(c->x, c->z, 1.5f)) { c->x = start_x[2]; c->z = start_z[2]; }
                c->a = 0;
                mcar = s;
            }
            say(T("DELIVERY"), T("Bring the orange car in one piece"), 3);
            break;
        }
    }
}

static void mission_update(float dt, float plx, float plz)
{
    if (mission_cool > 0) mission_cool -= dt;
    if (mtype == MS_NONE) {
        if (mission_cool > 0) return;
        for (int i = 0; i < 3; i++) {
            if (hypotf(plx - start_x[i], plz - start_z[i]) > 3.5f) continue;
            if (i < 2 && on_foot) { say(NULL, T("Get in a car first"), 1.5f); mission_cool = 2; return; }
            mission_start(i + 1, plx, plz);
            return;
        }
        return;
    }
    mtime -= dt;
    if (mtime <= 0) { mission_end(false, 0); return; }
    if (mtype == MS_DELIVER && mcar >= 0 && cars[mcar].kind == K_WRECK) { mission_end(false, 0); return; }
    bool in_right_car = !on_foot && (mtype != MS_DELIVER || pcar == mcar);
    if (in_right_car && hypotf(plx - mx[mcp], plz - mz[mcp]) < 6) {
        mcp++;
        if (mcp >= mcount) {
            int reward = mtype == MS_TAXI ? 90 + (int)(mtime * 4)
                       : mtype == MS_RACE ? 260 + (int)(mtime * 10)
                       : 450 - (int)(cars[pcar].dmg * 3);
            if (reward < 50) reward = 50;
            mission_end(true, reward);
        }
    }
}

/* ---------------- HUD ---------------- */

static void text_shadow(int x, int y, const char *s, uint16_t c)
{
    gfx_text(s_fb, x + 1, y + 1, s, RGB(0, 0, 0), 0);
    gfx_text(s_fb, x, y, s, c, 0);
}

static void draw_star(float cx, float cy, float r, uint16_t col)
{
    float xs[10], ys[10];
    for (int i = 0; i < 10; i++) { /* drawn as 5 triangles + centre pentagon */
        float a = -PI_F / 2 + i * PI_F / 5, rr = i & 1 ? r * 0.42f : r;
        xs[i] = cx + cosf(a) * rr; ys[i] = cy + sinf(a) * rr;
    }
    float px_[5], py_[5];
    for (int i = 0; i < 5; i++) { px_[i] = xs[i * 2 + 1]; py_[i] = ys[i * 2 + 1]; }
    fill_convex(px_, py_, 5, col);
    for (int i = 0; i < 5; i++) {
        float tx[3] = { xs[i * 2], xs[(i * 2 + 1) % 10], xs[(i * 2 + 9) % 10] };
        float ty[3] = { ys[i * 2], ys[(i * 2 + 1) % 10], ys[(i * 2 + 9) % 10] };
        fill_convex(tx, ty, 3, col);
    }
}

static void draw_minimap(float plx, float plz, float t)
{
    const int S = W < 140 ? 36 : 42, x0 = W - S - 3, y0 = H - S - 3;
    const float sc = 3.2f;
    float fx = cyaw_c, fz = cyaw_s, rx = -cyaw_s, rz = cyaw_c;
    for (int v = 0; v < S; v++) {
        uint16_t *row = s_fb + (y0 + v) * W + x0;
        float dv = (S / 2 - v) * sc;
        for (int u = 0; u < S; u++) {
            float du = (u - S / 2) * sc;
            float wx = plx + fx * dv + rx * du, wz = plz + fz * dv + rz * du;
            int k = tile_kind(wx, wz);
            row[u] = k == 0 ? RGB(105, 105, 115) : k == 2 ? RGB(40, 44, 58) : k == 3 ? RGB(45, 95, 45)
                   : k == 1 ? RGB(62, 66, 80) : RGB(10, 10, 14);
        }
    }
    gfx_rect(s_fb, x0 - 1, y0 - 1, S + 2, 1, RGB(0, 0, 0));
    gfx_rect(s_fb, x0 - 1, y0 + S, S + 2, 1, RGB(0, 0, 0));
    #define MAP_DOT(wx, wz, col, sz, clampit) do { \
        float ddx = (wx) - plx, ddz = (wz) - plz; \
        float mu = (ddx * rx + ddz * rz) / sc, mv = -(ddx * fx + ddz * fz) / sc; \
        float lim = S / 2 - 2; \
        if (clampit) { float m = fmaxf(fabsf(mu), fabsf(mv)); if (m > lim) { mu *= lim / m; mv *= lim / m; } } \
        if (fabsf(mu) <= S / 2 - 1 && fabsf(mv) <= S / 2 - 1) \
            gfx_rect(s_fb, x0 + S / 2 + (int)mu - sz / 2, y0 + S / 2 + (int)mv - sz / 2, sz, sz, col); \
    } while (0)
    for (int i = 0; i < MAX_CARS; i++)
        if (cars[i].kind == K_POLICE)
            MAP_DOT(cars[i].x, cars[i].z, fmodf(t * 4, 1) < 0.5f ? RGB(255, 40, 40) : RGB(60, 90, 255), 3, 0);
    if (mtype == MS_NONE) {
        MAP_DOT(start_x[0], start_z[0], RGB(250, 210, 40), 3, 1);
        MAP_DOT(start_x[1], start_z[1], RGB(240, 60, 60), 3, 1);
        MAP_DOT(start_x[2], start_z[2], RGB(60, 220, 110), 3, 1);
    } else if (mcp < mcount) {
        MAP_DOT(mx[mcp], mz[mcp], RGB(255, 240, 60), 4, 1);
    }
    if (mtype == MS_DELIVER && mcar >= 0 && pcar != mcar) MAP_DOT(cars[mcar].x, cars[mcar].z, RGB(255, 140, 30), 3, 1);
    /* player arrow (always pointing up, the map turns) */
    float ax[3] = { x0 + S / 2.0f, x0 + S / 2.0f - 3, x0 + S / 2.0f + 3 };
    float ay[3] = { y0 + S / 2.0f - 4, y0 + S / 2.0f + 3, y0 + S / 2.0f + 3 };
    fill_convex(ax, ay, 3, RGB(255, 255, 255));
}

static void draw_hud(float plx, float plz, float t, float pspeed)
{
    char s[40];
    snprintf(s, sizeof(s), "$%d", cash);
    text_shadow(3, 3, s, RGB(120, 235, 120));
    /* wanted stars */
    bool blink = star_flash > 0 && fmodf(t * 6, 1) < 0.5f;
    for (int i = 0; i < 5; i++) {
        uint16_t c = i < stars ? (blink ? RGB(255, 255, 255) : RGB(255, 205, 40)) : RGB(60, 62, 75);
        if (i < stars || stars) draw_star(W - 8 - i * 10, 7, 4.2f, c);
    }
    /* job info */
    if (mtype != MS_NONE && mcp < mcount) {
        int sec = (int)mtime;
        snprintf(s, sizeof(s), "%d:%02d", sec / 60, sec % 60);
        if (mtype == MS_RACE) snprintf(s + strlen(s), sizeof(s) - strlen(s), " %d/%d", mcp + 1, mcount);
        int tw = (int)strlen(s) * CHAR_W;
        text_shadow(W / 2 - tw / 2, 14, s, mtime < 10 && fmodf(t * 4, 1) < 0.5f ? RGB(255, 80, 80) : RGB(255, 255, 255));
        /* arrow towards the target */
        float rel = atan2f(mz[mcp] - plz, mx[mcp] - plx) - atan2f(cyaw_s, cyaw_c);
        float sa = sinf(rel), ca = cosf(rel);
        float cx = W / 2.0f, cy = 6;
        float lx[3] = { 0, -3.5f, 3.5f }, ly[3] = { -5, 4, 4 };
        float ax[3], ay[3];
        for (int i = 0; i < 3; i++) { /* rotate: forward = up on screen, right = right */
            ax[i] = cx + lx[i] * ca - ly[i] * sa;
            ay[i] = cy + lx[i] * sa + ly[i] * ca;
        }
        fill_convex(ax, ay, 3, RGB(255, 230, 60));
        int dist = (int)hypotf(mx[mcp] - plx, mz[mcp] - plz);
        snprintf(s, sizeof(s), "%dm", dist);
        text_shadow((int)cx + 7, 3, s, RGB(255, 230, 60));
    }
    /* speed + damage */
    if (!on_foot && pcar >= 0) {
        snprintf(s, sizeof(s), T("%d km/h"), (int)(fabsf(pspeed) * 3.6f));
        text_shadow(3, H - 10, s, RGB(255, 255, 255));
        float dmg = cars[pcar].dmg;
        int bw = 36;
        gfx_rect(s_fb, 3, H - 14, bw, 2, RGB(30, 30, 30));
        int left = (int)(bw * (1 - dmg / 100));
        if (left < 0) left = 0;
        gfx_rect(s_fb, 3, H - 14, left, 2, dmg > 70 ? RGB(240, 60, 50) : RGB(90, 220, 90));
    }
    draw_minimap(plx, plz, t);
    /* messages */
    if (msg_t > 0) {
        if (msg_big[0]) {
            int tw = gfx_text_width(msg_big, 2);
            gfx_text_scaled(s_fb, (W - tw) / 2 + 1, H / 2 - 21, msg_big, RGB(0, 0, 0), 0, 2);
            gfx_text_scaled(s_fb, (W - tw) / 2, H / 2 - 22, msg_big, RGB(255, 215, 60), 0, 2);
        }
        if (msg_small[0]) {
            int tw = (int)strlen(msg_small) * CHAR_W;
            if (tw > W - 4) gfx_text_wrap(s_fb, 3, H / 2 - 2, W - 6, msg_small, RGB(255, 255, 255));
            else text_shadow((W - tw) / 2, H / 2 - 2, msg_small, RGB(255, 255, 255));
        }
    }
}

static void draw_big_map(float plx, float plz, float t)
{
    int S = (W < H ? W : H) - 4;
    int x0 = (W - S) / 2, y0 = (H - S) / 2;
    float sc = WORLD / S;
    for (int v = 0; v < S; v++) {
        uint16_t *row = s_fb + (y0 + v) * W + x0;
        for (int u = 0; u < S; u++) {
            int k = tile_kind(u * sc, v * sc);
            row[u] = k == 0 ? RGB(110, 110, 120) : k == 2 ? RGB(38, 42, 56) : k == 3 ? RGB(45, 100, 45) : RGB(60, 64, 78);
        }
    }
    #define BIG_DOT(wx, wz, col, sz) gfx_rect(s_fb, x0 + (int)((wx) / sc) - sz / 2, y0 + (int)((wz) / sc) - sz / 2, sz, sz, col)
    {
        float sx = (STATION_I * B + R + (B - R) / 2) / sc, sz_ = (STATION_J * B + R + (B - R) / 2) / sc;
        gfx_rect(s_fb, x0 + (int)sx - 2, y0 + (int)sz_ - 2, 5, 5, RGB(40, 80, 220));
    }
    if (mtype == MS_NONE) {
        BIG_DOT(start_x[0], start_z[0], RGB(250, 210, 40), 4);
        BIG_DOT(start_x[1], start_z[1], RGB(240, 60, 60), 4);
        BIG_DOT(start_x[2], start_z[2], RGB(60, 220, 110), 4);
    } else if (mcp < mcount) BIG_DOT(mx[mcp], mz[mcp], RGB(255, 240, 60), 5);
    for (int i = 0; i < MAX_CARS; i++)
        if (cars[i].kind == K_POLICE) BIG_DOT(cars[i].x, cars[i].z, RGB(255, 50, 50), 3);
    if (fmodf(t * 3, 1) < 0.7f) BIG_DOT(plx, plz, RGB(255, 255, 255), 4);
    text_shadow(x0 + 3, y0 + 3, T("Yellow: taxi  Red: race"), RGB(255, 255, 255));
    text_shadow(x0 + 3, y0 + 12, T("Green: delivery"), RGB(255, 255, 255));
}

/* ---------------- main loop ---------------- */

static const char *const radio[] = { "Pocket FM", "Chip Wave 88", "Night Drive", "Bass City", "Retro Talk" };

static void reset_world(void)
{
    memset(cars, 0, sizeof(cars));
    build_city();
    on_foot = true;
    pcar = -1;
    px = road_c(STATION_I + 1) - 3; pz = STATION_J * B + B * 0.5f + R * 0.5f;
    pa = PI_F / 2;
    cam_yaw = pa;
    stars = 0; seen_t = bust_t = star_flash = 0;
    mtype = MS_NONE; mcar = -1; mission_cool = 0;
    for (int i = 0; i < N_TRAFFIC; i++) spawn_traffic(&cars[i], px, pz, 20, 120);
    for (int i = 0; i < N_PARKED; i++) spawn_parked(&cars[N_TRAFFIC + i]);
    /* one car waiting right next to you */
    car_t *c = &cars[N_TRAFFIC];
    memset(c, 0, sizeof(*c));
    c->kind = K_PARKED;
    set_car_look(c, M_COMPACT);
    c->x = px + 4; c->z = pz + 2; c->a = PI_F / 2;
    for (int i = 0; i < N_PEDS; i++) spawn_ped(&peds[i], px, pz);
    start_x[0] = road_c(STATION_I + 2); start_z[0] = road_c(STATION_J + 1) - 3;
    start_x[1] = road_c(N / 2) + 3;     start_z[1] = road_c(N / 2 + 1) - 14;
    start_x[2] = road_c(STATION_I) + 3; start_z[2] = road_c(STATION_J + 3) + 10;
}

static void busted(void)
{
    say(T("BUSTED"), T("The cops took 10% of your cash"), 3);
    cash -= cash / 10;
    stars = 0; bust_t = 0;
    if (mtype != MS_NONE) { mtype = MS_NONE; mcar = -1; }
    for (int i = 0; i < MAX_CARS; i++) if (cars[i].kind == K_POLICE) cars[i].kind = K_NONE;
    if (pcar >= 0) cars[pcar].kind = K_PARKED;
    on_foot = true; pcar = -1;
    px = road_c(STATION_I + 1) - 3; pz = STATION_J * B + B * 0.5f + R * 0.5f;
    pa = PI_F / 2;
    save_progress();
}

static void exit_car(void)
{
    car_t *c = &cars[pcar];
    float ca = cosf(c->a), sa = sinf(c->a);
    float ex = c->x + sa * 2.2f, ez = c->z - ca * 2.2f;      /* driver's door (left) */
    if (solid(ex, ez, 0.4f)) { ex = c->x - sa * 2.2f; ez = c->z + ca * 2.2f; }
    if (solid(ex, ez, 0.4f)) return;
    px = ex; pz = ez; pa = c->a;
    c->kind = K_PARKED;
    c->vx = c->vz = 0;
    on_foot = true;
    pcar = -1;
}

void game_city_run(const char *unused)
{
    (void)unused;
    gfx_set_origin(0, 0);
    load_progress();
    reset_world();
    pacer_t pacer;
    pacer_start(&pacer);
    int64_t t0 = esp_timer_get_time(), last = t0;
    float tod = 0.32f;      /* time of day 0..1, starts in the morning */
    bool map_open = false;
    say(T("POCKET CITY"), T("Find a colored pillar to start a job"), 3.5f);
#ifdef SIM
    if (getenv("CITY_TOD")) tod = atof(getenv("CITY_TOD"));
    if (getenv("CITY_STARS")) add_stars(atoi(getenv("CITY_STARS")));
#endif

    for (;;) {
        uint32_t k = input_poll(), pr = input_pressed();
        if (bridge_quit_requested()) { save_progress(); return; }
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f, t = (now - t0) / 1e6f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        if (pr & BTN_MENU) {
            ingame_action_t a = ingame_menu("Pocket City", false);
            if (a == MENU_QUIT) { save_progress(); return; }
            if (a == MENU_RESET) reset_world();
            last = esp_timer_get_time();
            pacer_start(&pacer);
            continue;
        }
        if (pr & BTN_START) map_open = !map_open;
        if (pr & BTN_SELECT) cam_mode ^= 1;

        float jx, jy;
        input_joystick(&jx, &jy);
        if (fabsf(jx) < 0.12f) jx = 0;
        if (fabsf(jy) < 0.12f) jy = 0;

        float plx, plz, pspeed = 0;
        if (!map_open) {
            tod += dt / 300;
            if (tod >= 1) tod -= 1;
            /* ---- player ---- */
            if (on_foot) {
                float sp = (k & BTN_B ? 8.5f : 4.2f) * -jy;
                pa = wrap_pi(pa + jx * 2.6f * dt);
                float nx = px + cosf(pa) * sp * dt, nz = pz + sinf(pa) * sp * dt;
                if (!solid(nx, pz, 0.4f)) px = nx;
                if (!solid(px, nz, 0.4f)) pz = nz;
                if (pr & BTN_A) {
                    int best = -1;
                    float bd = 4.5f;
                    for (int i = 0; i < MAX_CARS; i++) {
                        if (!cars[i].kind || cars[i].kind == K_WRECK) continue;
                        float d = hypotf(cars[i].x - px, cars[i].z - pz);
                        if (d < bd) { bd = d; best = i; }
                    }
                    if (best >= 0) {
                        car_t *c = &cars[best];
                        bool cop_near = false;
                        for (int i = 0; i < MAX_CARS; i++)
                            if (cars[i].kind == K_POLICE && hypotf(cars[i].x - px, cars[i].z - pz) < 45) cop_near = true;
                        if (c->kind == K_POLICE) add_stars(stars < 2 ? 2 - stars : 1);
                        else if (c->kind == K_TRAFFIC && cop_near) add_stars(1);
                        if (c->kind == K_TRAFFIC) { c->vx *= 0.3f; c->vz *= 0.3f; }
                        c->kind = K_PLAYER;
                        pcar = best;
                        on_foot = false;
                        char m[40];
                        snprintf(m, sizeof(m), T("Radio: %s"), radio[best % 5]);
                        say(NULL, m, 2);
                    }
                }
            } else {
                car_t *c = &cars[pcar];
                car_physics(c, -jy, jx, k & BTN_B, dt);
                car_speed(c, &pspeed);
                if ((pr & BTN_A) && fabsf(pspeed) < 4) exit_car();
                else if (c->dmg >= 100) {
                    exit_car();
                    if (!on_foot) { on_foot = true; px = c->x; pz = c->z; pcar = -1; } /* no room: just hop out */
                    c->kind = K_WRECK;
                    say(T("WRECKED"), NULL, 2);
                }
            }
            player_pos(&plx, &plz);

            /* ---- traffic / police / peds ---- */
            int police_n = 0;
            for (int i = 0; i < MAX_CARS; i++) {
                car_t *c = &cars[i];
                float d = hypotf(c->x - plx, c->z - plz);
                switch (c->kind) {
                    case K_TRAFFIC:
                        traffic_step(c, i, dt, plx, plz);
                        if (d > 160) spawn_traffic(c, plx, plz, 95, 130);
                        break;
                    case K_POLICE:
                        police_n++;
                        police_step(c, dt, plx, plz, pspeed);
                        if (stars == 0 && (d > 70 || c->calm > 12)) c->kind = K_NONE;
                        else if (d > 220) place_on_lane(c, plx, plz, 80, 110);
                        if (c->dmg >= 100) c->kind = K_WRECK;
                        break;
                    case K_WRECK:
                    case K_PARKED:
                        if (i != mcar && d > 200 && i >= N_TRAFFIC + N_PARKED) c->kind = K_NONE;
                        else if (c->vx != 0 || c->vz != 0) car_physics(c, 0, 0, true, dt);
                        break;
                }
            }
            /* keep enough traffic around */
            int traffic_n = 0;
            for (int i = 0; i < MAX_CARS; i++) traffic_n += cars[i].kind == K_TRAFFIC;
            if (traffic_n < N_TRAFFIC - 2) { int s = free_slot(); if (s >= 0) spawn_traffic(&cars[s], plx, plz, 95, 130); }
            int want_police = stars == 0 ? 0 : stars >= 4 ? 4 : stars;
            if (police_n < want_police && frand() < dt * 0.8f) spawn_police(plx, plz);
            collide_cars(dt);

            for (int i = 0; i < N_PEDS; i++) {
                ped_t *p = &peds[i];
                float x, z, a;
                ped_pos(p, &x, &z, &a);
                if (p->down > 0) { p->down -= dt; continue; }
                p->s += p->spd * dt;
                if (hypotf(x - plx, z - plz) > 130) { spawn_ped(p, plx, plz); continue; }
                if (!on_foot && fabsf(pspeed) > 5 && hypotf(x - plx, z - plz) < 1.9f) {
                    p->down = 4;
                    add_stars(stars < 1 ? 1 : (stars < 3 ? 1 : 0));
                    say(NULL, T("Hey! Watch it!"), 1.2f);
                }
            }

            /* ---- wanted level ---- */
            if (star_flash > 0) star_flash -= dt;
            if (stars) {
                bool seen = false, close = false;
                for (int i = 0; i < MAX_CARS; i++) {
                    if (cars[i].kind != K_POLICE) continue;
                    float d = hypotf(cars[i].x - plx, cars[i].z - plz);
                    if (d < 45) seen = true;
                    if (d < (on_foot ? 6.5f : 5.0f)) close = true;
                }
                seen_t = seen ? 0 : seen_t + dt;
                if (seen_t > 9) {
                    stars--;
                    seen_t = 0;
                    if (!stars) say(NULL, T("You lost the cops"), 2);
                }
                if (close && fabsf(pspeed) < 3) bust_t += dt * (on_foot ? 1.4f : 1.0f);
                else bust_t -= dt;
                if (bust_t < 0) bust_t = 0;
                if (bust_t > 1.6f) busted();
            }
            player_pos(&plx, &plz);
            mission_update(dt, plx, plz);
            if (msg_t > 0) msg_t -= dt;
        } else {
            player_pos(&plx, &plz);
        }

        /* ---- camera ---- */
        float fa = on_foot ? pa : cars[pcar].a;
        if (!on_foot && pspeed < -2) fa = cars[pcar].a; /* keep looking forward when reversing */
        float dyaw = wrap_pi(fa - cam_yaw);
        cam_yaw = wrap_pi(cam_yaw + dyaw * (cam_mode ? 1 : (1 - expf(-5 * dt))));
        if (cam_mode == 1) {
            cam_x = plx + cosf(cam_yaw) * (on_foot ? 0 : 0.2f);
            cam_z = plz + sinf(cam_yaw) * (on_foot ? 0 : 0.2f);
            cam_y = on_foot ? 1.7f : 1.35f;
            cp_c = cosf(0.04f); cp_s = sinf(0.04f);
        } else {
            float back = on_foot ? 4.2f : 7.5f + fabsf(pspeed) * 0.05f, up = on_foot ? 2.3f : 3.0f;
            float bx = plx, bz = plz;
            for (int s = 1; s <= 8; s++) {
                float tx = plx - cosf(cam_yaw) * back * s / 8, tz = plz - sinf(cam_yaw) * back * s / 8;
                if (solid(tx, tz, 0.3f)) break;
                bx = tx; bz = tz;
            }
            cam_x = bx; cam_z = bz; cam_y = up;
            float pitch = on_foot ? 0.22f : 0.2f;
            cp_c = cosf(pitch); cp_s = sinf(pitch);
        }
        cyaw_c = cosf(cam_yaw); cyaw_s = sinf(cam_yaw);
        half_w = W * 0.5f; half_h = H * 0.5f;
        focal = (W < H ? W : H) * 0.95f;
        float day = 0.5f - 0.5f * cosf(tod * 2 * PI_F);     /* 0 midnight .. 1 noon */
        s_light = 0.3f + 0.7f * day;
        s_night = 1 - day;

        s_fb = display_fb();
        if (map_open) {
            render(t);
            draw_big_map(plx, plz, t);
        } else {
            render(t);
            draw_hud(plx, plz, t, pspeed);
            if (on_foot) {
                for (int i = 0; i < MAX_CARS; i++)
                    if (cars[i].kind && cars[i].kind != K_WRECK && hypotf(cars[i].x - px, cars[i].z - pz) < 4.5f) {
                        const char *h = T("A: get in");
                        text_shadow((W - (int)strlen(h) * CHAR_W) / 2, H - 22, h, RGB(255, 255, 255));
                        break;
                    }
            }
        }
        if (g_settings.show_fps) draw_fps(s_fb, pacer.fps);
        display_present();
        pacer_wait(&pacer, 33333);
    }
}
