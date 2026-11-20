/*
 * Minimal MPEG-TS demuxer for Twitch HLS segments: H.264 (stream type 0x1B)
 * and AAC ADTS (0x0F). Timestamps are rebased so they stay monotonic across
 * HLS discontinuities and 33-bit wraparound.
 */
#include "ts_demux.h"

#define TS_PKT 188
#define MAX_JUMP (10LL * 90000) /* >10 s jump => new timeline */
#define FRAME_GAP 3003          /* ~1 frame at 29.97 fps, used to stitch timelines */

void ts_demux_init(ts_demux_t *d, ts_es_cb cb, void *user)
{
    memset(d, 0, sizeof(*d));
    d->pmt_pid = -1;
    d->pid[0] = d->pid[1] = -1;
    d->aud_mode = -1;
    d->cb = cb;
    d->user = user;
}

void ts_demux_free(ts_demux_t *d)
{
    buf_free(&d->pes[0]);
    buf_free(&d->pes[1]);
    buf_free(&d->ves);
}

static int has_aud(const uint8_t *p, int n)
{
    for (int i = 0; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 9)
            return 1;
    return 0;
}

/* Appends one video PES payload and emits every complete access unit. */
static int video_append(ts_demux_t *d, const uint8_t *data, int size, int64_t pts, int64_t dts)
{
    if (d->aud_mode < 0)
        d->aud_mode = has_aud(data, size);
    if (!d->aud_mode)
        return d->cb ? d->cb(d->user, TS_STREAM_VIDEO, data, size, pts, dts) : 0;

    if (d->ves.size == 0) {
        d->au_pts = pts;
        d->au_dts = dts;
    } else if (pts != TS_NOPTS && d->nvts < (int)(sizeof(d->vts) / sizeof(d->vts[0]))) {
        /* PTS applies to the first AU that starts inside this PES */
        d->vts[d->nvts].off = d->ves.size;
        d->vts[d->nvts].pts = pts;
        d->vts[d->nvts].dts = dts;
        d->nvts++;
    }
    if (buf_append(&d->ves, data, (size_t)size) < 0)
        return 0;

    size_t i = d->ves_scan;
    while (i + 4 <= d->ves.size) {
        uint8_t *b = d->ves.data;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 0x1F) == 9) {
            size_t start = (i > 0 && b[i - 1] == 0) ? i - 1 : i;
            if (start > 0) {
                if (d->cb) {
                    int r = d->cb(d->user, TS_STREAM_VIDEO, b, (int)start, d->au_pts, d->au_dts);
                    if (r < 0)
                        return r;
                }
                int64_t np = TS_NOPTS, nd = TS_NOPTS;
                int w = 0;
                for (int k = 0; k < d->nvts; k++) {
                    if (d->vts[k].off <= start) {
                        np = d->vts[k].pts;
                        nd = d->vts[k].dts;
                    } else {
                        d->vts[w] = d->vts[k];
                        d->vts[w].off -= start;
                        w++;
                    }
                }
                d->nvts = w;
                d->au_pts = np;
                d->au_dts = nd;
                memmove(b, b + start, d->ves.size - start);
                d->ves.size -= start;
                i -= start;
            }
            i += 4;
        } else {
            i++;
        }
    }
    d->ves_scan = i;
    return 0;
}

static int video_flush(ts_demux_t *d)
{
    int r = 0;
    if (d->ves.size > 0 && d->cb)
        r = d->cb(d->user, TS_STREAM_VIDEO, d->ves.data, (int)d->ves.size, d->au_pts, d->au_dts);
    buf_reset(&d->ves);
    d->ves_scan = 0;
    d->nvts = 0;
    return r;
}

void ts_demux_discontinuity(ts_demux_t *d)
{
    d->disc_pending = 1;
}

static int64_t read_ts(const uint8_t *p)
{
    return ((int64_t)((p[0] >> 1) & 0x07) << 30) |
           ((int64_t)p[1] << 22) |
           ((int64_t)(p[2] >> 1) << 15) |
           ((int64_t)p[3] << 7) |
           ((int64_t)p[4] >> 1);
}

static void fix_timestamps(ts_demux_t *d, int64_t *pts, int64_t *dts)
{
    if (*pts == TS_NOPTS)
        return;
    *pts += d->offset;
    *dts += d->offset;
    if (d->have_last) {
        int64_t diff = *pts - d->last_pts;
        if (d->disc_pending || diff > MAX_JUMP || diff < -MAX_JUMP) {
            int64_t adj = (d->last_pts + FRAME_GAP) - *pts;
            d->offset += adj;
            *pts += adj;
            *dts += adj;
        }
    }
    d->disc_pending = 0;
    if (!d->have_last || *pts > d->last_pts)
        d->last_pts = *pts;
    d->have_last = 1;
}

static int emit_pes(ts_demux_t *d, int idx)
{
    buf_t *b = &d->pes[idx];
    int ret = 0;

    if (!d->pes_active[idx])
        return 0;
    d->pes_active[idx] = 0;

    if (b->size < 9 || b->data[0] != 0 || b->data[1] != 0 || b->data[2] != 1)
        goto out;

    {
        const uint8_t *p = b->data;
        int flags = p[7];
        int hdr_len = p[8];
        int64_t pts = TS_NOPTS, dts = TS_NOPTS;
        size_t start = 9 + (size_t)hdr_len;

        if (start > b->size)
            goto out;
        if ((flags & 0x80) && hdr_len >= 5)
            pts = dts = read_ts(p + 9);
        if ((flags & 0x40) && hdr_len >= 10)
            dts = read_ts(p + 14);

        fix_timestamps(d, &pts, &dts);
        if (b->size > start) {
            if (idx == TS_STREAM_VIDEO)
                ret = video_append(d, p + start, (int)(b->size - start), pts, dts);
            else if (d->cb)
                ret = d->cb(d->user, idx, p + start, (int)(b->size - start), pts, dts);
        }
    }
out:
    buf_reset(b);
    return ret;
}

static void parse_pat(ts_demux_t *d, const uint8_t *p, int len)
{
    if (len < 1)
        return;
    int ptr = p[0];
    p += 1 + ptr;
    len -= 1 + ptr;
    if (len < 8 || p[0] != 0x00)
        return;
    int sec_len = ((p[1] & 0x0F) << 8) | p[2];
    int end = 3 + sec_len - 4; /* exclude CRC */
    if (end > len)
        end = len;
    for (int i = 8; i + 4 <= end; i += 4) {
        int prog = (p[i] << 8) | p[i + 1];
        int pid = ((p[i + 2] & 0x1F) << 8) | p[i + 3];
        if (prog != 0) {
            d->pmt_pid = pid;
            return;
        }
    }
}

static void parse_pmt(ts_demux_t *d, const uint8_t *p, int len)
{
    if (len < 1)
        return;
    int ptr = p[0];
    p += 1 + ptr;
    len -= 1 + ptr;
    if (len < 12 || p[0] != 0x02)
        return;
    int sec_len = ((p[1] & 0x0F) << 8) | p[2];
    int end = 3 + sec_len - 4;
    if (end > len)
        end = len;
    int pil = ((p[10] & 0x0F) << 8) | p[11];
    int i = 12 + pil;
    while (i + 5 <= end) {
        int type = p[i];
        int pid = ((p[i + 1] & 0x1F) << 8) | p[i + 2];
        int eslen = ((p[i + 3] & 0x0F) << 8) | p[i + 4];
        if (type == 0x1B && d->pid[TS_STREAM_VIDEO] < 0)
            d->pid[TS_STREAM_VIDEO] = pid;
        else if (type == 0x0F && d->pid[TS_STREAM_AUDIO] < 0)
            d->pid[TS_STREAM_AUDIO] = pid;
        i += 5 + eslen;
    }
}

static int handle_packet(ts_demux_t *d, const uint8_t *p)
{
    int pusi = p[1] & 0x40;
    int pid = ((p[1] & 0x1F) << 8) | p[2];
    int afc = (p[3] >> 4) & 3;
    int off = 4;

    if (afc & 2)
        off += 1 + p[4];
    if (!(afc & 1) || off >= TS_PKT)
        return 0;

    const uint8_t *pl = p + off;
    int len = TS_PKT - off;

    if (pid == 0) {
        if (pusi)
            parse_pat(d, pl, len);
        return 0;
    }
    if (pid == d->pmt_pid) {
        if (pusi)
            parse_pmt(d, pl, len);
        return 0;
    }

    for (int idx = 0; idx < 2; idx++) {
        if (pid != d->pid[idx])
            continue;
        if (pusi) {
            int r = emit_pes(d, idx);
            if (r < 0)
                return r;
            d->pes_active[idx] = 1;
            buf_reset(&d->pes[idx]);
        }
        if (d->pes_active[idx])
            buf_append(&d->pes[idx], pl, (size_t)len);
        return 0;
    }
    return 0;
}

int ts_demux_feed(ts_demux_t *d, const uint8_t *data, size_t len)
{
    size_t i = 0;

    /* complete a packet left over from the previous call */
    if (d->carry_len > 0) {
        size_t need = TS_PKT - (size_t)d->carry_len;
        if (len < need) {
            memcpy(d->carry + d->carry_len, data, len);
            d->carry_len += (int)len;
            return 0;
        }
        memcpy(d->carry + d->carry_len, data, need);
        d->carry_len = 0;
        i = need;
        if (d->carry[0] == 0x47) {
            int r = handle_packet(d, d->carry);
            if (r < 0)
                return r;
        }
    }

    while (i < len) {
        if (data[i] != 0x47) { /* resync */
            i++;
            continue;
        }
        if (len - i < TS_PKT) {
            d->carry_len = (int)(len - i);
            memcpy(d->carry, data + i, (size_t)d->carry_len);
            break;
        }
        int r = handle_packet(d, data + i);
        if (r < 0)
            return r;
        i += TS_PKT;
    }
    return 0;
}

int ts_demux_flush(ts_demux_t *d)
{
    d->carry_len = 0;
    for (int idx = 0; idx < 2; idx++) {
        int r = emit_pes(d, idx);
        if (r < 0)
            return r;
    }
    return video_flush(d);
}
