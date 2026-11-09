/* Opt-in public broker regression. Never registered as an offline CTest. */
#include <nng/nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <nng/mqtt/mqtt_quic_client.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static nng_pipe current;
static unsigned connections;
static const char *phase;
static void check(int rv)
{
    if (rv) { fprintf(stderr, "FAIL %s: %s (%d)\n", phase, nng_strerror(rv), rv); exit(1); }
}
static void require(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL %s: %s\n", phase, what); exit(1); }
}
static void pipe_event(nng_pipe p, nng_pipe_ev ev, void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    if (ev == NNG_PIPE_EV_ADD_POST) { current = p; ++connections; }
    pthread_mutex_unlock(&lock);
}
static nng_msg *packet(int type)
{
    nng_msg *m;
    check(nng_mqtt_msg_alloc(&m, 0));
    nng_mqtt_msg_set_packet_type(m, type);
    return m;
}
static void check_publish_ack(nng_msg *m, nng_mqtt_packet_type expected, unsigned index)
{
    nng_mqtt_packet_type actual = m ? nng_mqtt_msg_get_packet_type(m) : 0;
    const uint8_t *body = m ? nng_msg_body(m) : NULL;
    size_t len = m ? nng_msg_len(m) : 0;
    unsigned pid = len >= 2 ? ((unsigned)body[0] << 8) | body[1] : 0;
    unsigned reason = len >= 3 ? body[2] : 0;
    if (actual != expected || reason >= 0x80) {
        fprintf(stderr, "ACK rejected/mismatched: index=%u expected=%u actual=%u packet_id=%u reason=0x%02x body_length=%zu\n",
            index, (unsigned)expected, (unsigned)actual, pid, reason, len);
        require(0, "publication was not acknowledged successfully");
    }
}
static nng_msg *send_packet(nng_socket s, nng_msg *m, nng_mqtt_packet_type ack)
{
    nng_aio *aio;
    check(nng_aio_alloc(&aio, NULL, NULL));
    nng_aio_set_timeout(aio, 12000);
    nng_aio_set_msg(aio, m);
    nng_send_aio(s, aio);
    nng_aio_wait(aio);
    int rv = nng_aio_result(aio);
    m = nng_aio_get_msg(aio);
    nng_aio_set_msg(aio, NULL);
    nng_aio_free(aio);
    if (rv && m) { nng_msg_free(m); m = NULL; }
    check(rv);
    if (ack == NNG_MQTT_PUBACK || ack == NNG_MQTT_PUBCOMP)
        check_publish_ack(m, ack, 0);
    else if (ack) require(m && nng_mqtt_msg_get_packet_type(m) == ack, "wrong/missing acknowledgement");
    else require(m == NULL, "unexpected acknowledgement");
    return m;
}
static void subscribe_stream(nng_socket s, const char *topic, unsigned stream)
{
    phase = "SUBSCRIBE/SUBACK";
    nng_msg *m = packet(NNG_MQTT_SUBSCRIBE);
    check(nng_mqtt_quic_msg_set_stream(m, stream));
    nng_mqtt_topic_qos *t = nng_mqtt_topic_qos_array_create(1);
    require(t != NULL, "allocate topic");
    nng_mqtt_topic_qos_array_set(t, 0, topic, (uint32_t)strlen(topic), 2, 0, 0, 0);
    nng_mqtt_msg_set_subscribe_topics(m, t, 1);
    nng_mqtt_topic_qos_array_free(t, 1);
    m = send_packet(s, m, NNG_MQTT_SUBACK);
    uint32_t count;
    uint8_t *codes = nng_mqtt_msg_get_suback_return_codes(m, &count);
    require(count == 1 && codes && codes[0] == 2, "subscription QoS 2 rejected");
    nng_msg_free(m);
    puts("PASS SUBACK QoS 2");
}
static void exchange(nng_socket s, const char *topic, int qos, size_t size, unsigned sequence)
{
    phase = "publish and receive exact payload";
    uint8_t *data = malloc(size ? size : 1);
    require(data != NULL, "allocate payload");
    for (size_t i = 0; i < size; ++i) data[i] = (uint8_t)(i * 31 + sequence);
    nng_msg *m = packet(NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_qos(m, qos);
    nng_mqtt_msg_set_publish_topic(m, topic);
    nng_mqtt_msg_set_publish_payload(m, data, (uint32_t)size);
    m = send_packet(s, m, qos == 1 ? NNG_MQTT_PUBACK : qos == 2 ? NNG_MQTT_PUBCOMP : 0);
    if (m) nng_msg_free(m);
    check(nng_recvmsg(s, &m, 0));
    require(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBLISH, "expected PUBLISH");
    uint32_t len, topic_len;
    uint8_t *actual = nng_mqtt_msg_get_publish_payload(m, &len);
    const char *actual_topic = nng_mqtt_msg_get_publish_topic(m, &topic_len);
    require(topic_len == strlen(topic) && memcmp(actual_topic, topic, topic_len) == 0, "topic mismatch");
    require(len == size && (!size || memcmp(actual, data, size) == 0), "payload mismatch");
    require(nng_mqtt_msg_get_publish_qos(m) == qos, "receive QoS mismatch");
    nng_msg_free(m);
    free(data);
    printf("PASS publish/receive QoS=%d bytes=%zu sequence=%u\n", qos, size, sequence);
}

static void burst(nng_socket s, const char *topic)
{
    nng_aio *aio[32];
    const char *setting = getenv("NNG_BROKER_TEST_WINDOW");
    int window = setting ? atoi(setting) : 4;
    const char *pace = getenv("NNG_BROKER_TEST_PAUSE_MS");
    int pause_ms = pace ? atoi(pace) : 1000;
    require(pause_ms >= 0 && pause_ms <= 10000, "NNG_BROKER_TEST_PAUSE_MS must be 0..10000");
    require(window > 0 && window <= 32, "NNG_BROKER_TEST_WINDOW must be 1..32");
    phase = "32 QoS1 sends with bounded concurrency and per-topic ordering";
    for (int batch = 0; batch < 32; batch += window) {
      /* Public brokers may discard routed traffic under a burst limit even
       * while acknowledging ingress. Keep the default smoke test low-rate;
       * PAUSE_MS=0/WINDOW=32 retains the explicit stress scenario. */
      struct timespec pause = {pause_ms / 1000, (pause_ms % 1000) * 1000000L};
      nanosleep(&pause, NULL);
      int end = batch + window < 32 ? batch + window : 32;
      for (uint8_t i = batch; i < end; ++i) {
        check(nng_aio_alloc(&aio[i], NULL, NULL));
        nng_aio_set_timeout(aio[i], 12000);
        nng_msg *m = packet(NNG_MQTT_PUBLISH);
        nng_mqtt_msg_set_publish_topic(m, topic);
        nng_mqtt_msg_set_publish_qos(m, 1);
        nng_mqtt_msg_set_publish_payload(m, &i, 1);
        nng_aio_set_msg(aio[i], m);
        nng_send_aio(s, aio[i]);
    }
      for (uint8_t i = batch; i < end; ++i) {
        printf("WAIT burst PUBACK index=%u\n", i);
        nng_aio_wait(aio[i]);
        check(nng_aio_result(aio[i]));
        nng_msg *m = nng_aio_get_msg(aio[i]);
        require(m && nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBACK, "burst PUBACK missing");
        nng_aio_set_msg(aio[i], NULL);
        nng_msg_free(m);
        nng_aio_free(aio[i]);
        printf("WAIT burst delivery index=%u\n", i);
        check(nng_recvmsg(s, &m, 0));
        require(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBLISH, "burst PUBLISH missing");
        uint32_t len;
        uint8_t *data = nng_mqtt_msg_get_publish_payload(m, &len);
        require(len == 1 && data[0] == i, "burst payload out of order");
        nng_msg_free(m);
    }
    }
    printf("PASS 32 QoS1 sends, ACKs and ordered delivery; window=%d pause_ms=%d\n", window, pause_ms);
}


/* A separate receiver keeps the MQTT round-trip out of the sending cadence.
 * Each round sends QoS1 then QoS2 at 100ms intervals, without waiting for ACKs
 * between messages. The sender checks every ACK; the receiver checks every
 * payload, topic, QoS, sequence and duplicate. */
struct rate_receive {
    nng_socket socket;
    const char *topic;
    const char *topic2;
    unsigned per_qos;
    unsigned received;
};
static void *rate_receiver(void *arg)
{
    struct rate_receive *r = arg;
    unsigned total = r->per_qos * 2;
    unsigned char *seen = calloc(total, 1);
    require(seen != NULL, "allocate rate receive bitmap");
    for (unsigned i = 0; i < total; ++i) {
        nng_msg *m;
        int rv = nng_recvmsg(r->socket, &m, 0);
        if (rv) {
            fprintf(stderr, "RATE receive failure: received=%u expected=%u missing:", r->received, total);
            for (unsigned j = 0; j < total; ++j) if (!seen[j]) fprintf(stderr, " %u", j);
            fputc('\n', stderr);
        }
        check(rv);
        require(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBLISH, "rate expected PUBLISH");
        uint32_t len, topic_len;
        const uint8_t *data = nng_mqtt_msg_get_publish_payload(m, &len);
        const char *topic = nng_mqtt_msg_get_publish_topic(m, &topic_len);

        require(len == 1024, "rate payload size mismatch");
        uint32_t sequence = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
            ((uint32_t)data[2] << 8) | data[3];
        require(sequence < total && !seen[sequence], "rate unexpected/duplicate sequence");
        const char *expected_topic = r->topic2 && (sequence & 1) ? r->topic2 : r->topic;
        require(topic_len == strlen(expected_topic) && memcmp(topic, expected_topic, topic_len) == 0, "rate topic mismatch");
        if (r->topic2) require(nng_mqtt_quic_msg_get_stream(m) == (sequence & 1) + 1, "rate subscription stream mismatch");
        require(nng_mqtt_msg_get_publish_qos(m) == (sequence < r->per_qos ? 1 : 2), "rate QoS mismatch");
        for (unsigned j = 4; j < len; ++j)
            require(data[j] == (uint8_t)(sequence * 17 + j), "rate payload corruption");
        seen[sequence] = 1;
        ++r->received;
        if (r->received % 50 == 0) printf("RATE received=%u/%u\n", r->received, total);
        nng_msg_free(m);
    }
    free(seen);
    return NULL;
}
static void rate_test(nng_socket s, const char *topic, const char *topic2, unsigned per_qos)
{
    phase = "sustained QoS1 and QoS2";
    const char *setting = getenv("NNG_BROKER_TEST_RATE");
    unsigned rate = setting ? (unsigned)atoi(setting) : 10;
    require(rate >= 1 && rate <= 100, "RATE must be 1..100");
    unsigned total = per_qos * 2;
    nng_aio **aio = calloc(total, sizeof(*aio));
    require(aio != NULL, "allocate rate send slots");
    struct rate_receive receive = {s, topic, topic2, per_qos, 0};
    pthread_t thread;
    require(pthread_create(&thread, NULL, rate_receiver, &receive) == 0, "start rate receiver");
    struct timespec start, deadline, end;
    require(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "read monotonic clock");
    deadline = start;
    printf("RATE START qos1=%u qos2=%u target=%u/s payload=1024\n", per_qos, per_qos, rate);
    for (unsigned i = 0; i < total; ++i) {
        deadline.tv_nsec += 1000000000L / rate;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        int rv;
        do { rv = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL); } while (rv == EINTR);
        require(rv == 0, "rate pacing clock");
        check(nng_aio_alloc(&aio[i], NULL, NULL));
        nng_aio_set_timeout(aio[i], 12000);
        nng_msg *m = packet(NNG_MQTT_PUBLISH);
        nng_mqtt_msg_set_publish_topic(m, topic2 && (i & 1) ? topic2 : topic);
        if (topic2) check(nng_mqtt_quic_msg_set_stream(m, (i & 1) + 3));
        nng_mqtt_msg_set_publish_qos(m, i < per_qos ? 1 : 2);
        uint8_t data[1024];
        data[0] = (uint8_t)(i >> 24); data[1] = (uint8_t)(i >> 16);
        data[2] = (uint8_t)(i >> 8); data[3] = (uint8_t)i;
        for (unsigned j = 4; j < sizeof(data); ++j) data[j] = (uint8_t)(i * 17 + j);
        nng_mqtt_msg_set_publish_payload(m, data, sizeof(data));
        nng_aio_set_msg(aio[i], m);
        nng_send_aio(s, aio[i]);
        // Do not catch up with a burst after scheduling/network delays.
        // Keep at least one full interval between submissions.
        if (getenv("NNG_BROKER_TEST_MIN_SPACING"))
            require(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0, "read last submission time");
    }
    require(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "read rate end clock");
    double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    for (unsigned i = 0; i < total; ++i) {
        nng_aio_wait(aio[i]);
        int rv = nng_aio_result(aio[i]);
        if (rv) fprintf(stderr, "RATE ACK failure: index=%u qos=%d\n", i, i < per_qos ? 1 : 2);
        check(rv);
        nng_msg *m = nng_aio_get_msg(aio[i]);
        check_publish_ack(m, i < per_qos ? NNG_MQTT_PUBACK : NNG_MQTT_PUBCOMP, i);
        nng_msg_free(m);
        nng_aio_set_msg(aio[i], NULL);
        nng_aio_free(aio[i]);
    }
    printf("RATE ACKED=%u sent=%u elapsed=%.3fs rate=%.3f/s\n", total, total, elapsed, total / elapsed);
    require(pthread_join(thread, NULL) == 0, "join rate receiver");
    require(receive.received == total, "rate delivery count mismatch");
    require(total / elapsed >= rate * 0.95 && total / elapsed <= rate * 1.05, "sending cadence outside requested tolerance");
    printf("PASS RATE sent=%u acked=%u received=%u qos1=%u qos2=%u elapsed=%.3fs rate=%.3f/s duplicates=0\n",
        total, total, receive.received, per_qos, per_qos, elapsed, total / elapsed);
    free(aio);
}


static void unsubscribe_stream(nng_socket s, const char *topic, unsigned stream)
{
    nng_msg *m = packet(NNG_MQTT_UNSUBSCRIBE);
    nng_mqtt_topic *t = nng_mqtt_topic_array_create(1);
    require(t != NULL, "allocate unsubscribe topic");
    nng_mqtt_topic_array_set(t, 0, topic);
    nng_mqtt_msg_set_unsubscribe_topics(m, t, 1);
    nng_mqtt_topic_array_free(t, 1);
    check(nng_mqtt_quic_msg_set_stream(m, stream));
    m = send_packet(s, m, NNG_MQTT_UNSUBACK);
    nng_msg_free(m);
}
static void multi_topic_finish(nng_socket s, const char *first, const char *second)
{
    phase = "independent unsubscribe on two topic streams";
    unsubscribe_stream(s, first, 1);
    exchange(s, second, 2, 127, 201);
    nng_msg *m = packet(NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_topic(m, first);
    nng_mqtt_msg_set_publish_qos(m, 1);
    m = send_packet(s, m, NNG_MQTT_PUBACK); nng_msg_free(m);
    check(nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 500));
    require(nng_recvmsg(s, &m, 0) == NNG_ETIMEDOUT, "unsubscribed stream still delivers");
    check(nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 15000));
    unsubscribe_stream(s, second, 2);
    puts("PASS independent multi-topic stream subscriptions/unsubscriptions");
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s URL VERSION(4|5) MULTISTREAM(0|1) ROUNDS(1..20) [--network-fault]\n", argv[0]);
        return 2;
    }
    int network_fault = argc == 6 && strcmp(argv[5], "--network-fault") == 0;
    if (argc == 6 && !network_fault) return 2;
    int version = atoi(argv[2]), multi = atoi(argv[3]), rounds = atoi(argv[4]);
    if ((version != 4 && version != 5) || (multi != 0 && multi != 1) || rounds < 1 || rounds > 20) return 2;
    printf("START url=%s version=%d multistream=%d rounds=%d\n", argv[1], version, multi, rounds);
    char topic[160], first[180], second[180], client[80];
    snprintf(client, sizeof(client), "nanosdk-%ld-%ld-%d%d", (long)getpid(), (long)time(NULL), version, multi);
    snprintf(topic, sizeof(topic), "nanosdk/quic-regression/%s", client);
    const char *topic_override = getenv("NNG_BROKER_TEST_TOPIC");
    if (topic_override) {
        require(strlen(topic_override) < sizeof(topic), "test topic too long");
        snprintf(topic, sizeof(topic), "%s", topic_override);
    }
    snprintf(first, sizeof(first), "%s/0", topic);
    snprintf(second, sizeof(second), "%s/1", topic);
    nng_socket s;
    nng_dialer d;
    phase = "open/configure";
    check(version == 5 ? nng_mqttv5_quic_client_open(&s) : nng_mqtt_quic_client_open(&s));
    check(nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 15000));
    check(nng_socket_set_ms(s, NNG_OPT_RECONNMINT, 200));
    check(nng_socket_set_ms(s, NNG_OPT_RECONNMAXT, 1000));
    check(nng_pipe_notify(s, NNG_PIPE_EV_ADD_POST, pipe_event, NULL));
    check(nng_dialer_create(&d, s, argv[1]));
    check(nng_dialer_set_bool(d, NNG_OPT_QUIC_ENABLE_MULTISTREAM, multi != 0));
    check(nng_dialer_set_uint64(d, NNG_OPT_QUIC_CONNECT_TIMEOUT, 5));
    if (network_fault) {
        check(nng_dialer_set_uint64(d, NNG_OPT_QUIC_IDLE_TIMEOUT, 3));
        check(nng_dialer_set_int(d, NNG_OPT_QUIC_KEEPALIVE, 1));
    }
    nng_msg *m = packet(NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(m, version);
    nng_mqtt_msg_set_connect_client_id(m, client);
    nng_mqtt_msg_set_connect_clean_session(m, true);
    nng_mqtt_msg_set_connect_keep_alive(m, 10);
    check(nng_dialer_set_ptr(d, NNG_OPT_MQTT_CONNMSG, m));
    phase = "dial";
    check(nng_dialer_start(d, NNG_FLAG_NONBLOCK));
    const char *rate_setting = getenv("NNG_BROKER_TEST_RATE_MESSAGES");
    int rate_messages = rate_setting ? atoi(rate_setting) : 0;
    require(rate_messages >= 0 && rate_messages <= 3000, "RATE_MESSAGES must be 0..3000 per QoS per round");
    unsigned seq = 0;
    const size_t sizes[] = {0, 127, 1024, 16384, 65536};
    for (int round = 0; round < rounds; ++round) {
        phase = "CONNACK / automatic reconnect";
        check(nng_recvmsg(s, &m, 0));
        require(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_CONNACK, "expected CONNACK");
        require(nng_mqtt_msg_get_connack_return_code(m) == 0, "broker refused CONNECT");
        nng_msg_free(m);
        pthread_mutex_lock(&lock);
        unsigned count = connections;
        pthread_mutex_unlock(&lock);
        require(count == (unsigned)round + 1, "unexpected connection count");
        printf("PASS CONNACK connection=%u\n", count);
        subscribe_stream(s, topic, 0);
        if (rate_messages) {
            if (multi) {
                subscribe_stream(s, first, 1);
                subscribe_stream(s, second, 2);
                rate_test(s, first, second, (unsigned)rate_messages);
                multi_topic_finish(s, first, second);
            } else {
                rate_test(s, topic, NULL, (unsigned)rate_messages);
            }
        } else {
            for (int qos = 0; qos <= 2; ++qos)
                for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i)
                    exchange(s, topic, qos, sizes[i], ++seq);
        }
        if (round + 1 < rounds && network_fault && round == 0) {
            /* The opt-in UDP proxy blocks traffic on this marker. No pipe close:
             * the SDK must detect loss and reconnect automatically. */
            puts("FAULT blackhole");
        } else if (round + 1 < rounds) {
            phase = "force pipe close (same socket/dialer reconnects)";
            pthread_mutex_lock(&lock);
            nng_pipe p = current;
            pthread_mutex_unlock(&lock);
            check(nng_pipe_close(p));
            puts("PASS forced disconnect; waiting for automatic reconnect");
        }
    }
    if (!rate_messages) burst(s, topic);
    phase = "idle keepalive";
    puts("WAIT idle 12 seconds (MQTT keepalive=10)");
    sleep(12);
    exchange(s, topic, 1, 1024, ++seq);
    phase = "UNSUBSCRIBE/UNSUBACK";
    m = packet(NNG_MQTT_UNSUBSCRIBE);
    nng_mqtt_topic *t = nng_mqtt_topic_array_create(1);
    require(t != NULL, "allocate topic");
    nng_mqtt_topic_array_set(t, 0, topic);
    nng_mqtt_msg_set_unsubscribe_topics(m, t, 1);
    nng_mqtt_topic_array_free(t, 1);
    m = send_packet(s, m, NNG_MQTT_UNSUBACK);
    nng_msg_free(m);
    puts("PASS UNSUBACK");
    phase = "no delivery after unsubscribe / receive timeout";
    m = packet(NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_qos(m, 1);
    nng_mqtt_msg_set_publish_topic(m, topic);
    m = send_packet(s, m, NNG_MQTT_PUBACK);
    nng_msg_free(m);
    check(nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 1000));
    int rv = nng_recvmsg(s, &m, 0);
    if (rv == 0) {
        fprintf(stderr, "unexpected packet type=%d after unsubscribe\n", nng_mqtt_msg_get_packet_type(m));
        nng_msg_free(m);
    }
    require(rv == NNG_ETIMEDOUT, "expected receive timeout after unsubscribe");
    puts("PASS receive timeout after unsubscribe");
    phase = "close";
    check(nng_close(s));
    nng_fini();
    printf("PASS ALL version=%d multistream=%d connections=%d exchanges=%u\n", version, multi, rounds, seq);
    return 0;
}
