#include "m3u8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copies the next line into line (without CR/LF) and returns a pointer past it. */
static const char *next_line(const char *p, char *line, size_t cap)
{
    size_t n = 0;
    while (*p && *p != '\n' && *p != '\r') {
        if (n + 1 < cap)
            line[n++] = *p;
        p++;
    }
    line[n] = 0;
    while (*p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Reads ATTR=value or ATTR="value" from an attribute list. */
static int get_attr(const char *line, const char *attr, char *out, size_t cap)
{
    size_t alen = strlen(attr);
    const char *p = line;
    while ((p = strstr(p, attr)) != NULL) {
        char before = (p == line) ? ':' : p[-1];
        if ((before == ':' || before == ',') && p[alen] == '=') {
            p += alen + 1;
            size_t n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && n + 1 < cap)
                    out[n++] = *p++;
            } else {
                while (*p && *p != ',' && n + 1 < cap)
                    out[n++] = *p++;
            }
            out[n] = 0;
            return 1;
        }
        p += alen;
    }
    return 0;
}

typedef struct {
    char group[48];
    char name[48];
} media_name_t;

int m3u8_parse_master(const char *text, m3u8_variant_t *out, int max)
{
    static char line[M3U8_URL_MAX + 64];
    media_name_t names[M3U8_MAX_VARIANTS];
    int nnames = 0, count = 0, pending = 0;
    m3u8_variant_t cur;
    const char *p = text;

    memset(&cur, 0, sizeof(cur));
    while (*p) {
        p = next_line(p, line, sizeof(line));
        if (!strncmp(line, "#EXT-X-MEDIA:", 13)) {
            if (nnames < M3U8_MAX_VARIANTS &&
                get_attr(line, "GROUP-ID", names[nnames].group, sizeof(names[nnames].group)) &&
                get_attr(line, "NAME", names[nnames].name, sizeof(names[nnames].name)))
                nnames++;
        } else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            char tmp[64];
            memset(&cur, 0, sizeof(cur));
            if (get_attr(line, "BANDWIDTH", tmp, sizeof(tmp)))
                cur.bandwidth = atol(tmp);
            if (get_attr(line, "RESOLUTION", tmp, sizeof(tmp)))
                sscanf(tmp, "%dx%d", &cur.width, &cur.height);
            if (get_attr(line, "FRAME-RATE", tmp, sizeof(tmp)))
                cur.fps = (float)atof(tmp);
            if (get_attr(line, "VIDEO", tmp, sizeof(tmp))) {
                snprintf(cur.name, sizeof(cur.name), "%s", tmp);
                for (int i = 0; i < nnames; i++)
                    if (!strcmp(names[i].group, tmp))
                        snprintf(cur.name, sizeof(cur.name), "%s", names[i].name);
            }
            if (get_attr(line, "CODECS", tmp, sizeof(tmp)) && !strstr(tmp, "avc1"))
                cur.audio_only = 1;
            if (cur.width == 0)
                cur.audio_only = 1;
            pending = 1;
        } else if (line[0] && line[0] != '#' && pending) {
            snprintf(cur.url, sizeof(cur.url), "%s", line);
            if (!cur.name[0])
                snprintf(cur.name, sizeof(cur.name), "%dp", cur.height);
            if (count < max)
                out[count++] = cur;
            pending = 0;
        }
    }
    return count;
}

static void resolve_url(const char *base, const char *ref, char *out, size_t cap)
{
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8) || !base) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    const char *q = strchr(base, '?');
    size_t blen = q ? (size_t)(q - base) : strlen(base);
    size_t slash = 0;
    for (size_t i = 0; i < blen; i++)
        if (base[i] == '/')
            slash = i + 1;
    snprintf(out, cap, "%.*s%s", (int)slash, base, ref);
}

int m3u8_parse_media(const char *text, const char *base_url, m3u8_media_t *out)
{
    static char line[M3U8_URL_MAX + 64];
    const char *p = text;
    long long idx = 0;
    int disc = 0, is_ad = 0, have_inf = 0, cur_map = -1;
    float dur = 0;

    memset(out, 0, sizeof(*out));
    if (strncmp(text, "#EXTM3U", 7))
        return -1;

    while (*p) {
        p = next_line(p, line, sizeof(line));
        if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
            out->target_duration = (float)atof(line + 22);
        } else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            out->media_sequence = atoll(line + 22);
        } else if (!strncmp(line, "#EXT-X-DISCONTINUITY", 20) && line[20] == 0) {
            disc = 1;
        } else if (!strncmp(line, "#EXT-X-MAP:", 11)) {
            /* fMP4: init segment for the following media segments */
            const char *u = strstr(line, "URI=\"");
            if (u) {
                u += 5;
                const char *e = strchr(u, '"');
                char ref[M3U8_URL_MAX];
                size_t l = e ? (size_t)(e - u) : strlen(u);
                if (l >= sizeof(ref)) l = sizeof(ref) - 1;
                memcpy(ref, u, l);
                ref[l] = 0;
                char abs[M3U8_URL_MAX];
                resolve_url(base_url, ref, abs, sizeof(abs));
                cur_map = -1;
                for (int k = 0; k < out->nmap; k++)
                    if (!strcmp(out->map_url[k], abs))
                        cur_map = k;
                if (cur_map < 0) {
                    if (out->nmap == M3U8_MAX_MAPS) { /* keep the newest ones */
                        memmove(out->map_url[0], out->map_url[1], sizeof(out->map_url[0]) * (M3U8_MAX_MAPS - 1));
                        out->nmap--;
                        for (int k = 0; k < out->count; k++)
                            out->seg[k].map = out->seg[k].map > 0 ? out->seg[k].map - 1 : -1;
                    }
                    snprintf(out->map_url[out->nmap], sizeof(out->map_url[0]), "%s", abs);
                    cur_map = out->nmap++;
                }
            }
        } else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) {
            out->ended = 1;
        } else if (!strncmp(line, "#EXTINF:", 8)) {
            dur = (float)atof(line + 8);
            const char *title = strchr(line, ',');
            title = title ? title + 1 : "";
            /* Twitch titles live segments "live"; stitched ads carry the ad source */
            is_ad = title[0] && strncmp(title, "live", 4) != 0;
            have_inf = 1;
        } else if (line[0] && line[0] != '#') {
            if (have_inf) {
                if (out->count == M3U8_MAX_SEGMENTS) {
                    memmove(&out->seg[0], &out->seg[1], sizeof(out->seg[0]) * (M3U8_MAX_SEGMENTS - 1));
                    out->count--;
                }
                m3u8_segment_t *s = &out->seg[out->count++];
                s->seq = out->media_sequence + idx;
                s->duration = dur;
                s->discontinuity = disc;
                s->is_ad = is_ad;
                s->map = cur_map;
                resolve_url(base_url, line, s->url, sizeof(s->url));
            }
            idx++;
            disc = 0;
            is_ad = 0;
            have_inf = 0;
        }
    }
    if (out->target_duration <= 0)
        out->target_duration = 2;
    return 0;
}
