/*
 * vmsotel: the export queue, a ring of fixed slots. See queue.h.
 */

#include <stdlib.h>
#include <string.h>

#include "queue.h"

int vo_queue_init(vo_queue *q, unsigned nslots, size_t slot_size)
{
    memset(q, 0, sizeof *q);
    if (nslots == 0 || nslots > VO_QUEUE_MAX || slot_size == 0)
        return 0;
    q->mem = malloc((size_t) nslots * slot_size);
    if (q->mem == NULL)
        return 0;
    q->nslots = nslots;
    q->slot_size = slot_size;
    return 1;
}

void vo_queue_free(vo_queue *q)
{
    free(q->mem);
    memset(q, 0, sizeof *q);
}

int vo_queue_push(vo_queue *q, const char *p, size_t n)
{
    unsigned slot;
    int dropped = 0;

    if (n > q->slot_size)
        return -1;
    if (q->count == q->nslots) {
        vo_queue_pop(q);
        dropped = 1;
    }
    slot = (q->head + q->count) % q->nslots;
    memcpy(q->mem + (size_t) slot * q->slot_size, p, n);
    q->len[slot] = n;
    q->count++;
    return dropped;
}

const char *vo_queue_peek(const vo_queue *q, size_t *n)
{
    if (q->count == 0)
        return NULL;
    *n = q->len[q->head];
    return q->mem + (size_t) q->head * q->slot_size;
}

void vo_queue_pop(vo_queue *q)
{
    if (q->count == 0)
        return;
    q->head = (q->head + 1) % q->nslots;
    q->count--;
}
