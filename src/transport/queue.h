/*
 * vmsotel: the export queue.
 *
 * Encoded payloads wait here until the collector takes them. It holds
 * a fixed number of payloads of a fixed maximum size, allocated once at
 * startup; when it is full, one is dropped to make room. A VMS
 * production system must never fill its memory or its disk because a
 * monitoring server is down (docs/design.md, Running it).
 *
 * Each payload carries a kind, the caller's own number, which the agent
 * uses to say where it goes: metrics to one path, logs to another. When
 * the queue is full, the oldest payload of the kind set with
 * vo_queue_drop_first goes first, and only if there is none the oldest
 * of any: a metrics export is a snapshot the next one supersedes, but a
 * log payload holds records nothing will send again.
 */

#ifndef VO_QUEUE_H
#define VO_QUEUE_H

#include <stddef.h>

#define VO_QUEUE_MAX 64

typedef struct {
    char *mem;
    size_t slot_size;
    unsigned nslots;
    unsigned count;
    unsigned order[VO_QUEUE_MAX];   /* slots in use, oldest first */
    int used[VO_QUEUE_MAX];         /* by slot */
    size_t len[VO_QUEUE_MAX];       /* by slot */
    int kind[VO_QUEUE_MAX];         /* by slot */
    int drop_kind;                  /* dropped first; -1 for none */
    int dropped_kind;               /* what the last push dropped */
} vo_queue;

/* 1 on success, 0 if nslots is out of range or memory is short. */
int vo_queue_init(vo_queue *q, unsigned nslots, size_t slot_size);
void vo_queue_free(vo_queue *q);

/* The kind to drop first when the queue is full. */
void vo_queue_drop_first(vo_queue *q, int kind);

/* Copies a payload in. Returns 1 if one was dropped to make room (its
   kind in q->dropped_kind), 0 if not, -1 if the payload is larger than
   a slot. */
int vo_queue_push(vo_queue *q, const char *p, size_t n, int kind);

/* The oldest payload and its kind, or NULL when empty. */
const char *vo_queue_peek(const vo_queue *q, size_t *n, int *kind);
void vo_queue_pop(vo_queue *q);

#endif
