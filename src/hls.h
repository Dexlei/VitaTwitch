#ifndef VT_HLS_H
#define VT_HLS_H

#include <psp2/types.h>
#include "m3u8.h"
#include "pktqueue.h"
#include "ts_demux.h"
#include "mp4_demux.h"

typedef struct {
    char url[M3U8_URL_MAX];   /* media playlist */
    pktqueue_t *vq, *aq;      /* outputs: H.264 AUs, single ADTS frames */

    volatile int running;
    volatile int abort_req;   /* non-zero cancels in-flight HTTP requests */
    volatile int in_ad;       /* currently skipping Twitch ad segments */
    volatile int finished;    /* stream ended or failed */
    volatile int segments_done;
    volatile int retryable;
    volatile unsigned download_kbps;
    volatile unsigned segment_losses;
    int live_edge;
    char error[128];

    SceUID thread;
    ts_demux_t demux;
    mp4_demux_t mp4;                  /* fMP4 streams (#EXT-X-MAP), e.g. Enhanced Broadcasting */
    char map_loaded[M3U8_URL_MAX];    /* init segment currently in mp4 */
} hls_t;

int hls_start(hls_t *h, const char *media_url, pktqueue_t *vq, pktqueue_t *aq, int live_edge);
void hls_stop(hls_t *h);

#endif
