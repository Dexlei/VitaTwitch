#ifndef VT_M3U8_H
#define VT_M3U8_H

#define M3U8_URL_MAX 2048
#define M3U8_MAX_VARIANTS 16
#define M3U8_MAX_SEGMENTS 64

typedef struct {
    char name[48];        /* e.g. "720p60", "480p", "1080p60 (source)" */
    int width, height;
    float fps;
    long bandwidth;
    int audio_only;
    char url[M3U8_URL_MAX];
} m3u8_variant_t;

typedef struct {
    long long seq;
    float duration;
    int discontinuity;    /* EXT-X-DISCONTINUITY precedes this segment */
    int is_ad;            /* Twitch stitched ad (title is not "live") */
    char url[M3U8_URL_MAX];
} m3u8_segment_t;

typedef struct {
    float target_duration;
    long long media_sequence;
    int ended;
    int count;
    m3u8_segment_t seg[M3U8_MAX_SEGMENTS];
} m3u8_media_t;

/* Parses a master playlist. Returns the number of variants. */
int m3u8_parse_master(const char *text, m3u8_variant_t *out, int max);

/* Parses a media playlist (keeps the newest M3U8_MAX_SEGMENTS segments).
 * base_url resolves relative segment URIs. Returns 0 on success. */
int m3u8_parse_media(const char *text, const char *base_url, m3u8_media_t *out);

#endif
