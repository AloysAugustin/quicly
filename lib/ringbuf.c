#include "quicly/ringbuf.h"

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
    buf->data = NULL;
}

inline size_t quicly_ringbuf_available(quicly_ringbuf_t *buf) {
    if (buf->end_off < buf->start_off) {
        return buf->start_off - buf->end_off - 1;
    } else {
        return buf->capacity - buf->end_off + buf->start_off - 1;
    }
}

inline size_t quicly_ringbuf_used(quicly_ringbuf_t *buf) {
    if (buf->end_off >= buf->start_off) {
        return buf->end_off - buf->start_off;
    } else {
        return buf->capacity - buf->start_off + buf->end_off;
    }
}

inline size_t quicly_ringbuf_used_one_block(quicly_ringbuf_t *buf) {
    if (buf->end_off >= buf->start_off) {
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
        buf->start_off += new_size - old_size;
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

    assert(off <= data_available);

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
    buf->end_off = cpy_end_off;
    return 0;
}

inline int quicly_ringbuf_set(quicly_ringbuf_t *buf, size_t off, const void *src, size_t len) {
    int ret = 0;
    if (off + len + 1 > buf->capacity) {
        if ((ret = quicly_ringbuf_grow(buf, off + len + 1 - buf->capacity)) != 0)
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
