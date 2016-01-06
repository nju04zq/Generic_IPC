#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include "util.h"

#define NSEC_IN_ONE_MSEC 1000000

void
get_current_timestamp (char *ts)
{
    char ts_no_msec[TIMESTAMP_WITHOUT_MSEC_MAX_LEN+1];
    uint32_t msec;
    struct timespec tp;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &tp);
    msec = tp.tv_nsec/NSEC_IN_ONE_MSEC;
    localtime_r((time_t *)&tp.tv_sec, &tm);
    strftime(ts_no_msec, sizeof(ts_no_msec), "%F %T", &tm);
    snprintf(ts, TIMESTAMP_MAX_LEN+1, "%s.%03d", ts_no_msec, msec);
    return;
}

void
queue_init (queue_t *queue_p, uint32_t max_size)
{
    queue_p->queue_header.prev = &queue_p->queue_header;
    queue_p->queue_header.next = &queue_p->queue_header;
    queue_p->queue_size = 0;
    queue_p->queue_max_size = max_size;
    return;
}

uint32_t
queue_get_max_size (queue_t *queue_p)
{
    return queue_p->queue_max_size;
}

uint32_t
queue_get_size (queue_t *queue_p)
{
    return queue_p->queue_size;
}

int
enqueue (queue_t *queue_p, void *entry_p)
{
    queue_header_t *prev_p, *cur_p, *next_p;

    if (queue_p->queue_max_size != 0 &&
        queue_p->queue_size >= queue_p->queue_max_size) {
        return -1;
    }

    cur_p = (queue_header_t *)entry_p;
    prev_p = queue_p->queue_header.prev;
    next_p = &queue_p->queue_header;

    cur_p->prev = prev_p;
    cur_p->next = next_p;
    prev_p->next = cur_p;
    next_p->prev = cur_p;

    queue_p->queue_size++;
    return 0;
}

static void
queue_remove_entry (queue_t *queue_p, queue_header_t *cur_p)
{
    queue_header_t *prev_p, *next_p;

    prev_p = cur_p->prev;
    next_p = cur_p->next;

    prev_p->next = next_p;
    next_p->prev = prev_p;
    
    cur_p->prev = NULL;
    cur_p->next = NULL;

    queue_p->queue_size--;
    return;
}

void *
dequeue (queue_t *queue_p)
{
    queue_header_t *cur_p;

    if (queue_p->queue_size == 0) {
        return NULL;
    }

    cur_p = queue_p->queue_header.next;
    queue_remove_entry(queue_p, cur_p);
    return (void *)cur_p;
}

void *
queue_peek_head (queue_t *queue_p)
{
    if (queue_p->queue_size == 0) {
        return NULL;
    }

    return queue_p->queue_header.next;
}

void *
queue_get_next (queue_t *queue_p, void *entry_p)
{
    queue_header_t *cur_p, *next_p;

    cur_p = (queue_header_t *)entry_p;
    next_p = cur_p->next;

    if (next_p == &queue_p->queue_header) {
        return NULL;
    } else {
        return next_p;
    }
}

int
unqueue (queue_t *queue_p, void *entry_p)
{
    queue_header_t *cur_p;

    if (queue_p->queue_size == 0) {
        return -1;
    }

    cur_p = queue_peek_head(queue_p);
    while (cur_p != NULL) {
        if (cur_p == (queue_header_t *)entry_p) {
            break;
        }
        cur_p = queue_get_next(queue_p, cur_p);
    }

    if (cur_p == NULL) {
        return -1;
    }

    queue_remove_entry(queue_p, cur_p);
    return 0;
}

