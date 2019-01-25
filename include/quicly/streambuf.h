/*
 * Copyright (c) 2018 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef quicly_streambuf_h
#define quicly_streambuf_h

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "picotls.h"
#include "quicly.h"

/**
 * The simple stream buffer.  The API assumes that stream->data points to quicly_streambuf_t.  Applications can extend the structure
 * by passing arbitrary size to `quicly_streambuf_create`.
 */

typedef struct st_quicly_ringbuf_t {
    uint8_t *data;
    size_t  capacity;
    size_t  start_off;
    size_t  end_off;
} quicly_ringbuf_t;

typedef struct st_quicly_streambuf_t {
    struct {
        quicly_ringbuf_t buf;
        uint64_t max_stream_data;
    } egress;
    quicly_ringbuf_t ingress;
} quicly_streambuf_t;

int quicly_streambuf_create(quicly_stream_t *stream, size_t sz);
void quicly_streambuf_destroy(quicly_stream_t *stream);
void quicly_streambuf_egress_shift(quicly_stream_t *stream, size_t delta);
int quicly_streambuf_egress_emit(quicly_stream_t *stream, size_t off, void *dst, size_t *len, int *wrote_all);
int quicly_streambuf_egress_write(quicly_stream_t *stream, const void *src, size_t len);
int quicly_streambuf_egress_shutdown(quicly_stream_t *stream);
void quicly_streambuf_ingress_shift(quicly_stream_t *stream, size_t delta);
ptls_iovec_t quicly_streambuf_ingress_get(quicly_stream_t *stream);
int quicly_streambuf_ingress_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len);


static int quicly_ringbuf_init(quicly_ringbuf_t *buf, size_t start_size);
static void quicly_ringbuf_dispose(quicly_ringbuf_t *buf);
static size_t quicly_ringbuf_available(quicly_ringbuf_t *buf);
static size_t quicly_ringbuf_used(quicly_ringbuf_t *buf);
static size_t quicly_ringbuf_used_one_block(quicly_ringbuf_t *buf);
static int quicly_ringbuf_grow(quicly_ringbuf_t *buf, size_t min_grow_amount);
static void quicly_ringbuf_shift(quicly_ringbuf_t *buf, size_t amount);
static int quicly_ringbuf_emit(quicly_ringbuf_t *buf, size_t off, void *dst, size_t *len, int *wrote_all);
static int quicly_ringbuf_push(quicly_ringbuf_t *buf, const void *src, size_t len);
static int quicly_ringbuf_set(quicly_ringbuf_t *buf, size_t off, const void *src, size_t len);

inline int quicly_ringbuf_init(quicly_ringbuf_t *buf, size_t start_size) {
    buf->data = malloc(start_size);
    if (!buf->data)
        return -1;
    buf->capacity = start_size;
    buf->start_off = 0;
    buf->end_off = 0;
    return 0;
}

inline void quicly_ringbuf_dispose(quicly_ringbuf_t *buf) {
    free(buf->data);
}

inline size_t quicly_ringbuf_available(quicly_ringbuf_t *buf) {
    if (buf->end_off < buf->start_off) {
        return buf->start_off - buf->end_off;
    } else {
        return buf->capacity - buf->end_off + buf->start_off;
    }
}

inline size_t quicly_ringbuf_used(quicly_ringbuf_t *buf) {
    if (buf->end_off > buf->start_off) {
        return buf->end_off - buf->start_off;
    } else {
        return buf->capacity - buf->start_off + buf->end_off;
    }
}

inline size_t quicly_ringbuf_used_one_block(quicly_ringbuf_t *buf) {
    if (buf->end_off > buf->start_off) {
        return buf->end_off - buf->start_off;
    } else {
        return buf->capacity - buf->start_off;
    }
}


inline int quicly_ringbuf_grow(quicly_ringbuf_t *buf, size_t min_grow_amount) {
    size_t old_size = buf->capacity;
    size_t new_size = 2 * old_size;
    while (new_size - old_size < min_grow_amount) {
        new_size *= 2;
    }
    uint8_t *oldbuf = buf->data;
    buf->data = realloc(buf->data, new_size);
    if (!buf->data) {
        buf->data = oldbuf;
        return -1;
    }
    // Need to move some memory if the used part wraps around the end
    buf->capacity = new_size;
    if (buf->end_off < buf->start_off) {
        uint8_t *move_start = buf->data + buf->start_off;
        size_t move_size = old_size - buf->start_off;
        uint8_t *move_dest = buf->data + buf->capacity - move_size;
        memmove(move_dest, move_start, move_size);
    }
    return 0;
}

inline void quicly_ringbuf_shift(quicly_ringbuf_t *buf, size_t amount) {
    buf->start_off += amount;
    if (buf->start_off >= buf->capacity) {
        buf->start_off -= buf->capacity;
    }
}

inline int quicly_ringbuf_emit(quicly_ringbuf_t *buf, size_t off, void *dst, size_t *len, int *wrote_all) {
    size_t data_available = quicly_ringbuf_used(buf);

    assert(off < data_available);

    if (off + *len < data_available) {
        *wrote_all = 0;
    } else {
        *len = data_available - off;
        *wrote_all = 1;
    }

    size_t cpy_start_off = buf->start_off + off;
    size_t cpy_end_off = cpy_start_off + *len;
    if (cpy_start_off >= buf->capacity)
        cpy_start_off -= buf->capacity;
    if (cpy_end_off >= buf->capacity)
        cpy_end_off -= buf->capacity;

    if (cpy_end_off < cpy_start_off) {
        // Need 2 memcpy
        size_t split = buf->capacity - cpy_start_off;
        memcpy(dst, buf->data + cpy_start_off, split);
        memcpy(dst + split, buf->data, cpy_end_off);
    } else {
        // Simple case
        memcpy(dst, buf->data + cpy_start_off, *len);
    }
    return 0;
}

inline int quicly_ringbuf_push(quicly_ringbuf_t *buf, const void *src, size_t len) {
    size_t space_available = quicly_ringbuf_available(buf);
    int ret = 0;

    if (space_available < len) {
        if ((ret = quicly_ringbuf_grow(buf, len - space_available)) != 0)
            return ret;
    }

    size_t cpy_start_off = buf->end_off;
    size_t cpy_end_off = cpy_start_off + len;
    if (cpy_end_off >= buf->capacity)
        cpy_end_off -= buf->capacity;

    if (cpy_end_off < cpy_start_off) {
        // Need 2 memcpy
        size_t split = buf->capacity - cpy_start_off;
        memcpy(buf->data + cpy_start_off, src, split);
        memcpy(buf->data, src + split, cpy_end_off);
    } else {
        // Simple case
        memcpy(buf->data + cpy_start_off, src, len);
    }
    return 0;
}

inline int quicly_ringbuf_set(quicly_ringbuf_t *buf, size_t off, const void *src, size_t len) {
    int ret = 0;
    if (off + len > buf->capacity) {
        if ((ret = quicly_ringbuf_grow(buf, off + len - buf->capacity)) != 0)
            return ret;
    }

    size_t cpy_start_off = buf->start_off + off;
    size_t cpy_end_off = cpy_start_off + len;
    if (cpy_start_off >= buf->capacity)
        cpy_start_off -= buf->capacity;
    if (cpy_end_off >= buf->capacity)
        cpy_end_off -= buf->capacity;

    if (cpy_end_off < cpy_start_off) {
        // Need 2 memcpy
        size_t split = buf->capacity - cpy_start_off;
        memcpy(buf->data + cpy_start_off, src, split);
        memcpy(buf->data, src + split, cpy_end_off);
    } else {
        // Simple case
        memcpy(buf->data + cpy_start_off, src, len);
    }

    if (off + len > quicly_ringbuf_used(buf))
        buf->end_off = cpy_end_off;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
