/* i18n.h - menu language (English / Serbian Latin).
 *
 * T("English text") returns the text in the language picked in Settings.
 * Serbian letters come back as the font's single-byte glyph codes
 * (0x84..0x8D), so strlen() still equals the number of characters on screen.
 * Untranslated text is returned unchanged.
 */
#pragma once
#include <stddef.h>

enum { LANG_EN, LANG_SR, LANG_COUNT };

const char *T(const char *en);
/* Picks the right plural form: Tn(n, "%d game", "%d games").
 * Serbian has three forms; the "few" one (2-4) is looked up as "<many>#2". */
const char *Tn(int n, const char *one, const char *many);
const char *lang_name(int lang);
/* Converts device text (with glyph codes) to UTF-8 for the web UI. */
void i18n_to_utf8(const char *in, char *out, size_t n);
