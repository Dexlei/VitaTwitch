#ifndef VT_TEXT_H
#define VT_TEXT_H

#include <stddef.h>

/* Removes what the Vita's PGF font cannot draw (emoji, dingbats, zero-width
 * chars, control chars) and collapses the spaces left behind. */
void text_clean(const char *in, char *out, size_t cap);

#endif
