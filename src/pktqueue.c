#include "pktqueue.h"
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

packet_t *pkt_new(const uint8_t *data, int size, int64_t pts, int64_t dts)
{
    packet_t *p = malloc(sizeof(*p));
    if (!p)
        return NULL;
    /* 64 bytes of zero padding: decoders may read slightly past the end */
    p->data = malloc((size_t)size + 64);
    if (!p->data) {
        free(p);
        return NULL;
    }
    memcpy(p->data, data, (size_t)size);
    memset(p->data + size, 0, 64);
    p->size = size;
    p->pts = pts;
    p->dts = dts;
    p->next = NULL;
    return p;
}

void pkt_free(packet_t *p)
{
    if (!p)
        return;
    free(p->data);
    free(p);
}

void pq_init(pktqueue_t *q, int max_count, size_t max_bytes)
{
    memset(q, 0, sizeof(*q));
    q->max_count = max_count;
    q->max_bytes = max_bytes;
    pthread_mutex_init(&q->lock, NULL);
}

void pq_flush(pktqueue_t *q)
{
    pthread_mutex_lock(&q->lock);
    packet_t *p = q->head;
    while (p) {
        packet_t *n = p->next;
        pkt_free(p);
        p = n;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    q->bytes = 0;
    pthread_mutex_unlock(&q->lock);
}

void pq_destroy(pktqueue_t *q)
{
    pq_flush(q);
    pthread_mutex_destroy(&q->lock);
}

void pq_abort(pktqueue_t *q)
{
    q->aborted = 1;
}

int pq_put(pktqueue_t *q, packet_t *p)
{
    for (;;) {
        if (q->aborted) {
            pkt_free(p);
            return -1;
        }
        pthread_mutex_lock(&q->lock);
        if (q->count < q->max_count && q->bytes < q->max_bytes) {
            p->next = NULL;
            if (q->tail)
                q->tail->next = p;
            else
                q->head = p;
            q->tail = p;
            q->count++;
            q->bytes += (size_t)p->size;
            pthread_mutex_unlock(&q->lock);
            return 0;
        }
        pthread_mutex_unlock(&q->lock);
        sceKernelDelayThread(5000);
    }
}

packet_t *pq_get(pktqueue_t *q, int timeout_ms)
{
    int waited = 0;
    for (;;) {
        if (q->aborted)
            return NULL;
        pthread_mutex_lock(&q->lock);
        packet_t *p = q->head;
        if (p) {
            q->head = p->next;
            if (!q->head)
                q->tail = NULL;
            q->count--;
            q->bytes -= (size_t)p->size;
            pthread_mutex_unlock(&q->lock);
            return p;
        }
        pthread_mutex_unlock(&q->lock);
        if (waited >= timeout_ms)
            return NULL;
        sceKernelDelayThread(2000);
        waited += 2;
    }
}

int pq_count(pktqueue_t *q)
{
    return q->count;
}

int64_t pq_span(pktqueue_t *q)
{
    int64_t span = 0;
    pthread_mutex_lock(&q->lock);
    if (q->head && q->tail && q->head->pts != INT64_MIN && q->tail->pts != INT64_MIN)
        span = q->tail->pts - q->head->pts;
    pthread_mutex_unlock(&q->lock);
    return span;
}
