#pragma once
#include "common.h"

/* UI languages. The English text is the key: tr() returns its translation in the chosen language, or the text itself.
 * Translations live in resources/lang/<code>.txt, packed into the program at build time (cmake/lang.cmake). */
void           i18n_set(const wchar_t *code);    /* "" = the system's language */
const wchar_t *tr(const wchar_t *en);            /* NULL stays NULL */
int            i18n_count(void);                 /* languages besides English */
const wchar_t *i18n_code(int i);                 /* "de" … */
const wchar_t *i18n_name(int i);                 /* "Deutsch" … */
int            i18n_current(void);               /* index in use, -1 = English */
