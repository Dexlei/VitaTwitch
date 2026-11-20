#include "text.h"

#include <string.h>

void text_clean(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)in;
    int last_space = 1;
    if (cap == 0)
        return;
    while (*p && n + 4 < cap) {
        unsigned cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); len = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            len = 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0x10000; len = 4; }
        else { p++; continue; }

        int drop = cp >= 0x10000 ||                    /* emoji and other astral chars */
                   (cp >= 0x2600 && cp < 0x2900) ||    /* misc symbols, dingbats, braille (chat ASCII art) */
                   (cp >= 0x2B00 && cp < 0x2C00) ||
                   (cp >= 0xFE00 && cp < 0xFE10) ||    /* variation selectors */
                   (cp >= 0x200B && cp < 0x2010) ||    /* zero-width chars */
                   (cp >= 0xE0000) ||
                   cp < 0x20;                          /* control chars, \n, \t */
        if (!drop) {
            int is_space = cp == ' ';
            if (!(is_space && last_space)) {        /* collapse spaces left by removed emoji */
                memcpy(out + n, p, (size_t)len);
                n += (size_t)len;
            }
            last_space = is_space;
        }
        p += len;
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}
