#include "http.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#define VT_CA_PATH "app0:ca/cacert.pem"
#if LIBCURL_VERSION_NUM >= 0x074d00
static unsigned char *ca_pem;
static size_t ca_len;

/* Read with the Vita's own filesystem API. OpenSSL's fopen on device
 * paths varies between SDK builds. Keep the packaged trust store in RAM. */
static void load_ca(void)
{
    SceUID fd=sceIoOpen(VT_CA_PATH,SCE_O_RDONLY,0);
    if(fd<0){LOGE("TLS: CA open failed (0x%08X)",(unsigned)fd);return;}
    SceOff size=sceIoLseek(fd,0,SCE_SEEK_END);
    if(size>0 && size<=1024*1024 && sceIoLseek(fd,0,SCE_SEEK_SET)>=0){
        ca_pem=malloc((size_t)size);
        if(ca_pem){
            size_t off=0;
            while(off<(size_t)size){
                int n=sceIoRead(fd,ca_pem+off,(size_t)size-off);
                if(n<=0)break;
                off+=(size_t)n;
            }
            if(off==(size_t)size)ca_len=off;
            else {free(ca_pem);ca_pem=NULL;}
        }
    }
    sceIoClose(fd);
    LOGI("TLS: packaged CA loaded into memory (%u bytes)",(unsigned)ca_len);
}
#endif

int http_global_init(void)
{
    if(curl_global_init(CURL_GLOBAL_ALL)!=CURLE_OK)return -1;
    LOGI("HTTP: %s",curl_version());
#if LIBCURL_VERSION_NUM >= 0x074d00
    load_ca();
#endif
    return 0;
}

void http_global_term(void)
{
    curl_global_cleanup();
#if LIBCURL_VERSION_NUM >= 0x074d00
    free(ca_pem);ca_pem=NULL;ca_len=0;
#endif
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    buf_t *b = userdata;
    if(nmemb && size>SIZE_MAX/nmemb)return 0;
    size_t n = size * nmemb;
    if(b->size>16u*1024u*1024u || n>16u*1024u*1024u-b->size)return 0;
    if (buf_append(b, ptr, n) < 0)
        return 0;
    return n;
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    volatile int *abort_flag = clientp;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return (abort_flag && *abort_flag) ? 1 : 0;
}

CURL *http_new(void)
{
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    curl_easy_setopt(c, CURLOPT_USERAGENT, VT_USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024L);
    curl_easy_setopt(c, CURLOPT_CAINFO, VT_CA_PATH);
#if LIBCURL_VERSION_NUM >= 0x074d00
    if(ca_len){
        struct curl_blob blob={ca_pem,ca_len,CURL_BLOB_COPY};
        CURLcode rc=curl_easy_setopt(c,CURLOPT_CAINFO_BLOB,&blob);
        if(rc!=CURLE_OK)LOGW("TLS: CA memory option unavailable (%d)",(int)rc);
    }
#endif
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    return c;
}

void http_free(CURL *c)
{
    if (c)
        curl_easy_cleanup(c);
}

int http_request(CURL *c, const char *url, const char *post_body,
                 struct curl_slist *headers, buf_t *out, long *status,
                 volatile int *abort_flag)
{
    buf_reset(out);
    if (out->data)
        out->data[0] = 0;
    *status = 0;

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)abort_flag);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (post_body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    }

    unsigned long long t0 = sceKernelGetProcessTimeWide();
    char detail[CURL_ERROR_SIZE]={0};
    curl_easy_setopt(c,CURLOPT_ERRORBUFFER,detail);
    CURLcode res = curl_easy_perform(c);
    curl_easy_setopt(c,CURLOPT_ERRORBUFFER,NULL);
    unsigned long long ms = (sceKernelGetProcessTimeWide() - t0) / 1000ULL;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, NULL);
    if (!out->data)
        buf_append(out, "", 0);

    char u[160];
    const char *m = post_body ? "POST" : "GET";
    if (res != CURLE_OK) {
        if (!(abort_flag && *abort_flag)) /* user cancels are not errors */
            LOGE("%s %s -> curl %d (%s) tras %llu ms", m, log_url(url, u, sizeof(u)), (int)res,
                 curl_easy_strerror(res), ms);
    } else if (*status >= 400 && !(out->data && strstr((const char *)out->data, "authorization_pending"))) {
        LOGE("%s %s -> HTTP %ld (%llu ms)", m, log_url(url, u, sizeof(u)), *status, ms);
    } else if (ms > 4000) {
        LOGW("%s %s lenta: %llu ms, %u bytes", m, log_url(url, u, sizeof(u)), ms, (unsigned)out->size);
    }
    return res == CURLE_OK ? 0 : -(int)res;
}
