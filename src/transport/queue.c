/*
 * vmsotel: the export queue: fixed slots, and their order kept apart
 * from them, so one can leave from the middle without moving payloads.
 * See queue.h.
 */

#include <stdlib.h>
#include <string.h>

#include "queue.h"

int vo_queue_init(vo_queue *q, unsigned nslots, size_t slot_size)
{
    memset(q, 0, sizeof *q);
    q->drop_kind = -1;
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
    q->drop_kind = -1;
}

void vo_queue_drop_first(vo_queue *q, int kind)
{
    q->drop_kind = kind;
}

/* Takes the i-th oldest out of the order and frees its slot. */
static void remove_at(vo_queue *q, unsigned i)
{
    q->used[q->order[i]] = 0;
    memmove(q->order + i, q->order + i + 1,
            (q->count - i - 1) * sizeof q->order[0]);
    q->count--;
}

int vo_queue_push(vo_queue *q, const char *p, size_t n, int kind)
{
    unsigned slot, i;
    int dropped = 0;

    if (n > q->slot_size)
        return -1;
    if (q->count == q->nslots) {
        for (i = 0; i < q->count; i++)
            if (q->kind[q->order[i]] == q->drop_kind)
                break;
        if (i == q->count)
            i = 0;
        q->dropped_kind = q->kind[q->order[i]];
        remove_at(q, i);
        dropped = 1;
    }
    for (slot = 0; q->used[slot]; slot++)
        ;
    memcpy(q->mem + (size_t) slot * q->slot_size, p, n);
    q->len[slot] = n;
    q->kind[slot] = kind;
    q->used[slot] = 1;
    q->order[q->count++] = slot;
    return dropped;
}

const char *vo_queue_peek(const vo_queue *q, size_t *n, int *kind)
{
    if (q->count == 0)
        return NULL;
    *n = q->len[q->order[0]];
    *kind = q->kind[q->order[0]];
    return q->mem + (size_t) q->order[0] * q->slot_size;
}

void vo_queue_pop(vo_queue *q)
{
    if (q->count > 0)
        remove_at(q, 0);
}
