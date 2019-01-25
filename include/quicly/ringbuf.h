#ifndef quicly_ringbuf_h
#define quicly_ringbuf_h

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "quicly.h"


typedef struct st_quicly_ringbuf_t {
    uint8_t *data;
    size_t  capacity;
    size_t  start_off;
    size_t  end_off;
} quicly_ringbuf_t;


int quicly_ringbuf_init(quicly_ringbuf_t *buf, size_t start_size);
void quicly_ringbuf_dispose(quicly_ringbuf_t *buf);
size_t quicly_ringbuf_available(quicly_ringbuf_t *buf);
size_t quicly_ringbuf_used(quicly_ringbuf_t *buf);
size_t quicly_ringbuf_used_one_block(quicly_ringbuf_t *buf);
int quicly_ringbuf_grow(quicly_ringbuf_t *buf, size_t min_grow_amount);
void quicly_ringbuf_shift(quicly_ringbuf_t *buf, size_t amount);
int quicly_ringbuf_emit(quicly_ringbuf_t *buf, size_t off, void *dst, size_t *len, int *wrote_all);
int quicly_ringbuf_push(quicly_ringbuf_t *buf, const void *src, size_t len);
int quicly_ringbuf_set(quicly_ringbuf_t *buf, size_t off, const void *src, size_t len);



#ifdef __cplusplus
}
#endif

#endif
