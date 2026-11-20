/*
 * Live HLS downloader: polls the media playlist, downloads new MPEG-TS
 * segments, demuxes them and pushes elementary-stream packets to the
 * player queues. Twitch stitched ads are skipped (like streamlink's
 * --twitch-disable-ads).
 */
#include "hls.h"
#include "adts.h"
#include "http.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>

#define LIVE_EDGE 3 /* start this many segments behind the newest one */

static int on_es(void *user, int stream, const uint8_t *data, int size,
                 int64_t pts, int64_t dts)
{
    hls_t *h = user;

    if (!h->running)
        return -1;

    if (stream == TS_STREAM_VIDEO) {
        packet_t *p = pkt_new(data, size, pts, dts);
        if (!p)
            return 0;
        return pq_put(h->vq, p);
    }

    /* Split the PES into single ADTS frames, each with its own pts. */
    int off = 0, i = 0;
    while (off < size) {
        adts_info_t ai;
        if (adts_parse(data + off, size - off, &ai) < 0) {
            off++; /* resync */
            continue;
        }
        if (ai.frame_len > size - off)
            break;
        int64_t fpts = pts;
        if (pts != TS_NOPTS)
            fpts = pts + (int64_t)i * 1024 * 90000 / ai.sample_rate;
        packet_t *p = pkt_new(data + off, ai.frame_len, fpts, fpts);
        if (p && pq_put(h->aq, p) < 0)
            return -1;
        off += ai.frame_len;
        i++;
    }
    return 0;
}

static void sleep_ms_while_running(hls_t *h, int ms)
{
    while (ms > 0 && h->running) {
        int step = ms > 50 ? 50 : ms;
        sceKernelDelayThread(step * 1000);
        ms -= step;
    }
}

static void demux_discontinuity(hls_t *h)
{
    ts_demux_discontinuity(&h->demux);
    mp4_demux_discontinuity(&h->mp4);
}

static int hls_thread(SceSize args, void *argp)
{
    hls_t *h = *(hls_t **)argp;
    CURL *c = http_new();
    buf_t pl = {0}, seg = {0};
    static m3u8_media_t media; /* large; only one downloader runs at a time */
    long long next_seq = -1;
    int fails = 0;
    long status;

    if (!c) {
        snprintf(h->error, sizeof(h->error), "No se pudo iniciar curl");
        h->finished = 1;
        return sceKernelExitDeleteThread(0);
    }

    while (h->running) {
        if (http_request(c, h->url, NULL, NULL, &pl, &status, &h->abort_req) < 0 || status != 200 ||
            m3u8_parse_media((const char *)pl.data, h->url, &media) < 0) {
            if (!h->running)
                break;
            LOGW("hls: fallo al leer la playlist (HTTP %ld, intento %d)", status, fails + 1);
            if (++fails >= 8 || status == 404 || status == 403) {
                snprintf(h->error, sizeof(h->error), "Stream terminado (HTTP %ld)", status);
                LOGE("hls: %s", h->error);
                break;
            }
            sleep_ms_while_running(h, 1000);
            continue;
        }
        fails = 0;

        if (next_seq < 0) { /* join near the live edge */
            int start = media.count > LIVE_EDGE ? media.count - LIVE_EDGE : 0;
            next_seq = media.count ? media.seg[start].seq : 0;
            LOGI("hls: playlist con %d segmentos de %.1f s, empiezo en #%lld", media.count,
                 (double)media.target_duration, next_seq);
        }
        if (media.count && media.seg[0].seq > next_seq) {
            /* fell behind the playlist window: jump forward */
            LOGW("hls: descarga demasiado lenta, salto %lld segmentos", media.seg[0].seq - next_seq);
            next_seq = media.seg[0].seq;
            demux_discontinuity(h);
        }

        int got_new = 0;
        for (int i = 0; i < media.count && h->running; i++) {
            m3u8_segment_t *s = &media.seg[i];
            if (s->seq < next_seq)
                continue;
            next_seq = s->seq + 1;
            got_new = 1;

            if (s->is_ad) {
                if (!h->in_ad)
                    LOGI("hls: empieza un anuncio, se salta");
                h->in_ad = 1;
                demux_discontinuity(h);
                continue;
            }
            if (h->in_ad)
                LOGI("hls: fin del anuncio");
            h->in_ad = 0;
            if (s->discontinuity)
                demux_discontinuity(h);

            /* fMP4 stream: (re)load the init segment when it changes */
            if (s->map >= 0 && s->map < media.nmap && strcmp(h->map_loaded, media.map_url[s->map])) {
                int iok = 0;
                for (int attempt = 0; attempt < 2 && h->running && !iok; attempt++)
                    iok = http_request(c, media.map_url[s->map], NULL, NULL, &seg, &status, &h->abort_req) == 0 &&
                          status == 200;
                if (iok && mp4_demux_set_init(&h->mp4, seg.data, seg.size) == 0) {
                    if (!h->map_loaded[0])
                        LOGI("hls: formato fMP4 (CMAF), %d pistas, H.264 %s, AAC %s", h->mp4.ntrk,
                             h->mp4.ps.size ? "si" : "no", h->mp4.aac_object ? "si" : "no");
                    snprintf(h->map_loaded, sizeof(h->map_loaded), "%s", media.map_url[s->map]);
                } else if (h->running) {
                    LOGE("hls: no se pudo leer la cabecera fMP4 (HTTP %ld, %u bytes)", status, (unsigned)seg.size);
                    continue;
                }
            }

            int ok = 0;
            for (int attempt = 0; attempt < 2 && h->running && !ok; attempt++) {
                if (http_request(c, s->url, NULL, NULL, &seg, &status, &h->abort_req) == 0 &&
                    status == 200)
                    ok = 1;
            }
            if (!ok) {
                if (h->running)
                    LOGW("hls: segmento #%lld perdido (HTTP %ld)", s->seq, status);
                demux_discontinuity(h);
                continue;
            }
            if (mp4_looks_like(seg.data, seg.size)) {
                int r = mp4_demux_feed(&h->mp4, seg.data, seg.size);
                if (r == -2) {
                    if (!h->segments_done)
                        LOGE("hls: segmento fMP4 sin cabecera (EXT-X-MAP)");
                    continue;
                }
                if (r < 0)
                    break; /* aborted */
            } else if (ts_demux_feed(&h->demux, seg.data, seg.size) < 0 ||
                       ts_demux_flush(&h->demux) < 0) {
                break; /* aborted */
            }
            h->segments_done++;
        }

        if (media.ended) {
            snprintf(h->error, sizeof(h->error), "El directo ha terminado");
            LOGI("hls: la playlist indica fin del directo");
            break;
        }
        if (!got_new)
            sleep_ms_while_running(h, (int)(media.target_duration * 500));
    }

    buf_free(&pl);
    buf_free(&seg);
    http_free(c);
    h->finished = 1;
    return sceKernelExitDeleteThread(0);
}

int hls_start(hls_t *h, const char *media_url, pktqueue_t *vq, pktqueue_t *aq)
{
    memset(h, 0, sizeof(*h));
    snprintf(h->url, sizeof(h->url), "%s", media_url);
    h->vq = vq;
    h->aq = aq;
    h->running = 1;
    ts_demux_init(&h->demux, on_es, h);
    mp4_demux_init(&h->mp4, on_es, h);

    /* curl + OpenSSL need a generous stack */
    h->thread = sceKernelCreateThread("vt_hls", hls_thread, 0x10000100, 512 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (h->thread < 0) {
        h->running = 0;
        return -1;
    }
    hls_t *self = h;
    sceKernelStartThread(h->thread, sizeof(self), &self);
    return 0;
}

void hls_stop(hls_t *h)
{
    h->running = 0;
    h->abort_req = 1;
    if (h->thread >= 0) {
        /* curl's timeout is 20 s, so the thread always ends within that; leaving
         * earlier would free the demuxer under a running thread. */
        SceUInt timeout = 25 * 1000 * 1000;
        while (!h->finished && timeout > 0) {
            sceKernelDelayThread(10000);
            timeout -= 10000;
        }
    }
    ts_demux_free(&h->demux);
    mp4_demux_free(&h->mp4);
}
