/* i18n.c - see i18n.h. The table lives in lang/sr.txt (tools/gen_i18n.py). */
#include <string.h>
#include <stdlib.h>
#include "i18n.h"
#include "i18n_sr.h"
#include "system.h"

static int cmp(const void *key, const void *entry)
{
    return strcmp((const char *)key, ((const char *const *)entry)[0]);
}

static const char *lookup(const char *en)
{
    const char *const *e = bsearch(en, i18n_sr, I18N_SR_COUNT, sizeof(i18n_sr[0]), cmp);
    return e ? e[1] : NULL;
}

const char *T(const char *en)
{
    if (!en || g_settings.lang != LANG_SR) return en;
    const char *s = lookup(en);
    return s ? s : en;
}

const char *Tn(int n, const char *one, const char *many)
{
    if (g_settings.lang != LANG_SR) return n == 1 ? one : many;
    int d = n % 10, h = n % 100;
    if (d == 1 && h != 11) return T(one);
    if (d >= 2 && d <= 4 && (h < 12 || h > 14)) {
        char key[64];
        size_t l = strlen(many);
        if (l + 3 < sizeof(key)) {
            memcpy(key, many, l);
            memcpy(key + l, "#2", 3);
            const char *s = lookup(key);
            if (s) return s;
        }
    }
    return T(many);
}

const char *lang_name(int lang)
{
    return lang == LANG_SR ? "Srpski" : "English";
}

void i18n_to_utf8(const char *in, char *out, size_t n)
{
    /* UTF-8 for č ć đ š ž Č Ć Đ Š Ž, in glyph-code order (0x84..0x8D). */
    static const char *const u[10] = { "\xc4\x8d", "\xc4\x87", "\xc4\x91", "\xc5\xa1", "\xc5\xbe",
                                       "\xc4\x8c", "\xc4\x86", "\xc4\x90", "\xc5\xa0", "\xc5\xbd" };
    size_t o = 0;
    if (!n) return;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 0x84 && c <= 0x8d) {
            if (o + 2 >= n) break;
            out[o++] = u[c - 0x84][0];
            out[o++] = u[c - 0x84][1];
        } else {
            if (o + 1 >= n) break;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}
