/* Host-side tests for the portable parts (no Vita APIs):
 *   gcc -DVT_NO_NET -Isrc tests/host_test.c src/ts_demux.c src/m3u8.c src/twitch.c -o host_test
 *   host_test master.m3u8 media.m3u8 segment.ts gql_response.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adts.h"
#include "m3u8.h"
#include "ts_demux.h"
#include "twitch.h"

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    fread(b, 1, (size_t)n, f);
    b[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

typedef struct {
    int vframes, idr, sps, bad_startcode, not_aud, nopts;
    int aframes, rate, chans, bad_adts;
    long long vfirst, vlast, afirst, alast, vmin, vmax;
} stats_t;

static int on_es(void *user, int stream, const uint8_t *d, int size, int64_t pts, int64_t dts)
{
    stats_t *s = user;
    (void)dts;
    if (stream == TS_STREAM_VIDEO) {
        if (!(size > 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1))))
            s->bad_startcode++;
        { int o = d[2] == 1 ? 3 : 4; if (size <= o || (d[o] & 0x1F) != 9) s->not_aud++; }
        if (pts == TS_NOPTS) s->nopts++;
        for (int i = 0; i + 3 < size; i++) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                int t = d[i + 3] & 0x1F;
                if (t == 5) s->idr++;
                if (t == 7) s->sps++;
            }
        }
        if (s->vframes == 0) { s->vfirst = pts; s->vmin = s->vmax = pts; }
        if (pts < s->vmin) s->vmin = pts;
        if (pts > s->vmax) s->vmax = pts;
        s->vlast = pts;
        s->vframes++;
    } else {
        int off = 0;
        while (off < size) {
            adts_info_t ai;
            if (adts_parse(d + off, size - off, &ai) < 0) { s->bad_adts++; break; }
            s->rate = ai.sample_rate;
            s->chans = ai.channels;
            off += ai.frame_len;
            s->aframes++;
        }
        if (s->afirst == 0) s->afirst = pts;
        s->alast = pts;
    }
    return 0;
}

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); failures++; } printf("\n"); } while (0)

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s master.m3u8 media.m3u8 seg.ts gql.json\n", argv[0]);
        return 2;
    }

    printf("[master playlist]\n");
    m3u8_variant_t v[M3U8_MAX_VARIANTS];
    char *master = read_file(argv[1], NULL);
    int n = m3u8_parse_master(master, v, M3U8_MAX_VARIANTS);
    int has480 = 0, has720 = 0;
    for (int i = 0; i < n; i++) {
        printf("    %-18s %4dx%-4d %5.1ffps %8ld audio_only=%d url=%.40s...\n", v[i].name, v[i].width,
               v[i].height, v[i].fps, v[i].bandwidth, v[i].audio_only, v[i].url);
        if (!v[i].audio_only && v[i].height == 480) has480 = 1;
        if (!v[i].audio_only && v[i].height == 720) has720 = 1;
    }
    CHECK(n >= 2, "%d variants", n);
    CHECK(has480 && has720, "480p and 720p present");

    printf("[media playlist]\n");
    static m3u8_media_t media;
    char *mtext = read_file(argv[2], NULL);
    CHECK(m3u8_parse_media(mtext, "https://example.com/a/b.m3u8?x=1", &media) == 0, "parsed");
    int ads = 0;
    for (int i = 0; i < media.count; i++) ads += media.seg[i].is_ad;
    printf("    target=%.1f seq=%lld segments=%d ads=%d\n", media.target_duration,
           media.media_sequence, media.count, ads);
    CHECK(media.count > 0, "segments found");
    CHECK(!strncmp(media.seg[0].url, "https://", 8), "absolute segment url");

    printf("[ts demux]\n");
    size_t len;
    uint8_t *ts = (uint8_t *)read_file(argv[3], &len);
    stats_t s;
    memset(&s, 0, sizeof(s));
    ts_demux_t d;
    ts_demux_init(&d, on_es, &s);
    /* feed in odd-sized chunks to exercise the carry-over path */
    for (size_t off = 0; off < len; off += 1000)
        ts_demux_feed(&d, ts + off, len - off < 1000 ? len - off : 1000);
    ts_demux_flush(&d);
    double vdur = (s.vmax - s.vmin) / 90000.0;
    printf("    video: %d AUs, %d IDR, %d SPS, span %.2fs   audio: %d frames @%d Hz %dch\n",
           s.vframes, s.idr, s.sps, vdur, s.aframes, s.rate, s.chans);
    CHECK(d.pid[0] >= 0 && d.pid[1] >= 0, "video pid %d audio pid %d", d.pid[0], d.pid[1]);
    CHECK(s.vframes > 20, "video frames");
    CHECK(s.bad_startcode == 0, "every AU starts with an Annex-B start code");
    CHECK(s.not_aud == 0, "every AU begins with an AUD NAL");
    CHECK(s.nopts == 0, "every AU has a pts (%d missing)", s.nopts);
    CHECK(s.sps > 0 && s.idr > 0, "segment carries SPS + IDR (decodable from start)");
    CHECK(s.aframes > 50 && s.bad_adts == 0, "ADTS frames parse cleanly");
    CHECK(vdur > 1.5 && vdur < 2.5, "segment spans ~2 s");

    /* discontinuity: replay the same segment as if it were a new timeline */
    long long before = d.last_pts;
    stats_t s2;
    memset(&s2, 0, sizeof(s2));
    d.user = &s2;
    ts_demux_discontinuity(&d);
    ts_demux_feed(&d, ts, len);
    ts_demux_flush(&d);
    CHECK(s2.vmin > before && s2.vmin - before < 90000, "timestamps continue after discontinuity (+%.3fs)",
          (s2.vmin - before) / 90000.0);
    ts_demux_free(&d);

    printf("[twitch]\n");
    char body[1024], tok[8192], sig[128], login[64];
    twitch_sanitize_login("  Kai_Cenat!! ", login, sizeof(login));
    CHECK(!strcmp(login, "kai_cenat"), "sanitize -> %s", login);
    twitch_build_gql_body("kaicenat", body, sizeof(body));
    FILE *f = fopen("gql_body_test.json", "wb");
    fputs(body, f);
    fclose(f);
    char *gql = read_file(argv[4], NULL);
    CHECK(twitch_parse_token(gql, tok, sizeof(tok), sig, sizeof(sig)) == 0, "token parsed (sig=%s)", sig);
    CHECK(tok[0] == '{' && strstr(tok, "\"channel\"") != NULL, "token is unescaped JSON (%.50s...)", tok);
    f = fopen("token_test.txt", "wb");
    fputs(tok, f);
    fclose(f);

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
