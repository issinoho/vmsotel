/*
 * vmsotel: the export queue.
 *
 * Encoded payloads wait here until the collector takes them. It holds
 * a fixed number of payloads of a fixed maximum size, allocated once at
 * startup; when it is full, the oldest is dropped to make room. A VMS
 * production system must never fill its memory or its disk because a
 * monitoring server is down (docs/design.md, Running it).
 */

#ifndef VO_QUEUE_H
#define VO_QUEUE_H

#include <stddef.h>

#define VO_QUEUE_MAX 64

typedef struct {
    char *mem;
    size_t slot_size;
    unsigned nslots;
    unsigned head;              /* oldest */
    unsigned count;
    size_t len[VO_QUEUE_MAX];
} vo_queue;

/* 1 on success, 0 if nslots is out of range or memory is short. */
int vo_queue_init(vo_queue *q, unsigned nslots, size_t slot_size);
void vo_queue_free(vo_queue *q);

/* Copies a payload in. Returns 1 if the oldest was dropped to make
   room, 0 if not, -1 if the payload is larger than a slot. */
int vo_queue_push(vo_queue *q, const char *p, size_t n);

/* The oldest payload, or NULL when empty. */
const char *vo_queue_peek(const vo_queue *q, size_t *n);
void vo_queue_pop(vo_queue *q);

#endif
