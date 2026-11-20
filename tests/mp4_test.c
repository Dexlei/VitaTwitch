/* Host test for the fMP4 (CMAF) demuxer with real Twitch segments.
 *   gcc -DVT_NO_NET -Isrc tests/mp4_test.c src/mp4_demux.c src/adts.c -o mp4_test   (adts.c if present)
 *   mp4_test init.mp4 seg1.mp4 [seg2.mp4 ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp4_demux.h"

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); failures++; } printf("\n"); } while (0)

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    fread(b, 1, *n, f);
    fclose(f);
    return b;
}

static int vframes, aframes, keyframes, bad_annexb, bad_adts, ps_before_idr = 1, nonmono_dts, idr_first = -1;
static int64_t first_vpts = TS_NOPTS, last_vdts = TS_NOPTS, last_vpts_max = TS_NOPTS, first_apts = TS_NOPTS, last_apts;
static int max_cto_frames;

static int on_es(void *user, int stream, const uint8_t *d, int size, int64_t pts, int64_t dts)
{
    (void)user;
    if (stream == TS_STREAM_VIDEO) {
        if (size < 5 || d[0] || d[1] || d[2] || d[3] != 1) bad_annexb++;
        /* NAL types in order */
        int saw_sps = 0, saw_pps = 0, idr = 0;
        for (int i = 0; i + 4 < size; i++)
            if (!d[i] && !d[i + 1] && !d[i + 2] && d[i + 3] == 1) {
                int t = d[i + 4] & 0x1F;
                if (t == 7) saw_sps = 1;
                if (t == 8) saw_pps = 1;
                if (t == 5) { idr = 1; if (!(saw_sps && saw_pps)) ps_before_idr = 0; }
                i += 3;
            }
        if (idr) { keyframes++; if (idr_first < 0) idr_first = vframes; }
        if (first_vpts == TS_NOPTS) first_vpts = pts;
        if (last_vdts != TS_NOPTS && dts <= last_vdts) nonmono_dts++;
        int cto = (int)((pts - dts) / 1500); /* frames at 60 fps */
        if (cto > max_cto_frames) max_cto_frames = cto;
        last_vdts = dts;
        if (last_vpts_max == TS_NOPTS || pts > last_vpts_max) last_vpts_max = pts;
        vframes++;
    } else {
        int fl = ((d[3] & 3) << 11) | (d[4] << 3) | (d[5] >> 5);
        if (size < 8 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0 || fl != size) bad_adts++;
        if (first_apts == TS_NOPTS) first_apts = pts;
        last_apts = pts;
        aframes++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mp4_test init.mp4 seg.mp4...\n");
        return 2;
    }
    mp4_demux_t d;
    mp4_demux_init(&d, on_es, NULL);
    size_t n;
    uint8_t *init = read_file(argv[1], &n);
    printf("[init]\n");
    CHECK(mp4_looks_like(init, n), "init segment detected as MP4");
    CHECK(mp4_demux_set_init(&d, init, n) == 0, "init parsed: %d tracks, NAL length %d, SPS/PPS %u bytes, AAC obj %d freq idx %d ch %d",
          d.ntrk, d.nal_len_size, (unsigned)d.ps.size, d.aac_object, d.aac_freq_idx, d.aac_channels);
    for (int i = 0; i < d.ntrk; i++)
        printf("    track %u: %s, timescale %u\n", d.trk[i].id, d.trk[i].is_video ? "video" : "audio", d.trk[i].timescale);

    printf("[segmentos]\n");
    for (int a = 2; a < argc; a++) {
        uint8_t *s = read_file(argv[a], &n);
        CHECK(mp4_looks_like(s, n), "%s detected as MP4", argv[a]);
        int v0 = vframes, a0 = aframes;
        int r = mp4_demux_feed(&d, s, n);
        CHECK(r == 0 && vframes > v0 && aframes > a0, "%s: %d video frames, %d audio frames", argv[a], vframes - v0, aframes - a0);
        free(s);
    }
    uint8_t ts[188] = { 0x47 };
    CHECK(!mp4_looks_like(ts, sizeof(ts)), "MPEG-TS is not mistaken for MP4");

    printf("[resultado]\n");
    double vdur = (last_vpts_max - first_vpts) / 90000.0, adur = (last_apts - first_apts) / 90000.0;
    CHECK(bad_annexb == 0, "every access unit is Annex B (%d bad)", bad_annexb);
    CHECK(keyframes >= 1 && idr_first == 0, "%d keyframes, first frame is a keyframe", keyframes);
    CHECK(ps_before_idr, "SPS and PPS precede every keyframe");
    CHECK(nonmono_dts == 0, "decode timestamps strictly increase");
    CHECK(bad_adts == 0, "every audio frame has a valid ADTS header (%d bad)", bad_adts);
    CHECK(vdur > 0 && adur > 0 && (vdur - adur < 0.2 && adur - vdur < 0.2),
          "video %.2f s vs audio %.2f s, start offset %.0f ms, max reorder %d frames", vdur, adur,
          (first_vpts - first_apts) / 90.0, max_cto_frames);
    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", failures);
    mp4_demux_free(&d);
    return failures ? 1 : 0;
}
