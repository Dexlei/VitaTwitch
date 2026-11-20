#ifndef VT_LOG_H
#define VT_LOG_H

/* Diagnostic log: ux0:data/VitaTwitch/log.txt (the previous run is kept as
 * log_prev.txt). Each line is flushed at once so it survives a crash.
 * Never pass tokens or full URLs with query strings: use log_url(). */

void log_open(const char *path, const char *prev_path);
void log_close(void);
void log_write(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* copies url without its query string ("?sig=...&token=..." can hold secrets) */
const char *log_url(const char *url, char *out, int cap);

#define LOGI(...) log_write('I', __VA_ARGS__)
#define LOGW(...) log_write('W', __VA_ARGS__)
#define LOGE(...) log_write('E', __VA_ARGS__)

#endif
