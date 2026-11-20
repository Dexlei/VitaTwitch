#ifndef VT_ADTS_H
#define VT_ADTS_H

#include <stdint.h>

typedef struct {
    int sample_rate;
    int channels;
    int frame_len;   /* whole frame incl. header */
} adts_info_t;

static const int adts_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

/* Parses an ADTS header at p. Returns 0 on success. */
static inline int adts_parse(const uint8_t *p, int avail, adts_info_t *out)
{
    if (avail < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return -1;
    int sf = (p[2] >> 2) & 0x0F;
    int ch = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    int len = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);
    if (adts_rates[sf] == 0 || len < 7)
        return -1;
    out->sample_rate = adts_rates[sf];
    out->channels = ch ? ch : 2;
    out->frame_len = len;
    return 0;
}

#endif
