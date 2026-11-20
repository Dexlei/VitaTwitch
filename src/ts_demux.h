#ifndef VT_TS_DEMUX_H
#define VT_TS_DEMUX_H

#include <stdint.h>
#include "buf.h"

#define TS_NOPTS INT64_MIN

enum { TS_STREAM_VIDEO = 0, TS_STREAM_AUDIO = 1 };

/* Called once per complete PES payload: an H.264 access unit (Annex B) for
 * video, or one or more ADTS AAC frames for audio. Timestamps are 90 kHz and
 * already made continuous across discontinuities. Returns <0 to abort. */
typedef int (*ts_es_cb)(void *user, int stream, const uint8_t *data, int size,
                        int64_t pts, int64_t dts);

typedef struct {
    int pmt_pid;
    int pid[2];          /* video / audio PID, -1 if unknown */
    buf_t pes[2];
    int pes_active[2];

    uint8_t carry[188];  /* partial TS packet between feeds */
    int carry_len;

    /* H.264 access-unit reassembly: Twitch's transcodes do not align PES
     * packets with frames, so AUs are split on AUD NAL units instead. */
    int aud_mode;        /* -1 unknown, 0 PES-aligned fallback, 1 split on AUD */
    buf_t ves;           /* pending video elementary stream */
    size_t ves_scan;
    int64_t au_pts, au_dts;
    struct { size_t off; int64_t pts, dts; } vts[32];
    int nvts;

    /* timestamp continuity */
    int64_t offset;
    int64_t last_pts;
    int have_last;
    int disc_pending;

    ts_es_cb cb;
    void *user;
} ts_demux_t;

void ts_demux_init(ts_demux_t *d, ts_es_cb cb, void *user);
void ts_demux_free(ts_demux_t *d);
/* Feed raw TS bytes (any chunking). Returns <0 if the callback aborted. */
int ts_demux_feed(ts_demux_t *d, const uint8_t *data, size_t len);
/* Emit pending PES packets (call at the end of each HLS segment). */
int ts_demux_flush(ts_demux_t *d);
/* Next timestamps belong to a new timeline (EXT-X-DISCONTINUITY, skipped ads). */
void ts_demux_discontinuity(ts_demux_t *d);

#endif
