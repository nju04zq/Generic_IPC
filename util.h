#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <stdbool.h>

#define TRUE true
#define FALSE false

// Not the accurate one, but enough for use
#define TIMESTAMP_MAX_LEN 31
#define TIMESTAMP_WITHOUT_MSEC_MAX_LEN 23

typedef struct queue_header_s {
    struct queue_header_s *prev;
    struct queue_header_s *next;
} queue_header_t;

typedef struct queue_s {
    queue_header_t queue_header;
    uint32_t queue_size;
    uint32_t queue_max_size;
} queue_t;

void
get_current_timestamp(char *ts);

void
queue_init(queue_t *queue_p, uint32_t max_size);

uint32_t
queue_get_max_size(queue_t *queue_p);

uint32_t
queue_get_size(queue_t *queue_p);

int
enqueue(queue_t *queue_p, void *entry_p);

void *
dequeue(queue_t *queue_p);

void *
queue_peek_head(queue_t *queue_p);

void *
queue_get_next(queue_t *queue_p, void *entry_p);

int
unqueue(queue_t *queue_p, void *entry_p);
#endif //__UTIL_H__
