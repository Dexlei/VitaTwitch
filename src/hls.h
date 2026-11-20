#ifndef VT_HLS_H
#define VT_HLS_H

#include <psp2/types.h>
#include "m3u8.h"
#include "pktqueue.h"
#include "ts_demux.h"

typedef struct {
    char url[M3U8_URL_MAX];   /* media playlist */
    pktqueue_t *vq, *aq;      /* outputs: H.264 AUs, single ADTS frames */

    volatile int running;
    volatile int abort_req;   /* non-zero cancels in-flight HTTP requests */
    volatile int in_ad;       /* currently skipping Twitch ad segments */
    volatile int finished;    /* stream ended or failed */
    volatile int segments_done;
    char error[128];

    SceUID thread;
    ts_demux_t demux;
} hls_t;

int hls_start(hls_t *h, const char *media_url, pktqueue_t *vq, pktqueue_t *aq);
void hls_stop(hls_t *h);

#endif
