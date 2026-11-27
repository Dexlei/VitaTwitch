/*
 * HLS player: hardware H.264 (sceAvcdec) + hardware AAC (sceAudiodec).
 *
 *  hls thread -> video queue -> decode thread (core 1): sceAvcdecDecode -> ring of RGBA textures
 *                                                  -> main thread: picks the due frame, vita2d
 *             -> audio queue -> audio thread (core 2): sceAudiodecDecode -> sceAudioOut
 *
 * The audio thread drives the master clock; frames are presented when the
 * clock reaches their pts (late ones are dropped, early ones wait in the ring).
 * The decoders are used directly: the system SceAvPlayer cannot open HLS URLs
 * (NetStream ships a modified WebMAF build of it, not usable here).
 */
#include "player.h"
#include "adts.h"
#include "chat.h"
#include "chat_view.h"
#include "hls.h"
#include "images.h"
#include "log.h"
#include "pktqueue.h"
#include "ts_demux.h"
#include "ui.h"
#include "i18n.h"
#include "ime_utf8.h"
#include "playback_policy.h"
#include <psp2/power.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/videodec.h>

#define SCREEN_W 960
#define CHAT_PANEL_W 320
#define SCREEN_H 544
#define ALIGN16(x) (((x) + 15) & ~15)
#define AUDIO_GRAIN 1024
#define PTS_PER_MS 90

#define COLOR_WHITE  RGBA8(255, 255, 255, 255)
#define COLOR_PURPLE RGBA8(145, 70, 255, 255)
#define COLOR_GREY   RGBA8(180, 180, 190, 255)
#define COLOR_OSD_BG RGBA8(0, 0, 0, 170)
#define COLOR_LIVE   RGBA8(235, 4, 0, 255)

/* ------------------------------------------------------------------ clock */

typedef struct {
    pthread_mutex_t lock;
    int valid;
    int64_t pts;     /* 90 kHz */
    uint64_t time;   /* sceKernelGetProcessTimeWide() when pts was set */
    int64_t cap_us;  /* max extrapolation (freezes the clock on audio stalls) */
} av_clock_t;

static void clock_set(av_clock_t *c, int64_t pts, int64_t cap_us)
{
    pthread_mutex_lock(&c->lock);
    c->pts = pts;
    c->time = sceKernelGetProcessTimeWide();
    c->cap_us = cap_us;
    c->valid = 1;
    pthread_mutex_unlock(&c->lock);
}

static int64_t clock_get(av_clock_t *c)
{
    int64_t r = TS_NOPTS;
    pthread_mutex_lock(&c->lock);
    if (c->valid) {
        int64_t el = (int64_t)(sceKernelGetProcessTimeWide() - c->time);
        if (el > c->cap_us)
            el = c->cap_us;
        r = c->pts + el * 9 / 100;
    }
    pthread_mutex_unlock(&c->lock);
    return r;
}

/* ----------------------------------------------------------------- player */

typedef struct {
    pktqueue_t vq, aq;
    hls_t hls;
    av_clock_t clock;
    int profile;
    volatile int volume;
    int freerun;                /* no audio: clock runs on wall time */

    volatile int audio_running;
    volatile int audio_done;
    volatile int audio_buffering;
    volatile int audio_failed;
    volatile int rebuffers;     /* times audio ran dry and had to buffer again */
    SceUID audio_thread;
} player_t;

static player_t g_pl;

/* ------------------------------------------------------------ audio thread */

static int audio_thread(SceSize args, void *argp)
{
    player_t *pl = *(player_t **)argp;
    static int16_t silence[AUDIO_GRAIN * 2];
    static int16_t accum[AUDIO_GRAIN * 2 * 4];
    int accum_frames = 0;
    int64_t accum_pts = TS_NOPTS;

    int port = -1, rate = 0, chans = 0, current_volume=-1;
    int lib_init = 0, dec_ok = 0, prebuffer = 1, underflows = 0, audio_errors = 0;
    SceAudiodecCtrl ctrl;
    SceAudiodecInfo info;
    uint8_t *es = NULL;
    int16_t *pcm = NULL;

    memset(silence, 0, sizeof(silence));

    while (pl->audio_running) {
        if (prebuffer) {
            /* wait for ~1 s of audio before (re)starting output */
            if (pq_span(&pl->aq) >= vt_profile_buffer_ms(pl->profile,pl->rebuffers) * PTS_PER_MS ||
                (pl->hls.finished && pq_count(&pl->aq) > 0)) {
                prebuffer = 0;
            } else {
                pl->audio_buffering = 1;
                if (port >= 0)
                    sceAudioOutOutput(port, silence);
                else
                    sceKernelDelayThread(10000);
                continue;
            }
        }

        packet_t *p = pq_get(&pl->aq, 20);
        if (!p) {
            if (port >= 0)
                sceAudioOutOutput(port, silence);
            if (++underflows > 10 && !prebuffer) {
                prebuffer = 1;
                pl->rebuffers++;
            }
            continue;
        }
        underflows = 0;

        adts_info_t ai;
        if (adts_parse(p->data, p->size, &ai) < 0 || ai.channels > 2) {
            pkt_free(p);
            continue;
        }

        if (!dec_ok || ai.sample_rate != rate || ai.channels != chans) {
            if (dec_ok) {
                sceAudiodecDeleteDecoder(&ctrl);
                dec_ok = 0;
            }
            if (!lib_init) {
                SceAudiodecInitParam init;
                memset(&init, 0, sizeof(init));
                init.size = sizeof(init.aac);
                init.aac.totalStreams = 1;
                int ir = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &init);
                if (ir < 0) {
                    LOGE("audio: sceAudiodecInitLibrary 0x%08X, sigo sin sonido", (unsigned)ir);
                    pl->audio_failed = 1;
                    pkt_free(p);
                    break;
                }
                lib_init = 1;
            }
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info.aac);
            info.aac.isAdts = 1;
            info.aac.ch = ai.channels;
            info.aac.samplingRate = ai.sample_rate;
            info.aac.isSbr = 0;
            memset(&ctrl, 0, sizeof(ctrl));
            ctrl.size = sizeof(ctrl);
            ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
            ctrl.pInfo = &info;
            int cr = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AAC);
            if (cr < 0) {
                LOGE("audio: sceAudiodecCreateDecoder 0x%08X (%d Hz, %d canales), sigo sin sonido",
                     (unsigned)cr, ai.sample_rate, ai.channels);
                pl->audio_failed = 1;
                pkt_free(p);
                break;
            }
            dec_ok = 1;
            free(es);
            free(pcm);
            es = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxEsSize));
            pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxPcmSize));

            SceAudioOutMode mode = ai.channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
            if (port < 0) {
                port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, ai.sample_rate, mode);
                if (port >= 0) {
                    current_volume=pl->volume;
                    int vol[2] = { SCE_AUDIO_VOLUME_0DB*current_volume/100, SCE_AUDIO_VOLUME_0DB*current_volume/100 };
                    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
                }
            } else {
                sceAudioOutSetConfig(port, AUDIO_GRAIN, ai.sample_rate, mode);
            }
            if (port < 0 || !es || !pcm) {
                LOGE("audio: no se pudo abrir la salida (puerto 0x%08X, memoria %s)", (unsigned)port,
                     es && pcm ? "ok" : "sin memoria");
                pl->audio_failed = 1;
                pkt_free(p);
                break;
            }
            LOGI("audio: AAC %d Hz, %d canales", ai.sample_rate, ai.channels);
            rate = ai.sample_rate;
            chans = ai.channels;
            accum_frames = 0;
        }

        if ((uint32_t)p->size > ctrl.maxEsSize) {
            pkt_free(p);
            continue;
        }
        memcpy(es, p->data, (size_t)p->size);
        ctrl.pEs = es;
        ctrl.pPcm = pcm;
        int r = sceAudiodecDecode(&ctrl);
        int64_t pts = p->pts;
        pkt_free(p);
        if (r < 0) {
            if (++audio_errors <= 5)
                LOGW("audio: error al decodificar AAC 0x%08X", (unsigned)r);
            sceAudiodecClearContext(&ctrl);
            continue;
        }

        int frames = (int)(ctrl.outputPcmSize / (sizeof(int16_t) * (unsigned)chans));
        int room = (int)(sizeof(accum) / sizeof(int16_t)) / chans - accum_frames;
        if (frames > room)
            frames = room;
        if (accum_frames == 0)
            accum_pts = pts;
        memcpy(accum + accum_frames * chans, pcm, (size_t)frames * chans * sizeof(int16_t));
        accum_frames += frames;

        int64_t grain_dur = (int64_t)AUDIO_GRAIN * 90000 / rate;
        while (accum_frames >= AUDIO_GRAIN && pl->audio_running) {
            if(current_volume!=pl->volume){
                current_volume=pl->volume;
                int v=SCE_AUDIO_VOLUME_0DB*current_volume/100;
                int vol[2]={v,v};
                sceAudioOutSetVolume(port,SCE_AUDIO_VOLUME_FLAG_L_CH|SCE_AUDIO_VOLUME_FLAG_R_CH,vol);
            }
            sceAudioOutOutput(port, accum);
            pl->audio_buffering = 0;
            /* the previous grain is what is audible right now */
            if (accum_pts != TS_NOPTS)
                clock_set(&pl->clock, accum_pts - grain_dur, grain_dur * 100 / 9 * 2);
            accum_frames -= AUDIO_GRAIN;
            memmove(accum, accum + AUDIO_GRAIN * chans, (size_t)accum_frames * chans * sizeof(int16_t));
            if (accum_pts != TS_NOPTS)
                accum_pts += grain_dur;
        }
    }

    if (port >= 0)
        sceAudioOutReleasePort(port);
    if (dec_ok)
        sceAudiodecDeleteDecoder(&ctrl);
    if (lib_init)
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
    free(es);
    free(pcm);
    pl->audio_done = 1;
    return sceKernelExitDeleteThread(0);
}

/* ---------------------------------------------------------- video decoder */

typedef struct {
    int lib_init;
    SceUID memblock;
    SceAvcdecCtrl ctrl;
    int created;
    int w, h;             /* decoder / texture size (16-aligned) */
    unsigned pitch;       /* texture pitch in pixels */
} vdec_t;

static void vdec_close(vdec_t *v)
{
    if (v->created)
        sceAvcdecDeleteDecoder(&v->ctrl);
    if (v->memblock >= 0)
        sceKernelFreeMemBlock(v->memblock);
    if (v->lib_init)
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    memset(v, 0, sizeof(*v));
    v->memblock = -1;
}

static int vdec_open(vdec_t *v, int w, int h, char *err, int errlen)
{
    static const int refs_try[] = { 6, 4, 3 };
    int ret = -1;

    memset(v, 0, sizeof(*v));
    v->memblock = -1;
    v->w = ALIGN16(w > 0 ? w : 1280);
    v->h = ALIGN16(h > 0 ? h : 720);

    for (unsigned i = 0; i < sizeof(refs_try) / sizeof(refs_try[0]); i++) {
        SceVideodecQueryInitInfoHwAvcdec init;
        memset(&init, 0, sizeof(init));
        init.size = sizeof(init);
        init.horizontal = v->w;
        init.vertical = v->h;
        init.numOfRefFrames = refs_try[i];
        init.numOfStreams = 1;
        ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
        if (ret < 0)
            continue;
        v->lib_init = 1;

        SceAvcdecQueryDecoderInfo q;
        SceAvcdecDecoderInfo di;
        memset(&q, 0, sizeof(q));
        memset(&di, 0, sizeof(di));
        q.horizontal = init.horizontal;
        q.vertical = init.vertical;
        q.numOfRefFrames = init.numOfRefFrames;
        ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
        if (ret >= 0) {
            SceSize sz = (di.frameMemSize + 0xFFFFF) & ~0xFFFFF;
            v->memblock = sceKernelAllocMemBlock("vt_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
            if (v->memblock >= 0) {
                sceKernelGetMemBlockBase(v->memblock, &v->ctrl.frameBuf.pBuf);
                v->ctrl.frameBuf.size = sz;
                ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->ctrl, &q);
                if (ret >= 0) {
                    v->created = 1;
                    break;
                }
                sceKernelFreeMemBlock(v->memblock);
                v->memblock = -1;
            } else {
                ret = v->memblock;
            }
        }
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        v->lib_init = 0;
    }
    if (!v->created) {
        snprintf(err, (size_t)errlen, vt_msg("No se pudo iniciar el decodificador H.264 (0x%08X)"), (unsigned)ret);
        LOGE("video: %s, %dx%d", err, v->w, v->h);
        vdec_close(v);
        return -1;
    }
    LOGI("video: decodificador H.264 %dx%d listo", v->w, v->h);
    return 0;
}

/* Looks at the first slice of an access unit: keyframe (IDR) and/or a frame no
 * other frame references (nal_ref_idc == 0), which can be skipped safely. */
static void au_info(const uint8_t *d, int n, int *idr, int *disposable)
{
    *idr = *disposable = 0;
    for (int i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int type = d[i + 3] & 0x1F, ref = (d[i + 3] >> 5) & 3;
            if (type == 5) {
                *idr = 1;
                return;
            }
            if (type == 1) {
                *disposable = ref == 0;
                return;
            }
            i += 3;
        }
    }
}

/* ------------------------------------------------- decode thread + frame ring
 *
 * The decoder runs on its own thread (core 1) and writes pictures into a ring
 * of textures; the main thread only picks the frame that is due on the audio
 * clock and draws it. Before, decoding happened on the drawing thread straight
 * into the one texture on screen, so any stall in drawing (emotes, chat panel,
 * GPU waits) made the video late and it never caught up.
 */

#define MAX_SLOTS 8
#define DEC_PICS  3   /* an IDR can make the decoder output several pictures at once */

enum { SLOT_FREE, SLOT_DECODING, SLOT_READY, SLOT_SHOWN };

typedef struct {
    vita2d_texture *tex;
    int64_t pts;
    int w, h;
    int state;
} vslot_t;

typedef struct {
    vdec_t *v;
    const player_params_t *pp;
    vslot_t slot[MAX_SLOTS];
    int nslot;
    int ready[MAX_SLOTS];        /* READY slots in output (display) order */
    int nready;
    pthread_mutex_t lock;
    volatile int running, done;
    volatile int shown_any;      /* lag checks start once something is on screen */
    volatile int fatal;
    char fatal_msg[128];
    SceUID thread;

    /* stats, reset by the presenter every interval */
    volatile int st_aus, st_pics, st_multi, st_errors, st_undecoded, st_chat_skip, st_idr_jumps;
    volatile uint64_t st_dec_us, st_dec_max;
    volatile int64_t st_max_lag;
    volatile int total_pics, total_errors;
} vpipe_t;

static vpipe_t g_vp;

static int64_t pic_pts(const SceAvcdecPicture *pic)
{
    if (pic->info.pts.upper == 0xFFFFFFFF && pic->info.pts.lower == 0xFFFFFFFF)
        return TS_NOPTS;
    return (int64_t)(((uint64_t)pic->info.pts.upper << 32) | pic->info.pts.lower);
}

static int vdec_thread(SceSize args, void *argp)
{
    player_t *pl = &g_pl;
    vpipe_t *vp = &g_vp;
    vdec_t *v = vp->v;
    int skip_to_idr = 0, consecutive_err = 0;
    int elm = DEC_PICS; /* pictures offered per call; 1 if the firmware rejects more */
    (void)args; (void)argp;

    while (vp->running) {
        /* reserve DEC_PICS free textures (back-pressure when the ring is full) */
        int fr[DEC_PICS], nf = 0;
        pthread_mutex_lock(&vp->lock);
        for (int i = 0; i < vp->nslot && nf < DEC_PICS; i++)
            if (vp->slot[i].state == SLOT_FREE)
                fr[nf++] = i;
        if (nf == DEC_PICS)
            for (int k = 0; k < nf; k++)
                vp->slot[fr[k]].state = SLOT_DECODING;
        pthread_mutex_unlock(&vp->lock);
        if (nf < DEC_PICS) {
            sceKernelDelayThread(2000);
            continue;
        }

        packet_t *p = pq_get(&pl->vq, 20);
        int skip = 0;
        if (p) {
            int idr = 0, disposable = 0;
            au_info(p->data, p->size, &idr, &disposable);
            /* lag of the AU's decode time against the audio clock (dts: AUs arrive
             * in decode order; pts runs ahead and hid the real lag) */
            int64_t clk = clock_get(&pl->clock);
            int64_t lag = 0;
            if (clk != TS_NOPTS && p->dts != TS_NOPTS && vp->shown_any)
                lag = clk - p->dts;
            if (lag < 0 || lag > 60000 * PTS_PER_MS)
                lag = 0;
            if (lag > vp->st_max_lag)
                vp->st_max_lag = lag;
            int chat = vp->pp->chat_on && *vp->pp->chat_on;

            if (skip_to_idr && !idr) {
                skip = 1;
                vp->st_undecoded++;
            } else if (skip_to_idr) {
                skip_to_idr = 0;
                LOGI("video: reanudo en keyframe");
            } else if (lag > 1000 * PTS_PER_MS && !idr) {
                skip_to_idr = 1;
                skip = 1;
                vp->st_idr_jumps++;
                vp->st_undecoded++;
                LOGW("video: %.1f s de retraso, salto al siguiente keyframe", lag / PTS_PER_MS / 1000.0);
            } else if (lag > 150 * PTS_PER_MS && disposable) {
                skip = 1;
                vp->st_undecoded++;
            } else if (disposable && chat && vp->pp->fps > 40 && lag > 80 * PTS_PER_MS) {
                /* chat visible on a 60 fps stream: unreferenced frames (about half
                 * on Twitch) are not decoded, halving the decoder's work */
                skip = 1;
                vp->st_chat_skip++;
            }
        }
        if (!p || skip) {
            if (p)
                pkt_free(p);
            pthread_mutex_lock(&vp->lock);
            for (int k = 0; k < nf; k++)
                vp->slot[fr[k]].state = SLOT_FREE;
            pthread_mutex_unlock(&vp->lock);
            continue;
        }

        SceAvcdecAu au;
        SceAvcdecArrayPicture arr;
        SceAvcdecPicture pic[DEC_PICS];
        SceAvcdecPicture *pics[DEC_PICS];
        memset(&au, 0, sizeof(au));
        memset(&arr, 0, sizeof(arr));
        memset(pic, 0, sizeof(pic));
        for (int k = 0; k < DEC_PICS; k++) {
            pic[k].size = sizeof(pic[k]);
            pic[k].frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
            pic[k].frame.framePitch = v->pitch;
            pic[k].frame.frameWidth = v->w;
            pic[k].frame.frameHeight = v->h;
            pic[k].frame.pPicture[0] = vita2d_texture_get_datap(vp->slot[fr[k]].tex);
            pics[k] = &pic[k];
        }
        arr.numOfElm = (uint32_t)elm;
        arr.pPicture = pics;
        au.es.pBuf = p->data;
        au.es.size = (uint32_t)p->size;
        if (p->pts != TS_NOPTS) {
            au.pts.upper = (uint32_t)((uint64_t)p->pts >> 32);
            au.pts.lower = (uint32_t)p->pts;
        } else {
            au.pts.upper = au.pts.lower = 0xFFFFFFFF;
        }
        if (p->dts != TS_NOPTS) {
            au.dts.upper = (uint32_t)((uint64_t)p->dts >> 32);
            au.dts.lower = (uint32_t)p->dts;
        } else {
            au.dts.upper = au.dts.lower = 0xFFFFFFFF;
        }

        uint64_t t0 = sceKernelGetProcessTimeWide();
        int r = sceAvcdecDecode(&v->ctrl, &au, &arr);
        uint64_t dt = sceKernelGetProcessTimeWide() - t0;
        int psize = p->size;
        pkt_free(p);
        vp->st_aus++;
        vp->st_dec_us += dt;
        if (dt > vp->st_dec_max)
            vp->st_dec_max = dt;

        int nout = 0;
        if (r < 0) {
            vp->st_errors++;
            if (++vp->total_errors <= 5)
                LOGW("video: error al decodificar 0x%08X (%d bytes)", (unsigned)r, psize);
            if (elm > 1 && vp->total_pics == 0 && consecutive_err >= 29) {
                /* not one picture yet: maybe numOfElm > 1 is not accepted
                 * (only numOfElm = 1 is used in public code) */
                LOGW("video: sin imagenes tras %d errores, pruebo con 1 imagen por llamada", consecutive_err + 1);
                elm = 1;
                skip_to_idr = 1;
                consecutive_err = 0;
            }
            if (++consecutive_err > 300) {
                snprintf(vp->fatal_msg, sizeof(vp->fatal_msg), vt_msg("Error del decodificador H.264 (0x%08X)"), (unsigned)r);
                vp->fatal = 1;
            }
        } else {
            consecutive_err = 0;
            nout = (int)arr.numOfOutput;
            if (nout > elm)
                nout = elm;
            if (nout > 1)
                vp->st_multi++;
            vp->st_pics += nout;
            if (nout > 0 && vp->total_pics == 0)
                LOGI("video: primera imagen decodificada %dx%d", (int)pic[0].frame.horizontalSize,
                     (int)pic[0].frame.verticalSize);
            vp->total_pics += nout;
        }

        pthread_mutex_lock(&vp->lock);
        for (int k = 0; k < DEC_PICS; k++) {
            vslot_t *s = &vp->slot[fr[k]];
            if (k < nout && vp->nready < MAX_SLOTS) {
                s->pts = pic_pts(&pic[k]);
                s->w = (int)pic[k].frame.horizontalSize;
                s->h = (int)pic[k].frame.verticalSize;
                s->state = SLOT_READY;
                vp->ready[vp->nready++] = fr[k];
            } else {
                s->state = SLOT_FREE;
            }
        }
        pthread_mutex_unlock(&vp->lock);
    }
    vp->done = 1;
    return sceKernelExitDeleteThread(0);
}

static void vpipe_free(vpipe_t *vp)
{
    vita2d_wait_rendering_done();
    for (int i = 0; i < vp->nslot; i++)
        if (vp->slot[i].tex)
            vita2d_free_texture(vp->slot[i].tex);
    vp->nslot = 0;
    pthread_mutex_destroy(&vp->lock);
}

static int vpipe_start(vpipe_t *vp, vdec_t *v, const player_params_t *pp, char *err, int errlen)
{
    memset(vp, 0, sizeof(*vp));
    vp->v = v;
    vp->pp = pp;
    pthread_mutex_init(&vp->lock, NULL);

    /* as many 1280x720 RGBA textures as fit in ~32 MB, between 4 and 8 */
    size_t frame_bytes = (size_t)v->w * v->h * 4;
    int want = (int)((32u * 1024 * 1024) / frame_bytes);
    if (want > MAX_SLOTS) want = MAX_SLOTS;
    /* the decoder reserves DEC_PICS free slots per call and one is on screen:
     * with fewer than DEC_PICS + 3 it can't run ahead and video stays late
     * after any hiccup (1080p). Fewer are still accepted if memory runs out. */
    if (want < DEC_PICS + 3) want = DEC_PICS + 3;
    for (int i = 0; i < want; i++) {
        vita2d_texture *t = vita2d_create_empty_texture_format(v->w, v->h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!t)
            break;
        memset(vita2d_texture_get_datap(t), 0, (size_t)vita2d_texture_get_stride(t) * v->h);
        vp->slot[vp->nslot].tex = t;
        vp->slot[vp->nslot].state = SLOT_FREE;
        vp->nslot++;
    }
    if (vp->nslot < DEC_PICS + 1) {
        snprintf(err, (size_t)errlen, vt_msg("Sin memoria para las texturas de video"));
        LOGE("video: solo %d texturas de %dx%d", vp->nslot, v->w, v->h);
        vpipe_free(vp);
        return -1;
    }
    v->pitch = vita2d_texture_get_stride(vp->slot[0].tex) / 4;

    vp->running = 1;
    vp->thread = sceKernelCreateThread("vt_vdec", vdec_thread, 0x10000100 - 10, 64 * 1024, 0,
                                       SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (vp->thread < 0 || sceKernelStartThread(vp->thread, 0, NULL) < 0) {
        snprintf(err, (size_t)errlen, vt_msg("No se pudo crear el hilo de video"));
        LOGE("video: %s (0x%08X)", err, (unsigned)vp->thread);
        vp->running = 0;
        vpipe_free(vp);
        return -1;
    }
    LOGI("video: hilo de decodificacion con %d texturas", vp->nslot);
    return 0;
}

static void vpipe_stop(vpipe_t *vp)
{
    vp->running = 0;
    for (int i = 0; i < 300 && !vp->done; i++) /* pq_get times out every 20 ms */
        sceKernelDelayThread(10000);
}

/* ---------------------------------------------------------------- drawing */

typedef struct {
    const player_params_t *pp;
    int osd;
    int dropped;
    float fps_now;
    int cpu_mhz;
    char volume_notice[96];
    const char *status;   /* centred message (buffering, ads...) or NULL */
    int menu;             /* in-player extras opened by SELECT */
    int menu_sel;
    const char *notice;   /* transient send/authentication feedback */
    uint64_t notice_until;
} view_t;

static void draw_view(const vslot_t *frame, view_t *vw)
{
    int chat = vw->pp->chat_on && *vw->pp->chat_on;
    int AW = chat ? SCREEN_W - CHAT_PANEL_W : SCREEN_W; /* video area width */

    img_frame(); /* emote textures (must run before start_drawing) */
    if (chat)
        chat_view_update(CHAT_PANEL_W, SCREEN_H, vw->pp->font, vw->pp->channel);
    vita2d_start_drawing();
    vita2d_clear_screen();

    if (frame && frame->w > 0 && frame->h > 0) {
        /* visible size: playlist resolution if it fits, else decoder output size */
        int vis_w = (vw->pp->width > 0 && vw->pp->width <= frame->w) ? vw->pp->width : frame->w;
        int vis_h = (vw->pp->height > 0 && vw->pp->height <= frame->h) ? vw->pp->height : frame->h;
        float sx = (float)AW / vis_w;
        float sy = (float)SCREEN_H / vis_h;
        float s = sx < sy ? sx : sy;
        float dw = vis_w * s, dh = vis_h * s;
        vita2d_draw_texture_part_scale(frame->tex, (AW - dw) / 2, (SCREEN_H - dh) / 2,
                                       0, 0, vis_w, vis_h, s, s);
    }
    if (chat)
        chat_view_draw(AW, 0);

    vita2d_pgf *f = vw->pp->font;
    if (vw->osd) {
        char line[160];
        vita2d_draw_rectangle(0, 0, AW, 64, COLOR_OSD_BG);
        vita2d_draw_rectangle(14, 12, 54, 20, COLOR_LIVE);
        vita2d_pgf_draw_text(f, 19, 27, COLOR_WHITE, 0.7f, vt_msg("DIRECTO"));
        vita2d_pgf_draw_text(f, 78, 29, COLOR_WHITE, 1.05f, vw->pp->channel);
        int64_t buffered = pq_span(&g_pl.vq) / PTS_PER_MS;
        snprintf(line, sizeof(line), vt_msg("%s   buffer %.1f s   descartados %d"),
                 vw->pp->quality, buffered / 1000.0, vw->dropped);
        int lw = vita2d_pgf_text_width(f, 0.8f, line);
        vita2d_pgf_draw_text(f, AW - 14 - lw, 27, COLOR_GREY, 0.8f, line);
        if (vw->pp->title && vw->pp->title[0]) {
            /* single line, cut to the screen width */
            char t[200];
            snprintf(t, sizeof(t), "%s", vw->pp->title);
            size_t n = strlen(t);
            while (n > 0 && vita2d_pgf_text_width(f, 0.85f, t) > AW - 28) {
                do { n--; } while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80);
                if (n + 4 <= sizeof(t))
                    memcpy(t + n, "...", 4);
            }
            vita2d_pgf_draw_text(f, 14, 54, COLOR_GREY, 0.85f, t);
        }
        vita2d_draw_rectangle(0, SCREEN_H - 32, AW, 32, COLOR_OSD_BG);
        vita2d_pgf_draw_text(f, 14, SCREEN_H - 10, COLOR_GREY, 0.8f,
                             vw->pp->btn_back == SCE_CTRL_CIRCLE
                                 ? vt_msg("O: salir    Triangulo: info    Cuadrado: chat")
                                 : vt_msg("X: salir    Triangulo: info    Cuadrado: chat"));
    }
    if(vw->pp->diagnostics && !vw->menu){
        char a[160],b[160];
        snprintf(a,sizeof(a),"%s %.1f | %s %.2f Mbps | %s %d MHz",vt_opt(VT_O_FPS),vw->fps_now,vt_opt(VT_O_RATE),g_pl.hls.download_kbps/1000.0,vt_opt(VT_O_CPU),vw->cpu_mhz);
        snprintf(b,sizeof(b),"%s %.1f s | %s %d | %s %d",vt_opt(VT_O_BUFFER),pq_span(&g_pl.aq)/90000.0,vt_opt(VT_O_REBUFFERS),g_pl.rebuffers,vt_opt(VT_O_DROPPED),vw->dropped);
        vita2d_draw_rectangle(0,70,AW,58,COLOR_OSD_BG);
        ui_text_fit(10,91,AW-20,COLOR_WHITE,0.72f,a);
        ui_text_fit(10,116,AW-20,COLOR_WHITE,0.72f,b);
    }
    if (vw->pp->chat_on && *vw->pp->chat_on && vw->pp->chat_oauth && !vw->menu) {
        /* Tap target for composing chat messages. Only becomes writable
         * after the authenticated chat IRC connection is ready. */
        vita2d_draw_rectangle(SCREEN_W-155, SCREEN_H-41, 147, 34, COLOR_OSD_BG);
        vita2d_pgf_draw_text(f, SCREEN_W-146, SCREEN_H-18, COLOR_WHITE, 0.85f,
                             vt_tr(VT_I_WRITE));
    }
    if(vw->menu){
        static const vt_text_id labels[]={VT_I_CHAT,VT_I_SEND_MESSAGE,
                                        VT_I_SHOW_INFO,VT_I_CLOSE,VT_I_EXIT};
        vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,RGBA8(0,0,0,190));
        ui_round_rect(205,95,550,354,12,C_SURFACE);
        vita2d_pgf_draw_text(f,233,136,C_WHITE,1.25f,vt_tr(VT_I_MENU));
        for(int i=0;i<5;i++){
            int y=151+i*55;
            if(i==vw->menu_sel)ui_round_rect(222,y,516,49,8,C_PURPLE);
            const char *label=labels[i]==VT_I_SHOW_INFO?
                 (vw->osd?vt_tr(VT_I_HIDE_INFO):vt_tr(VT_I_SHOW_INFO)):vt_tr(labels[i]);
            vita2d_pgf_draw_text(f,245,y+32,C_WHITE,1.02f,label);
            if(i==0)vita2d_pgf_draw_text(f,640,y+32,C_WHITE,0.8f,
                chat?vt_tr(VT_I_ON):vt_tr(VT_I_OFF));
        }
    }
    if(vw->notice&&sceKernelGetProcessTimeWide()<vw->notice_until){
        ui_round_rect(160,SCREEN_H-83,640,47,9,C_PANEL);
        ui_text_fit(180,SCREEN_H-54,600,C_WHITE,0.95f,vw->notice);
    }
    if (vw->status) {
        int tw = vita2d_pgf_text_width(f, 1.2f, vw->status);
        vita2d_draw_rectangle((AW - tw) / 2 - 20, SCREEN_H / 2 - 28, tw + 40, 44, COLOR_OSD_BG);
        vita2d_pgf_draw_text(f, (AW - tw) / 2, SCREEN_H / 2 + 4, COLOR_WHITE, 1.2f, vw->status);
    }

    vita2d_end_drawing();
    vita2d_common_dialog_update(); /* required by nonblocking Vita OSK */
    vita2d_wait_rendering_done();
    img_free_trash(); /* the GPU is idle now: evicted emote textures can go */
    vita2d_swap_buffers();
}


/* Native PS Vita IME without blocking the video/decode/render loop. */
static int compose_start(SceWChar16 *title,SceWChar16 *buffer){
    static SceWChar16 blank[]={0};
    memset(buffer,0,sizeof(SceWChar16)*192);
    vt_utf8_to_utf16((uint16_t*)title,64,vt_tr(VT_I_SEND_MESSAGE));
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages =
        SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH |
        SCE_IME_LANGUAGE_PORTUGUESE_BR | SCE_IME_LANGUAGE_FRENCH |
        SCE_IME_LANGUAGE_GERMAN | SCE_IME_LANGUAGE_ITALIAN |
        SCE_IME_LANGUAGE_RUSSIAN | SCE_IME_LANGUAGE_JAPANESE |
        SCE_IME_LANGUAGE_KOREAN | SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE |
        SCE_IME_LANGUAGE_TRADITIONAL_CHINESE | SCE_IME_LANGUAGE_TURKISH;
    param.languagesForced=SCE_FALSE;
    param.type=SCE_IME_TYPE_DEFAULT;
    param.option=SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    param.dialogMode=SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode=SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title=title;
    param.maxTextLength=180; /* Twitch IRC lines limited by byte count below */
    param.initialText=blank;
    param.inputTextBuffer=buffer;
    return sceImeDialogInit(&param)==0;
}

/* ------------------------------------------------------------------- main */

int player_run(const player_params_t *pp, char *err, int errlen)
{
    player_t *pl = &g_pl;
    vpipe_t *vp = &g_vp;
    vdec_t vdec;
    view_t view;
    int ret = 0;

    memset(pl, 0, sizeof(*pl));
    memset(&view, 0, sizeof(view));
    view.pp = pp;
    view.osd = 1;
    pl->profile=pp->profile;
    pl->volume=pp->volume?*pp->volume:100;
    view.cpu_mhz=vt_profile_clock(pp->profile,pp->height,pp->fps);
    err[0] = 0;
    pthread_mutex_init(&pl->clock.lock, NULL);

    if (vdec_open(&vdec, pp->width, pp->height, err, errlen) < 0) {
        pthread_mutex_destroy(&pl->clock.lock);
        return -1;
    }

    pq_init(&pl->vq, 900, 24 * 1024 * 1024);
    pq_init(&pl->aq, 1500, 2 * 1024 * 1024);

    if (vpipe_start(vp, &vdec, pp, err, errlen) < 0) {
        ret = -1;
        goto out_queues;
    }
    if (hls_start(&pl->hls, pp->url, &pl->vq, &pl->aq, vt_profile_edge(pp->profile)) < 0) {
        snprintf(err, (size_t)errlen, vt_msg("No se pudo crear el hilo de descarga"));
        LOGE("%s", err);
        ret = -1;
        pq_abort(&pl->vq);
        vpipe_stop(vp);
        vpipe_free(vp);
        goto out_queues;
    }

    pl->audio_running = 1;
    /* audio on core 2: it must never preempt the drawing thread on core 0 */
    pl->audio_thread = sceKernelCreateThread("vt_audio", audio_thread, 100, 64 * 1024, 0,
                                             SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (pl->audio_thread >= 0) {
        player_t *self = pl;
        if (sceKernelStartThread(pl->audio_thread, sizeof(self), &self) < 0) {
            sceKernelDeleteThread(pl->audio_thread);
            pl->audio_thread = -1;
            pl->audio_done = 1;
            pl->audio_failed = 1;
        }
    } else {
        LOGE("audio: no se pudo crear el hilo (0x%08X)", (unsigned)pl->audio_thread);
        pl->audio_done = 1;
        pl->audio_failed = 1;
    }

    if (pp->chat_on && *pp->chat_on && pp->login)
        chat_start(pp->login,pp->chat_nick,pp->chat_oauth);
    img_set_deferred_free(1);

    ui_input_t input={0};
    ui_input_poll(&input); /* swallow X from quality selector */
    SceWChar16 compose_title[64]={0},compose_text[192]={0};
    int composing=0;
    int should_exit=0;

    int cur = -1;                 /* slot on screen */
    int presented = 0, late_dropped = 0;
    uint64_t start_us = sceKernelGetProcessTimeWide();
    uint64_t last_draw = 0, last_tick = 0, last_stats = start_us, osd_until = start_us + 5000000;
    uint64_t last_present = 0, max_present_gap = 0;
    uint64_t draw_us = 0, draw_max = 0;
    unsigned draw_n = 0;
    int freerun_logged = 0, first_logged = 0;
    int fps_frames=0, bad_intervals=0, stable_intervals=0, prev_rebuffers=0;
    uint64_t fps_time=start_us;
    /* a frame is due when its pts is within half a frame of the clock */
    int64_t half = (int64_t)(90000.0 / ((pp->fps > 1 ? pp->fps : 30.0f) * 2.0f));

    for (;;) {
        uint64_t now_us = sceKernelGetProcessTimeWide();

        ui_input_poll(&input);
        unsigned pressed=input.pressed;
        int open_composer=0;
        if(composing){
            pressed=0;
            if(sceImeDialogGetStatus()==SCE_COMMON_DIALOG_STATUS_FINISHED){
                SceImeDialogResult result;
                memset(&result,0,sizeof(result));
                sceImeDialogGetResult(&result);
                sceImeDialogTerm();
                composing=0;
                if(result.button==SCE_IME_DIALOG_BUTTON_ENTER){
                    char text[CHAT_TEXT_MAX];
                    vt_utf16_to_utf8(text,sizeof(text),(const uint16_t*)compose_text,192);
                    int rc=chat_send_text(text);
                    view.notice=rc==0?vt_msg("Message queued (server confirmation pending)"):
                        rc==-3?vt_msg("Please slow down (chat rate limit)"):
                        rc==-2?vt_msg("Message empty or too long"):
                        vt_tr(VT_I_LOGIN_TO_CHAT);
                    view.notice_until=now_us+3000000ULL;
                }
                last_draw=0;
            }
        } else {
            int accept=pp->btn_back==SCE_CTRL_CIRCLE?SCE_CTRL_CROSS:SCE_CTRL_CIRCLE;
            if(pressed&SCE_CTRL_SELECT){view.menu=!view.menu;view.menu_sel=0;last_draw=0;}
            if(view.menu){
                if(pressed&SCE_CTRL_UP)view.menu_sel=(view.menu_sel+4)%5;
                if(pressed&SCE_CTRL_DOWN)view.menu_sel=(view.menu_sel+1)%5;
                if(input.touch_tap){
                    int hit=0;
                    for(int i=0;i<5;i++)
                        if(ui_touch_hit(&input,222,151+i*55,516,49)){
                            view.menu_sel=i;pressed|=accept;hit=1;break;
                        }
                    if(!hit)view.menu=0;
                }
                if(pressed&pp->btn_back)view.menu=0;
                if(view.menu&&(pressed&accept)){
                    switch(view.menu_sel){
                        case 0:
                            if(pp->chat_on&&pp->login){
                                *pp->chat_on=!*pp->chat_on;
                                if(*pp->chat_on&&!chat_running())
                                    chat_start(pp->login,pp->chat_nick,pp->chat_oauth);
                            }
                            break;
                        case 1:open_composer=1;view.menu=0;break;
                        case 2:view.osd=!view.osd;osd_until=0;break;
                        case 3:view.menu=0;break;
                        case 4:should_exit=1;break;
                    }
                }
            }else{
                if(pressed&pp->btn_back)should_exit=1;
                if(pressed&SCE_CTRL_TRIANGLE){view.osd=!view.osd;osd_until=0;}
                if((pressed&SCE_CTRL_SQUARE)&&pp->chat_on&&pp->login){
                    *pp->chat_on=!*pp->chat_on;
                    if(*pp->chat_on&&!chat_running())
                        chat_start(pp->login,pp->chat_nick,pp->chat_oauth);
                    LOGI("chat %s",*pp->chat_on?"visible":"oculto");
                }
                if(pressed&SCE_CTRL_RTRIGGER)open_composer=1;
                if(pressed&(SCE_CTRL_LEFT|SCE_CTRL_RIGHT)){
                    int v=pl->volume+(pressed&SCE_CTRL_RIGHT?5:-5);
                    if(v<0)v=0;
                    if(v>100)v=100;
                    pl->volume=v; if(pp->volume)*pp->volume=v;
                    snprintf(view.volume_notice,sizeof(view.volume_notice),"%s: %d%%",vt_opt(VT_O_VOLUME),v);
                    view.notice=view.volume_notice;view.notice_until=now_us+2000000ULL;
                }
                if(input.touch_tap){
                    if(pp->chat_on&&*pp->chat_on&&
                       ui_touch_hit(&input,SCREEN_W-155,SCREEN_H-41,147,34))
                        open_composer=1;
                    else{view.osd=!view.osd;osd_until=0;}
                }
            }
            if(open_composer){
                if(!chat_can_send()){
                    view.notice=vt_tr(VT_I_LOGIN_TO_CHAT);
                    view.notice_until=now_us+3000000ULL;
                }else if(compose_start(compose_title,compose_text)){
                    composing=1;view.menu=0;last_draw=0;
                }else{
                    view.notice=vt_msg("Cannot open PS Vita keyboard");
                    view.notice_until=now_us+3000000ULL;
                }
            }
        }
        if(should_exit)break;
        if (osd_until && now_us > osd_until) {
            view.osd = 0;
            osd_until = 0;
        }
        if (now_us - last_tick > 1000000) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); /* keep the screen on */
            last_tick = now_us;
        }
        if(now_us-fps_time>=1000000ULL){view.fps_now=(float)(fps_frames*1000000.0/(now_us-fps_time));fps_frames=0;fps_time=now_us;}
        if (now_us - last_stats > 10000000ULL) { /* every 10 s */
            int idec, iev, ifail, ifree;
            img_stats(&idec, &iev, &ifail, &ifree);
            LOGI("estado: %d AUs -> %d imagenes (%d multiples), %d mostradas, %d tarde, %d sin decodificar, "
                 "%d omitidas por chat, %d saltos, %d errores; cola video %.1f s, audio %.1f s, listas %d, "
                 "%d rebuffers, chat %s",
                 vp->st_aus, vp->st_pics, vp->st_multi, presented, late_dropped, vp->st_undecoded,
                 vp->st_chat_skip, vp->st_idr_jumps, vp->st_errors, pq_span(&pl->vq) / PTS_PER_MS / 1000.0,
                 pq_span(&pl->aq) / PTS_PER_MS / 1000.0, vp->nready, pl->rebuffers,
                 pp->chat_on && *pp->chat_on ? "visible" : "oculto");
            LOGI("tiempos: decodificar %.1f ms (max %.1f), dibujar %.1f ms (max %.1f), hueco max entre "
                 "imagenes %.0f ms, retraso max %.2f s; panel chat %d x %.1f ms; emotes: %d decodificados, "
                 "%d expulsados, %d fallidos, %d liberados",
                 vp->st_aus ? vp->st_dec_us / 1000.0 / vp->st_aus : 0.0, vp->st_dec_max / 1000.0,
                 draw_n ? draw_us / 1000.0 / draw_n : 0.0, draw_max / 1000.0, max_present_gap / 1000.0,
                 vp->st_max_lag / PTS_PER_MS / 1000.0, chat_view_renders(), chat_view_avg_ms_x10() / 10.0,
                 idec, iev, ifail, ifree);
            int poor=(pl->rebuffers-prev_rebuffers>=2)||vp->st_idr_jumps>=2||(presented>30&&late_dropped>presented/4);
            prev_rebuffers=pl->rebuffers;
            if(poor){bad_intervals++;stable_intervals=0;if(view.cpu_mhz<444 && scePowerSetArmClockFrequency(444)>=0)view.cpu_mhz=444;}
            else {bad_intervals=0;stable_intervals++;}
            if(stable_intervals>=3 && pp->profile!=VT_PROFILE_QUALITY && pp->height<=540 && pp->fps<=40 && view.cpu_mhz>333){
                if(scePowerSetArmClockFrequency(333)>=0)view.cpu_mhz=333;
            }
            if(bad_intervals>=2 && pp->auto_quality && pp->has_lower && !pl->hls.in_ad && !composing){
                ret=PLAYER_LOWER_QUALITY;break;
            }
            vp->st_aus = vp->st_pics = vp->st_multi = vp->st_errors = vp->st_undecoded = 0;
            vp->st_chat_skip = vp->st_idr_jumps = 0;
            vp->st_dec_us = vp->st_dec_max = 0;
            vp->st_max_lag = 0;
            presented = late_dropped = 0;
            draw_us = draw_max = max_present_gap = 0;
            draw_n = 0;
            last_stats = now_us;
        }
        if (pl->freerun && !freerun_logged) {
            LOGW("video: reloj sin audio (audio fallido o ausente)");
            freerun_logged = 1;
        }
        if (vp->fatal) {
            snprintf(err, (size_t)errlen, "%s", vp->fatal_msg);
            ret = -1;
            break;
        }
        /* end: everything shown, or the last frames can never become due (audio
         * ran out first, its clock stopped) and nothing was shown for 3 s */
        if (pl->hls.finished && pq_count(&pl->vq) == 0 &&
            (vp->nready == 0 ||
             (pq_count(&pl->aq) == 0 && now_us - (last_present ? last_present : start_us) > 3000000ULL))) {
            snprintf(err, (size_t)errlen, "%s", pl->hls.error[0] ? pl->hls.error : vt_msg("El stream ha terminado"));
            ret = pl->hls.retryable ? PLAYER_RECONNECT : -1;
            break;
        }

        /* status line */
        if (pl->hls.in_ad)
            view.status = vt_msg("Anuncio de Twitch: saltando...");
        else if (cur < 0 || pl->audio_buffering)
            view.status = vt_msg("Cargando...");
        else
            view.status = NULL;

        /* no audio thread to drain it: a full audio queue would block the hls
         * thread in pq_put and starve the video */
        if (pl->audio_done && pq_count(&pl->aq) > 0)
            pq_flush(&pl->aq);

        /* clock: audio is master; without audio, run on wall time from the first frame */
        int64_t clk = clock_get(&pl->clock);
        {
            pthread_mutex_lock(&vp->lock);
            int64_t head = vp->nready ? vp->slot[vp->ready[0]].pts : TS_NOPTS;
            pthread_mutex_unlock(&vp->lock);
            if (!pl->freerun) {
                int no_audio = pl->audio_failed ||
                               (clk == TS_NOPTS && pq_count(&pl->aq) == 0 && pq_span(&pl->vq) > 3000 * PTS_PER_MS);
                if (no_audio && head != TS_NOPTS) {
                    clock_set(&pl->clock, head, INT64_MAX / 2);
                    pl->freerun = 1;
                    clk = head;
                }
            } else if (head != TS_NOPTS && clk != TS_NOPTS && clk - head > 1000 * PTS_PER_MS) {
                /* wall clock ran ahead (download stall): nothing would ever be on
                 * time again, the decoder would only show keyframes. Re-anchor. */
                clock_set(&pl->clock, head, INT64_MAX / 2);
                clk = head;
            }
        }

        /* pick the newest frame that is due; older due ones are dropped */
        int next = -1;
        if (clk != TS_NOPTS) {
            pthread_mutex_lock(&vp->lock);
            while (vp->nready > 0) {
                int h = vp->ready[0];
                int64_t hp = vp->slot[h].pts;
                int64_t d = hp == TS_NOPTS ? 0 : hp - clk;
                if (d > 5000 * PTS_PER_MS)
                    d = 0; /* broken timestamps: don't stall */
                if (d > half)
                    break; /* not due yet */
                if (vp->nready > 1) {
                    int64_t np = vp->slot[vp->ready[1]].pts;
                    if (np != TS_NOPTS && hp != TS_NOPTS && np - clk <= half && np - clk > -5000 * PTS_PER_MS) {
                        /* the following frame is due too: this one is late */
                        vp->slot[h].state = SLOT_FREE;
                        memmove(vp->ready, vp->ready + 1, sizeof(int) * (size_t)(vp->nready - 1));
                        vp->nready--;
                        late_dropped++;
                        view.dropped++;
                        continue;
                    }
                }
                next = h;
                memmove(vp->ready, vp->ready + 1, sizeof(int) * (size_t)(vp->nready - 1));
                vp->nready--;
                break;
            }
            pthread_mutex_unlock(&vp->lock);
        }

        if (next >= 0) {
            uint64_t t_draw = sceKernelGetProcessTimeWide();
            draw_view(&vp->slot[next], &view); /* ends with vita2d_wait_rendering_done() */
            uint64_t t_end = sceKernelGetProcessTimeWide();
            /* the GPU is done with the previous frame: give its texture back */
            pthread_mutex_lock(&vp->lock);
            if (cur >= 0)
                vp->slot[cur].state = SLOT_FREE;
            vp->slot[next].state = SLOT_SHOWN;
            pthread_mutex_unlock(&vp->lock);
            cur = next;
            vp->shown_any = 1;
            if (!first_logged) {
                LOGI("video: primer frame %dx%d tras %llu ms", vp->slot[cur].w, vp->slot[cur].h,
                     (unsigned long long)(t_end - start_us) / 1000ULL);
                first_logged = 1;
            }
            if (last_present && t_draw - last_present > max_present_gap)
                max_present_gap = t_draw - last_present;
            last_present = t_draw;
            last_draw = t_end;
            presented++;
            fps_frames++;
            draw_us += t_end - t_draw;
            draw_n++;
            if (t_end - t_draw > draw_max)
                draw_max = t_end - t_draw;
        } else if (now_us - last_draw > 100000) {
            /* nothing new: keep the status/chat/OSD alive */
            draw_view(cur >= 0 ? &vp->slot[cur] : NULL, &view);
            last_draw = sceKernelGetProcessTimeWide();
        } else {
            sceKernelDelayThread(2000);
        }
    }

    if(composing)sceImeDialogTerm();
    LOGI("fin de reproduccion tras %llu s: %d imagenes, %d descartados, %d errores de video, %d rebuffers%s%s",
         (unsigned long long)(sceKernelGetProcessTimeWide() - start_us) / 1000000ULL, vp->total_pics,
         view.dropped, vp->total_errors, pl->rebuffers, ret < 0 ? ", motivo: " : " (salida del usuario)",
         ret < 0 ? err : "");

    /* teardown: unblock producers first, then stop threads */
    pq_abort(&pl->vq);
    pq_abort(&pl->aq);
    vpipe_stop(vp);
    chat_stop();
    hls_stop(&pl->hls);
    pl->audio_running = 0;
    for (int i = 0; i < 300 && !pl->audio_done; i++)
        sceKernelDelayThread(10000);
    vpipe_free(vp);

out_queues:
    pq_destroy(&pl->vq);
    pq_destroy(&pl->aq);
    vdec_close(&vdec);
    chat_view_free();
    img_set_deferred_free(0);
    pthread_mutex_destroy(&pl->clock.lock);
    return ret;
}
