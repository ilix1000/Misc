/*
 * unzip.c - minimal .zip reader + DEFLATE decoder, so ROMs can stay zipped
 * on the SD card. Supports "stored" and "deflate" entries (what every normal
 * zip tool produces). No zip64, no encryption.
 */
#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unzip.h"
#include "esp_heap_caps.h"

/* ---------------- DEFLATE ---------------- */

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint8_t *out;
    size_t out_len, out_pos;
    uint32_t bitbuf;
    int bitcnt;
    jmp_buf err;
} inf_t;

typedef struct {
    uint16_t count[16];   /* number of codes of each length */
    uint16_t symbol[288]; /* symbols ordered by code */
} huff_t;

static inline uint32_t bits(inf_t *s, int n)
{
    while (s->bitcnt < n) {
        if (s->in_pos >= s->in_len) longjmp(s->err, 1);
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void build(huff_t *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = i;
}

static int decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    longjmp(s->err, 1);
}

static const uint16_t lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                                    67,83,99,115,131,163,195,227,258 };
static const uint8_t lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void codes(inf_t *s, const huff_t *lcode, const huff_t *dcode)
{
    for (;;) {
        int sym = decode(s, lcode);
        if (sym < 256) {
            if (s->out_pos >= s->out_len) longjmp(s->err, 1);
            s->out[s->out_pos++] = sym;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) longjmp(s->err, 1);
            size_t len = lbase[sym] + bits(s, lext[sym]);
            int ds = decode(s, dcode);
            if (ds >= 30) longjmp(s->err, 1);
            size_t dist = dbase[ds] + bits(s, dext[ds]);
            if (dist > s->out_pos || s->out_pos + len > s->out_len) longjmp(s->err, 1);
            uint8_t *o = s->out + s->out_pos;
            for (size_t i = 0; i < len; i++) o[i] = o[(ptrdiff_t)i - (ptrdiff_t)dist];
            s->out_pos += len;
        }
    }
}

static void stored(inf_t *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len) longjmp(s->err, 1);
    unsigned len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
    unsigned nlen = s->in[s->in_pos + 2] | (s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF) || s->in_pos + len > s->in_len || s->out_pos + len > s->out_len)
        longjmp(s->err, 1);
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
}

static void fixed(inf_t *s)
{
    static huff_t lc, dc;
    static bool built;
    if (!built) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(&dc, l, 30);
        built = true;
    }
    codes(s, &lc, &dc);
}

static void dynamic(inf_t *s)
{
    static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    huff_t lc, dc;
    uint8_t l[320];
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) longjmp(s->err, 1);
    memset(l, 0, 19);
    for (int i = 0; i < ncode; i++) l[order[i]] = bits(s, 3);
    build(&lc, l, 19);
    for (int i = 0; i < nlen + ndist; ) {
        int sym = decode(s, &lc), rep, val = 0;
        if (sym < 16) { l[i++] = sym; continue; }
        if (sym == 16) {
            if (i == 0) longjmp(s->err, 1);
            val = l[i - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + bits(s, 3);
        } else {
            rep = 11 + bits(s, 7);
        }
        if (i + rep > nlen + ndist) longjmp(s->err, 1);
        while (rep--) l[i++] = val;
    }
    build(&lc, l, nlen);
    build(&dc, l + nlen, ndist);
    codes(s, &lc, &dc);
}

long inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    inf_t s = { .in = in, .in_len = in_len, .out = out, .out_len = out_len };
    if (setjmp(s.err)) return -1;
    int last;
    do {
        last = bits(&s, 1);
        switch (bits(&s, 2)) {
            case 0: stored(&s); break;
            case 1: fixed(&s); break;
            case 2: dynamic(&s); break;
            default: return -1;
        }
    } while (!last);
    return (long)s.out_pos;
}

uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

/* ---------------- zip container ---------------- */

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool has_ext(const char *name, const char *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[12];
    int n = 0;
    for (const char *p = dot; *p && n < 11; p++) ext[n++] = tolower((unsigned char)*p);
    ext[n] = 0;
    for (const char *p = exts; *p; ) {
        const char *sp = strchr(p, ' ');
        int len = sp ? (int)(sp - p) : (int)strlen(p);
        if (len == n && strncmp(p, ext, n) == 0) return true;
        p += len + (sp ? 1 : 0);
    }
    return false;
}

bool zip_find(const char *zip_path, const char *exts, zip_entry_t *out)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f) return false;
    bool found = false;
    uint8_t *buf = NULL;

    /* End-of-central-directory record is in the last 22..65557 bytes. It's
     * almost always in the last few, so try a small read before a big one. */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long eocd = -1;
    uint8_t rec[22];
    for (int attempt = 0; attempt < 2 && eocd < 0; attempt++) {
        long tail = attempt == 0 ? 1024 : 65557;
        if (tail > fsize) tail = fsize;
        if (attempt == 1 && tail <= 1024) break;
        uint8_t *t = malloc(tail);
        if (!t) goto done;
        fseek(f, fsize - tail, SEEK_SET);
        if (fread(t, 1, tail, f) == (size_t)tail) {
            for (long i = tail - 22; i >= 0; i--)
                if (rd32(t + i) == 0x06054b50) { eocd = i; memcpy(rec, t + i, 22); break; }
        }
        free(t);
    }
    if (eocd < 0) goto done;
    uint16_t count = rd16(rec + 10);
    uint32_t cd_size = rd32(rec + 12), cd_off = rd32(rec + 16);
    if (cd_size > 4 * 1024 * 1024 || (long)cd_off + (long)cd_size > fsize) goto done;

    buf = malloc(cd_size);
    if (!buf) goto done;
    fseek(f, cd_off, SEEK_SET);
    if (fread(buf, 1, cd_size, f) != cd_size) goto done;

    for (uint32_t p = 0, i = 0; i < count && p + 46 <= cd_size; i++) {
        const uint8_t *h = buf + p;
        if (rd32(h) != 0x02014b50) break;
        uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (p + 46 + nlen > cd_size) break;
        char name[256];
        int n = nlen < 255 ? nlen : 255;
        memcpy(name, h + 46, n);
        name[n] = 0;
        uint16_t method = rd16(h + 10), flags = rd16(h + 8);
        if (n > 0 && name[n - 1] != '/' && has_ext(name, exts) && !(flags & 1) && (method == 0 || method == 8)) {
            const char *base = strrchr(name, '/');
            strncpy(out->name, base ? base + 1 : name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = 0;
            out->method = method;
            out->crc = rd32(h + 16);
            out->comp_size = rd32(h + 20);
            out->size = rd32(h + 24);
            out->local_offset = rd32(h + 42);
            found = true;
            break;
        }
        p += 46 + nlen + xlen + clen;
    }
done:
    free(buf);
    fclose(f);
    return found;
}

uint8_t *zip_extract(const char *zip_path, const zip_entry_t *e)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f) return NULL;
    uint8_t lh[30], *comp = NULL, *out = NULL;
    fseek(f, e->local_offset, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) goto fail;
    fseek(f, e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);

    out = heap_caps_malloc(e->size ? e->size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(e->size ? e->size : 1);
    if (!out) goto fail;
    if (e->method == 0) {
        if (fread(out, 1, e->size, f) != e->size) goto fail;
    } else {
        comp = heap_caps_malloc(e->comp_size ? e->comp_size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!comp) comp = malloc(e->comp_size ? e->comp_size : 1);
        if (!comp || fread(comp, 1, e->comp_size, f) != e->comp_size) goto fail;
        if (inflate_raw(comp, e->comp_size, out, e->size) != (long)e->size) goto fail;
        free(comp);
        comp = NULL;
    }
    fclose(f);
    if (crc32_calc(out, e->size) != e->crc) { free(out); return NULL; }
    return out;
fail:
    free(comp);
    free(out);
    fclose(f);
    return NULL;
}
