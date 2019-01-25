#include <string.h>
#include "quicly/streambuf.h"
#include "test.h"

void test_ringbuf(void) {
    int ret = 0;
    quicly_ringbuf_t b;

    ret = quicly_ringbuf_init(&b, 128);
    ok(ret == 0);

    const char *test_data = "AZERTYUIOPQSDFGHJKL"; // 19 bytes
    const size_t data_len = 19;

    // Test expansion
    for (int i = 0; i < 10; i ++) {
        ok(quicly_ringbuf_used(&b) == i * data_len);
        quicly_ringbuf_push(&b, test_data, data_len);
    }

    for (int i = 0; i < 10; i ++) {
        char temp[data_len];
        size_t len = data_len;
        int wrote_all;
        quicly_ringbuf_emit(&b, i * data_len, temp, &len, &wrote_all);
        ok(len == data_len);
        ok(memcmp(temp, test_data, data_len) == 0);
        if (i < 9) {
            ok(wrote_all == 0);
        } else {
            ok(wrote_all == 1);
        }
    }

    quicly_ringbuf_shift(&b, 2 * data_len);
    ok(quicly_ringbuf_used(&b) == 8 * data_len);

    quicly_ringbuf_shift(&b, 6 * data_len);
    ok(quicly_ringbuf_used(&b) == 2 * data_len);

    // Test wrapping around
    for (int i = 0; i < 10; i ++) {
        ok(quicly_ringbuf_used(&b) == (i + 2) * data_len);
        quicly_ringbuf_push(&b, test_data, data_len);
    }
    
    {
        char temp[12 * data_len];
        size_t len = 12 * data_len;
        int wrote_all;
        quicly_ringbuf_emit(&b, 0, temp, &len, &wrote_all);
        ok(len == 12 * data_len);
        for (int i = 0; i < 12; i ++) {
            ok(memcmp(temp + (data_len * i), test_data, data_len) == 0);
        }
        ok(wrote_all == 1);
    }
    // Make sure that we did wrap around
    ok(quicly_ringbuf_used_one_block(&b) != quicly_ringbuf_used(&b));

    // Test growth with wrap around. Size should be 256 now and 12*19 = 228 bytes are used, we need to add at least 2 * 19 bytes
    for (int i = 0; i < 4; i ++) {
        ok(quicly_ringbuf_used(&b) == (i + 12) * data_len);
        quicly_ringbuf_push(&b, test_data, data_len);
    }

    for (int i = 0; i < 16; i ++) {
        char temp[data_len];
        size_t len = data_len;
        int wrote_all;
        quicly_ringbuf_emit(&b, i * data_len, temp, &len, &wrote_all);
        ok(len == data_len);
        ok(memcmp(temp, test_data, data_len) == 0);
        if (i < 15) {
            ok(wrote_all == 0);
        } else {
            ok(wrote_all == 1);
        }
    }

    // Test set without growth
    quicly_ringbuf_set(&b, 128, test_data, data_len);
    ok(quicly_ringbuf_used(&b) == 16 * data_len);
    {
        char temp[data_len];
        size_t len = data_len;
        int wrote_all;
        quicly_ringbuf_emit(&b, 128, temp, &len, &wrote_all);
        ok(len == data_len);
        ok(memcmp(temp, test_data, data_len) == 0);
        ok(wrote_all == 0);
    }

    // Now set with growth
    quicly_ringbuf_set(&b, 512, test_data, data_len);
    ok(quicly_ringbuf_used(&b) == 512 + data_len);
    {
        char temp[data_len];
        size_t len = data_len;
        int wrote_all;
        quicly_ringbuf_emit(&b, 512, temp, &len, &wrote_all);
        ok(len == data_len);
        ok(memcmp(temp, test_data, data_len) == 0);
        ok(wrote_all == 1);
        quicly_ringbuf_emit(&b, 0, temp, &len, &wrote_all);
        ok(len == data_len);
        ok(memcmp(temp, test_data, data_len) == 0);
        ok(wrote_all == 0);
    }

    quicly_ringbuf_shift(&b, 512);
    ok(quicly_ringbuf_used(&b) == data_len);

    // Test zero emit at end
    {
        char temp[data_len];
        size_t len = data_len;
        int wrote_all;
        quicly_ringbuf_emit(&b, 19, temp, &len, &wrote_all);
        ok(len == 0);
        ok(wrote_all == 1);
    }

    quicly_ringbuf_dispose(&b);
}
