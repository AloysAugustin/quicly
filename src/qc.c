#include <getopt.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <ev.h>

#include "quicly.h"
#include "quicly/streambuf.h"
#include "../deps/picotls/t/util.h"



static unsigned verbosity = 0;

static void hexdump(const char *title, const uint8_t *p, size_t l)
{
    fprintf(stderr, "%s (%zu bytes):\n", title, l);

    while (l != 0) {
        int i;
        fputs("   ", stderr);
        for (i = 0; i < 16; ++i) {
            fprintf(stderr, " %02x", *p++);
            if (--l == 0)
                break;
        }
        fputc('\n', stderr);
    }
}

static ptls_handshake_properties_t hs_properties;
static quicly_context_t ctx;
static ptls_context_t tlsctx = {ptls_openssl_random_bytes,
                                &ptls_get_time,
                                ptls_openssl_key_exchanges,
                                ptls_openssl_cipher_suites,
                                {NULL},
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                0,
                                0,
                                NULL,
                                1};
 
static int on_stop_sending(quicly_stream_t *stream, uint16_t error_code);
static int on_receive_reset(quicly_stream_t *stream, uint16_t error_code);
static int server_on_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len);
static int client_on_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len);

static const quicly_stream_callbacks_t server_stream_callbacks = {quicly_streambuf_destroy,
                                                                  quicly_streambuf_egress_shift,
                                                                  quicly_streambuf_egress_emit,
                                                                  on_stop_sending,
                                                                  server_on_receive,
                                                                  on_receive_reset},
                                       client_stream_callbacks = {quicly_streambuf_destroy,
                                                                  quicly_streambuf_egress_shift,
                                                                  quicly_streambuf_egress_emit,
                                                                  on_stop_sending,
                                                                  client_on_receive,
                                                                  on_receive_reset};

static void print_stats(quicly_conn_t *conn) {
    const quicly_cid_t *host_cid = quicly_get_host_cid(conn);
    char *host_cid_hex = quicly_hexdump(host_cid->cid, host_cid->len, SIZE_MAX);
    uint64_t num_received, num_sent, num_lost, num_ack_received, num_bytes_sent;
    quicly_get_packet_stats(conn, &num_received, &num_sent, &num_lost, &num_ack_received, &num_bytes_sent);
    fprintf(stderr,
            "conn:%s: received: %" PRIu64 ", sent: %" PRIu64 ", lost: %" PRIu64 ", ack-received: %" PRIu64
            ", bytes-sent: %" PRIu64 "\n",
            host_cid_hex, num_received, num_sent, num_lost, num_ack_received, num_bytes_sent);
    free(host_cid_hex);
}

static int on_stop_sending(quicly_stream_t *stream, uint16_t error_code)
{
    fprintf(stderr, "received STOP_SENDING: %" PRIu16 "\n", error_code);
    return 0;
}

static int on_receive_reset(quicly_stream_t *stream, uint16_t error_code)
{
    fprintf(stderr, "received RESET_STREAM: %" PRIu16 "\n", error_code);
    return 0;
}

static int server_on_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    client_on_receive(stream, off, src, len);
    //quicly_streambuf_egress_shutdown(stream);
    return 0;
}

static int client_on_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    ptls_iovec_t input;
    int ret;

    if ((ret = quicly_streambuf_ingress_receive(stream, off, src, len)) != 0)
        return ret;

    if ((input = quicly_streambuf_ingress_get(stream)).len != 0) {
        fwrite(input.base, 1, input.len, stdout);
        fflush(stdout);
        quicly_streambuf_ingress_shift(stream, input.len);
    }

    if (quicly_recvstate_transfer_complete(&stream->recvstate)) {
        static size_t num_resp_received;
        ++num_resp_received;
        fprintf(stderr, "transfer complete\n");
    }

    return 0;
}

static int on_stream_open(quicly_stream_t *stream)
{
    int ret;
    fprintf(stderr, "Stream opened!\n");
    if ((ret = quicly_streambuf_create(stream, sizeof(quicly_streambuf_t))) != 0)
        return ret;
    stream->callbacks = ctx.tls->certificates.count != 0 ? &server_stream_callbacks : &client_stream_callbacks;
    return 0;
}


static void on_conn_close(quicly_conn_t *conn, uint16_t code, const uint64_t *frame_type, const char *reason, size_t reason_len)
{
    fprintf(stderr, "%s close:0x%" PRIx16 ":%.*s\n", frame_type != NULL ? "connection" : "application", code, (int)reason_len,
            reason);
}


static int send_one(int fd, quicly_datagram_t *p)
{
    int ret;
    struct msghdr mess;
    struct iovec vec;
    memset(&mess, 0, sizeof(mess));
    mess.msg_name = &p->sa;
    mess.msg_namelen = p->salen;
    vec.iov_base = p->data.base;
    vec.iov_len = p->data.len;
    mess.msg_iov = &vec;
    mess.msg_iovlen = 1;
    if (verbosity >= 2)
        hexdump("sendmsg", vec.iov_base, vec.iov_len);
    ret = (int)sendmsg(fd, &mess, 0);
    return ret;
}

static int send_pending(int fd, quicly_conn_t *conn)
{
    quicly_datagram_t *packets[16];
    size_t num_packets, i;
    int ret;

    do {
        num_packets = sizeof(packets) / sizeof(packets[0]);
        if ((ret = quicly_send(conn, packets, &num_packets)) == 0) {
            // fprintf(stderr, "%ld packets to send\n", num_packets);
            for (i = 0; i != num_packets; ++i) {
                if ((ret = send_one(fd, packets[i])) == -1)
                    perror("sendmsg failed");
                ret = 0;
                quicly_default_free_packet(&ctx, packets[i]);
                print_stats(conn);
            }
        } else {
            fprintf(stderr, "quicly_send returned %d\n", ret);
        }
    } while (ret == 0 && num_packets == sizeof(packets) / sizeof(packets[0]));

    return ret;
}

static void set_alpn(ptls_handshake_properties_t *pro, const char *alpn_str)
{
    const char *start, *cur;
    ptls_iovec_t *list = NULL;
    size_t entries = 0;
    start = cur = alpn_str;
#define ADD_ONE()                                                                                                                  \
    if ((cur - start) > 0) {                                                                                                       \
        list = realloc(list, sizeof(*list) * (entries + 1));                                                                       \
        list[entries].base = (void *)strndup(start, cur - start);                                                                  \
        list[entries++].len = cur - start;                                                                                         \
    }

    while (*cur) {
        if (*cur == ',') {
            ADD_ONE();
            start = cur + 1;
        }
        cur++;
    }
    if (start != cur)
        ADD_ONE();

    pro->client.negotiated_protocols.list = list;
    pro->client.negotiated_protocols.count = entries;
}


static int open_stream_if_ready(quicly_conn_t *conn, quicly_stream_t **stream) {
    if (*stream) {
        return 0;
    }
    if (quicly_connection_is_ready(conn)) {
        return quicly_open_stream(conn, stream, 0);
    }
    return 0;
}


static int run_client(struct sockaddr *sa, socklen_t salen, const char *host)
{
    int fd, ret;
    struct sockaddr_in local;
    quicly_conn_t *conn = NULL;

    fprintf(stderr, "Starting client\n");

    if ((fd = socket(sa->sa_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        perror("socket(2) failed");
        return 1;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    if (bind(fd, (void *)&local, sizeof(local)) != 0) {
        perror("bind(2) failed");
        return 1;
    }
    ret = quicly_connect(&conn, &ctx, host, sa, salen, &hs_properties, NULL);
    assert(ret == 0);
    send_pending(fd, conn);

    quicly_stream_t *stream = NULL;
    ret = open_stream_if_ready(conn, &stream);
    assert(ret == 0);

    while (1) {
        fd_set readfds;
        struct timeval *tv, tvbuf;
        do {
            int64_t timeout_at = conn != NULL ? quicly_get_first_timeout(conn) : INT64_MAX;
            if (timeout_at != INT64_MAX) {
                int64_t delta = timeout_at - quicly_get_context(conn)->now(quicly_get_context(conn));
                if (delta > 0) {
                    tvbuf.tv_sec = delta / 1000;
                    tvbuf.tv_usec = (delta % 1000) * 1000;
                } else {
                    tvbuf.tv_sec = 0;
                    tvbuf.tv_usec = 0;
                }
                tv = &tvbuf;
            } else {
                tv = NULL;
            }
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            FD_SET(STDIN_FILENO, &readfds);
        } while (select(fd + 1, &readfds, NULL, NULL, tv) == -1 && errno == EINTR);

        if (FD_ISSET(fd, &readfds)) {
            uint8_t buf[4096];
            struct msghdr mess;
            struct sockaddr sa;
            struct iovec vec;
            memset(&mess, 0, sizeof(mess));
            mess.msg_name = &sa;
            mess.msg_namelen = sizeof(sa);
            vec.iov_base = buf;
            vec.iov_len = sizeof(buf);
            mess.msg_iov = &vec;
            mess.msg_iovlen = 1;
            ssize_t rret;
            while ((rret = recvmsg(fd, &mess, 0)) <= 0)
                ;
            if (verbosity >= 2)
                hexdump("recvmsg", buf, rret);
            size_t off = 0;
            while (off != rret) {
                quicly_decoded_packet_t packet;
                size_t plen = quicly_decode_packet(&packet, buf + off, rret - off, 0);
                if (plen == SIZE_MAX)
                    break;
                quicly_receive(conn, &packet);
                off += plen;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            // Send data in first stream, but only once it is open, and if there is not too much stuff in the buffer
            if (stream) {
                quicly_streambuf_t *sbuf = stream->data;
                if (quicly_ringbuf_used(&sbuf->egress.buf) < 16*1023*1024) {
                    uint8_t buf[1024];
                    int32_t len = read(STDIN_FILENO, buf, 1024);
                    if (len <= 0) {
                        fprintf(stderr, "Read from stdin returned %d %s", len, strerror(errno));
                        return 1;
                    }

                    int err = quicly_streambuf_egress_write(stream, buf, len);
                    assert(err == 0);
                }
            }
        }

        ret = open_stream_if_ready(conn, &stream);
        assert(ret == 0);

        if (conn != NULL && send_pending(fd, conn) != 0) {
            quicly_free(conn);
            conn = NULL;
            return 1;
        }
    }
}

static quicly_conn_t **conns;
static size_t num_conns = 0;

static void on_signal(int signo)
{
    size_t i;
    for (i = 0; i != num_conns; ++i) {
        print_stats(conns[i]);
    }
    if (signo == SIGINT)
        _exit(0);
}

static int run_server(struct sockaddr *sa, socklen_t salen)
{
    int fd;

    if ((fd = socket(sa->sa_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        perror("socket(2) failed");
        return 1;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        return 1;
    }
    if (bind(fd, sa, salen) != 0) {
        perror("bind(2) failed");
        return 1;
    }

    while (1) {
        fd_set readfds;
        struct timeval *tv, tvbuf;
        do {
            int64_t timeout_at = INT64_MAX;
            size_t i;
            for (i = 0; i != num_conns; ++i) {
                int64_t conn_to = quicly_get_first_timeout(conns[i]);
                if (conn_to < timeout_at)
                    timeout_at = conn_to;
            }
            if (timeout_at != INT64_MAX) {
                int64_t delta = timeout_at - ctx.now(&ctx);
                if (delta > 0) {
                    tvbuf.tv_sec = delta / 1000;
                    tvbuf.tv_usec = (delta % 1000) * 1000;
                } else {
                    tvbuf.tv_sec = 0;
                    tvbuf.tv_usec = 0;
                }
                tv = &tvbuf;
            } else {
                tv = NULL;
            }
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
        } while (select(fd + 1, &readfds, NULL, NULL, tv) == -1 && errno == EINTR);
        if (FD_ISSET(fd, &readfds)) {
            uint8_t buf[4096];
            struct msghdr mess;
            struct sockaddr sa;
            struct iovec vec;
            memset(&mess, 0, sizeof(mess));
            mess.msg_name = &sa;
            mess.msg_namelen = sizeof(sa);
            vec.iov_base = buf;
            vec.iov_len = sizeof(buf);
            mess.msg_iov = &vec;
            mess.msg_iovlen = 1;
            ssize_t rret;
            do {
                rret = recvmsg(fd, &mess, MSG_DONTWAIT);
                if (rret < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        fprintf(stderr, "recvmsg error: %s", strerror(errno));
                        exit(1);
                    }
                }
                if (verbosity >= 2)
                    hexdump("recvmsg", buf, rret);
                size_t off = 0;
                while (off != rret) {
                    quicly_decoded_packet_t packet;
                    size_t plen = quicly_decode_packet(&packet, buf + off, rret - off, 8);
                    if (plen == SIZE_MAX)
                        break;
                    quicly_conn_t *conn = NULL;
                    size_t i;
                    for (i = 0; i != num_conns; ++i) {
                        if (quicly_is_destination(conns[i], (packet.octets.base[0] & 0x80) == 0, packet.cid.dest)) {
                            conn = conns[i];
                            break;
                        }
                    }
                    if (conn != NULL) {
                        /* existing connection */
                        quicly_receive(conn, &packet);
                    } else {
                        /* new connection */
                        int ret = quicly_accept(&conn, &ctx, &sa, mess.msg_namelen, NULL, &packet);
                        if (ret == 0) {
                            assert(conn != NULL);
                            conns = realloc(conns, sizeof(*conns) * (num_conns + 1));
                            assert(conns != NULL);
                            conns[num_conns++] = conn;
                        } else {
                            assert(conn == NULL);
                            if (ret == QUICLY_ERROR_VERSION_NEGOTIATION) {
                                quicly_datagram_t *rp =
                                    quicly_send_version_negotiation(&ctx, &sa, salen, packet.cid.src, packet.cid.dest);
                                assert(rp != NULL);
                                if (send_one(fd, rp) == -1)
                                    perror("sendmsg failed");
                            }
                        }
                    }
                    off += plen;
                }
            } while (1);
        }
        {
            size_t i;
            for (i = 0; i != num_conns; ++i) {
                if (quicly_get_first_timeout(conns[i]) <= ctx.now(&ctx)) {
                    if (send_pending(fd, conns[i]) != 0) {
                        quicly_free(conns[i]);
                        memmove(conns + i, conns + i + 1, (num_conns - i - 1) * sizeof(*conns));
                        --i;
                        --num_conns;
                    }
                }
            }
        }
    }
}

static void usage(const char *cmd)
{
    printf("Usage: %s [options] host port\n"
           "\n"
           "Options:\n"
           "  -a <alpn list>       a coma separated list of ALPN identifiers\n"
           "  -c certificate-file\n"
           "  -k key-file          specifies the credentials to be used for running the\n"
           "                       server. If omitted, the command runs as a client.\n"
           "  -l log-file          file to log traffic secrets\n"
           "  -n                   enforce version negotiation (client-only)\n"
           "  -p path              path to request (can be set multiple times)\n"
           "  -r [initial-rto]     initial RTO (in milliseconds)\n"
           "  -S [secret]          use stateless retry protected by the secret\n"
           "  -s session-file      file to load / store the session ticket\n"
           "  -V                   verify peer using the default certificates\n"
           "  -v                   verbose mode (-vv emits packet dumps as well)\n"
           "  -h                   print this help\n"
           "\n",
           cmd);
}

int main(int argc, char **argv)
{
    const char *host, *port;
    struct sockaddr_storage sa;
    socklen_t salen;
    int ch;

    ctx = quicly_default_context;
    ctx.tls = &tlsctx;
    ctx.on_stream_open = on_stream_open;
    ctx.on_conn_close = on_conn_close;

    setup_session_cache(ctx.tls);
    quicly_amend_ptls_context(ctx.tls);

    while ((ch = getopt(argc, argv, "a:c:k:l:nr:s:Vvh")) != -1) {
        switch (ch) {
        case 'a':
            set_alpn(&hs_properties, optarg);
            break;
        case 'c':
            load_certificate_chain(ctx.tls, optarg);
            break;
        case 'k':
            load_private_key(ctx.tls, optarg);
            break;
        case 'l':
            setup_log_secret(ctx.tls, optarg);
            break;
        case 'n':
            ctx.enforce_version_negotiation = 1;
            break;
        case 'r':
            if (sscanf(optarg, "%" PRIu32, &ctx.loss->default_initial_rtt) != 1) {
                fprintf(stderr, "invalid argument passed to `-r`\n");
                exit(1);
            }
            break;
        case 's':
            setup_session_file(ctx.tls, &hs_properties, optarg);
            break;
        case 'V':
            setup_verify_certificate(ctx.tls);
            break;
        case 'v':
            ++verbosity;
            break;
        default:
            usage(argv[0]);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    ctx.event_log.mask = UINT64_MAX;
    ctx.event_log.cb = quicly_default_event_log;
    quicly_default_event_log_fp = stderr;

    if (ctx.tls->certificates.count != 0 || ctx.tls->sign_certificate != NULL) {
        /* server */
        if (ctx.tls->certificates.count == 0 || ctx.tls->sign_certificate == NULL) {
            fprintf(stderr, "-ck and -k options must be used together\n");
            exit(1);
        }
    } else {
        /* client */
    }
    if (argc != 2) {
        fprintf(stderr, "missing host and port\n");
        exit(1);
    }
    host = (--argc, *argv++);
    port = (--argc, *argv++);

    if (resolve_address((void *)&sa, &salen, host, port, AF_INET, SOCK_DGRAM, IPPROTO_UDP) != 0)
        exit(1);

    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    return ctx.tls->certificates.count != 0 ? run_server((void *)&sa, salen) : run_client((void *)&sa, salen, host);
}

