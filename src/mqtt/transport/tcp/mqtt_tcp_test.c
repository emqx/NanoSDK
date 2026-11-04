//
// Copyright 2026 NanoMQ Team, Inc.
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/nng.h>
#include <nng/supplemental/tls/tls.h>

#include <nuts.h>

#define MQTT_V311_NOT_AUTHORIZED 0x05

typedef struct {
	nng_aio    *accept_aio;
	nng_aio    *send_aio;
	nng_stream *stream;
	uint8_t     connack[8];
	size_t      length;
	size_t      offset;
	bool        fragmented;
} reject_server;

static void
reject_server_send_cb(void *arg)
{
	reject_server *server = arg;
	if (nng_aio_result(server->send_aio) != 0) {
		return;
	}
	server->offset += nng_aio_count(server->send_aio);
	if (server->offset < server->length) {
		nng_iov iov = {
			.iov_buf = server->connack + server->offset,
			.iov_len = server->fragmented ? 1 : server->length - server->offset,
		};
		// Force separate transport reads, including a split CONNACK body.
		nng_msleep(5);
		nng_aio_set_iov(server->send_aio, 1, &iov);
		nng_stream_send(server->stream, server->send_aio);
	}
}

static void
capture_pipe(nng_pipe pipe, nng_pipe_ev event, void *arg)
{
	(void) event;
	*(nng_pipe *) arg = pipe;
}

static void
reject_server_accept_cb(void *arg)
{
	reject_server *server = arg;
	nng_iov        iov;

	if (nng_aio_result(server->accept_aio) != 0) {
		return;
	}
	server->stream = nng_aio_get_output(server->accept_aio, 0);
	iov.iov_buf    = server->connack;
	iov.iov_len    = server->fragmented ? 1 : server->length;
	if (nng_aio_set_iov(server->send_aio, 1, &iov) != 0) {
		return;
	}
	nng_stream_send(server->stream, server->send_aio);
}

static void
check_connack(uint8_t type, uint8_t reason_code, int expected,
    bool fragmented, bool tls, bool v5, int qos_timeout)
{
	reject_server server = {
		.connack = { type, 0x02, 0x00, reason_code },
		.length = 4,
		.fragmented = fragmented,
	};
	nng_stream_listener *listener = NULL;
	nng_socket           socket;
	nng_dialer           dialer;
	nng_msg             *connmsg = NULL;
	char                 url[64];
	int                  port;
	int                  reason;
	nng_tls_config      *server_config = NULL, *client_config = NULL;
	nng_pipe             pipe = { 0 };

	if (v5) {
		uint8_t connack[] = { type, 6, 0, reason_code, 3, 0x13, 0, 0 };
		memcpy(server.connack, connack, sizeof(connack));
		server.length = sizeof(connack);
	}

	NUTS_PASS(nng_aio_alloc(
	    &server.accept_aio, reject_server_accept_cb, &server));
	NUTS_PASS(nng_aio_alloc(&server.send_aio, reject_server_send_cb, &server));
	NUTS_PASS(nng_stream_listener_alloc(&listener,
	    tls ? "tls+tcp://127.0.0.1:0" : "tcp://127.0.0.1:0"));
	if (tls) {
		NUTS_PASS(nng_tls_config_alloc(&server_config, NNG_TLS_MODE_SERVER));
		NUTS_PASS(nng_tls_config_own_cert(
		    server_config, nuts_server_crt, nuts_server_key, NULL));
		NUTS_PASS(nng_stream_listener_set_ptr(
		    listener, NNG_OPT_TLS_CONFIG, server_config));
	}
	NUTS_PASS(nng_stream_listener_listen(listener));
	NUTS_PASS(nng_stream_listener_get_int(
	    listener, NNG_OPT_TCP_BOUND_PORT, &port));
	snprintf(url, sizeof(url), "%s://127.0.0.1:%d",
	    tls ? "tls+mqtt-tcp" : "mqtt-tcp", port);

	NUTS_PASS(v5 ? nng_mqttv5_client_open(&socket) : nng_mqtt_client_open(&socket));
	NUTS_PASS(nng_pipe_notify(socket, NNG_PIPE_EV_ADD_POST, capture_pipe, &pipe));
	NUTS_PASS(nng_dialer_create(&dialer, socket, url));
	if (tls) {
		NUTS_PASS(nng_tls_config_alloc(&client_config, NNG_TLS_MODE_CLIENT));
		NUTS_PASS(nng_tls_config_ca_chain(client_config, nuts_server_crt, NULL));
		NUTS_PASS(nng_tls_config_server_name(client_config, "localhost"));
		NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_TLS_CONFIG, client_config));
	}
	NUTS_PASS(nng_mqtt_msg_alloc(&connmsg, 0));
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(
	    connmsg, v5 ? MQTT_PROTOCOL_VERSION_v5 : MQTT_PROTOCOL_VERSION_v311);
	nng_mqtt_msg_set_connect_clean_session(connmsg, true);
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg));

	nng_stream_listener_accept(listener, server.accept_aio);
	NUTS_TRUE(nng_dialer_start(dialer, 0) == expected);
	NUTS_PASS(
	    nng_dialer_get_int(dialer, NNG_OPT_MQTT_CONNECT_REASON, &reason));
	if (type == 0x20) {
		NUTS_TRUE(reason == reason_code);
	}

	nng_aio_wait(server.accept_aio);
	NUTS_PASS(nng_aio_result(server.accept_aio));
	nng_aio_wait(server.send_aio);
	NUTS_PASS(nng_aio_result(server.send_aio));

	if (expected == 0 && v5) {
		nng_duration keepalive = -1;
		NUTS_PASS(nng_pipe_get_ms(pipe, NNG_OPT_MQTT_KEEPALIVE_INTERVAL, &keepalive));
		NUTS_TRUE(keepalive == 0);
	}
	if (expected == 0 && qos_timeout) {
		nng_aio *aio[4];
		for (unsigned i = 0; i < 4; i++) {
			nng_msg *msg;
			NUTS_PASS(nng_aio_alloc(&aio[i], NULL, NULL));
			NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
			nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
			nng_mqtt_msg_set_publish_topic(msg, "unacked");
			nng_mqtt_msg_set_publish_qos(msg, 1 + (i % 2));
			nng_aio_set_msg(aio[i], msg);
			nng_aio_set_timeout(aio[i], qos_timeout == 1 ? 50 : 2000);
			nng_send_aio(socket, aio[i]);
		}
		if (qos_timeout == 2) NUTS_PASS(nng_pipe_close(pipe));
		if (qos_timeout == 3) NUTS_PASS(nng_close(socket));
		for (unsigned i = 0; i < 4; i++) {
			nng_aio_wait(aio[i]);
			NUTS_TRUE(nng_aio_result(aio[i]) ==
			    (qos_timeout == 1 ? NNG_ETIMEDOUT : NNG_ECLOSED));
			NUTS_TRUE(nng_aio_get_msg(aio[i]) == NULL);
			nng_aio_free(aio[i]);
		}
	}
	if (qos_timeout != 3) {
		NUTS_PASS(nng_dialer_close(dialer));
		NUTS_PASS(nng_close(socket));
	}
	nng_stream_listener_close(listener);
	if (server.stream != NULL) {
		nng_stream_close(server.stream);
		nng_stream_free(server.stream);
	}
	nng_stream_listener_free(listener);
	nng_aio_free(server.send_aio);
	nng_aio_free(server.accept_aio);
	if (server_config != NULL) nng_tls_config_free(server_config);
	if (client_config != NULL) nng_tls_config_free(client_config);
}

static void
test_rejected_connack_fails_negotiation(void)
{
	check_connack(0x20, MQTT_V311_NOT_AUTHORIZED, NNG_ECONNREFUSED, false, false, false, false);
}

static void
test_invalid_connack_type(void)
{
	check_connack(0x30, 0, PROTOCOL_ERROR, false, false, false, false);
}

static void
test_offline_send_timeout(void)
{
	nng_socket socket;
	nng_aio *aio;
	nng_msg *msg;
	NUTS_PASS(nng_mqtt_client_open(&socket));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_topic(msg, "offline");
	nng_mqtt_msg_set_publish_qos(msg, 0);
	nng_aio_set_msg(aio, msg);
	nng_aio_set_timeout(aio, 20);
	nng_send_aio(socket, aio);
	nng_aio_wait(aio);
	NUTS_TRUE(nng_aio_result(aio) == NNG_ETIMEDOUT);
	nng_msg_free(nng_aio_get_msg(aio));
	nng_aio_set_msg(aio, NULL);
	nng_aio_free(aio);
	NUTS_PASS(nng_close(socket));
}

static void
test_null_send(void)
{
	nng_socket socket;
	nng_aio *aio;
	NUTS_PASS(nng_mqtt_client_open(&socket));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_send_aio(socket, aio);
	nng_aio_wait(aio);
	NUTS_TRUE(nng_aio_result(aio) == NNG_EINVAL);
	nng_aio_free(aio);
	NUTS_PASS(nng_close(socket));
}

static void test_fragmented_connack(void)
{
	check_connack(0x20, 0, 0, true, false, false, false);
}
static void test_qos_timeout(void)
{
	check_connack(0x20, 0, 0, false, false, false, true);
}
static void test_pending_pipe_close(void)
{
	check_connack(0x20, 0, 0, false, false, false, 2);
}
static void test_pending_socket_close(void)
{
	check_connack(0x20, 0, 0, false, false, true, 3);
}
#ifdef NNG_SUPP_SQLITE
static void test_sqlite_ownership(void)
{
	nng_socket sock;
	nng_mqtt_sqlite_option *opt;
	NUTS_PASS(nng_mqtt_alloc_sqlite_opt(&opt));
	NUTS_PASS(nng_mqtt_client_open(&sock));
	NUTS_PASS(nng_socket_set_ptr(sock, NNG_OPT_MQTT_SQLITE, opt));
	NUTS_PASS(nng_close(sock));
	// Under ASAN this detects socket finalization freeing the caller's object.
	nng_mqtt_set_sqlite_enable(opt, false);
	NUTS_PASS(nng_mqtt_free_sqlite_opt(opt));
}
#endif
static void test_connect_ownership(void)
{
	const char *urls[] = {
		"mqtt-tcp://127.0.0.1:1883",
#ifdef NNG_SUPP_TLS
		"tls+mqtt-tcp://127.0.0.1:8883",
#endif
	};
	for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
		nng_socket sock;
		nng_dialer dialer;
		nng_msg *msg, *got;
		int reason = -1;
		NUTS_PASS(nng_mqtt_client_open(&sock));
		NUTS_PASS(nng_socket_get_int(sock, NNG_OPT_MQTT_DISCONNECT_REASON, &reason));
		NUTS_TRUE(reason == 0);
		NUTS_PASS(nng_dialer_create(&dialer, sock, urls[i]));
		for (int j = 0; j < 2; j++) {
			NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
			nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
			NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
			NUTS_PASS(nng_dialer_get_ptr(dialer, NNG_OPT_MQTT_CONNMSG, (void **) &got));
			NUTS_TRUE(got == msg);
			NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
		}
		NUTS_PASS(nng_close(sock));
	}
}
static void test_v5_keepalive(void)
{
	check_connack(0x20, 0, 0, true, false, true, false);
}
#ifdef NNG_SUPP_TLS
static void test_tls_fragmented_connack(void)
{
	check_connack(0x20, 0, 0, true, true, true, true);
}
static void test_tls_rejected_connack(void)
{
	check_connack(0x20, 0x87, NNG_ECONNREFUSED, true, true, true, false);
}
#endif


/* Independent wire peer for successful MQTT exchanges over both transports. */
typedef struct {
    nng_stream_listener *listener;
    int error;
    unsigned connects, publishes, subscribes, unsubscribes;
} exchange_peer;
static int peer_io(nng_stream *stream, nng_aio *aio, void *buf, size_t len, bool send)
{
    size_t offset = 0;
    while (offset < len) {
        nng_iov iov = {(uint8_t *)buf + offset, len - offset};
        nng_aio_set_iov(aio, 1, &iov);
        if (send) nng_stream_send(stream, aio); else nng_stream_recv(stream, aio);
        nng_aio_wait(aio);
        int rv = nng_aio_result(aio);
        if (rv) return rv;
        size_t count = nng_aio_count(aio);
        if (!count) return NNG_ECLOSED;
        offset += count;
    }
    return 0;
}
static void exchange_peer_run(void *arg)
{
    exchange_peer *p = arg;
    nng_aio *aio;
    if ((p->error = nng_aio_alloc(&aio, NULL, NULL)) != 0) return;
    nng_aio_set_timeout(aio, 5000);
    for (unsigned round = 0; round < 2 && !p->error; ++round) {
        nng_stream_listener_accept(p->listener, aio);
        nng_aio_wait(aio);
        if ((p->error = nng_aio_result(aio)) != 0) break;
        nng_stream *stream = nng_aio_get_output(aio, 0);
        unsigned version = 0;
        for (;;) {
            uint8_t frame[8192], ack[6] = {0};
            size_t h = 1, body = 0, shift = 0;
            int rv = peer_io(stream, aio, frame, 1, false);
            if (rv) { if (rv == NNG_ETIMEDOUT) p->error = rv; break; }
            do {
                rv = peer_io(stream, aio, frame + h, 1, false);
                if (rv) break;
                body |= (size_t)(frame[h] & 127) << shift;
                shift += 7;
            } while ((frame[h++] & 128) && h < 5);
            if (rv || body > sizeof(frame) - h) { p->error = rv ? rv : NNG_EPROTO; break; }
            if ((rv = peer_io(stream, aio, frame + h, body, false)) != 0) { p->error = rv; break; }
            unsigned type = frame[0] >> 4;
            size_t acklen = 0;
            if (type == 1 && body >= 7) {
                version = frame[h + 6]; ++p->connects;
                ack[0] = 0x20; ack[1] = version == 5 ? 3 : 2;
                acklen = ack[1] + 2;
            } else if (type == 3 && body >= 2) {
                ++p->publishes;
                if ((rv = peer_io(stream, aio, frame, h + body, true)) != 0) { p->error = rv; break; }
                unsigned qos = (frame[0] >> 1) & 3;
                if (qos) {
                    size_t pos = h + 2 + ((size_t)frame[h] << 8) + frame[h + 1];
                    if (pos + 2 > h + body) { p->error = NNG_EPROTO; break; }
                    ack[0] = qos == 1 ? 0x40 : 0x50; ack[1] = 2;
                    ack[2] = frame[pos]; ack[3] = frame[pos + 1]; acklen = 4;
                }
            } else if ((type == 5 || type == 6) && body >= 2) {
                ack[0] = type == 5 ? 0x62 : 0x70; ack[1] = 2;
                ack[2] = frame[h]; ack[3] = frame[h + 1]; acklen = 4;
            } else if ((type == 8 || type == 10) && body >= 2) {
                ack[0] = type == 8 ? 0x90 : 0xb0;
                ack[2] = frame[h]; ack[3] = frame[h + 1];
                if (type == 8) {
                    ++p->subscribes; ack[1] = version == 5 ? 4 : 3;
                    ack[version == 5 ? 5 : 4] = 2;
                } else { ++p->unsubscribes; ack[1] = version == 5 ? 4 : 2; }
                acklen = ack[1] + 2;
            } else if (type == 12) { ack[0] = 0xd0; acklen = 2; }
            else if (type != 4 && type != 7 && type != 14) { p->error = NNG_EPROTO; break; }
            if (acklen && (p->error = peer_io(stream, aio, ack, acklen, true)) != 0) break;
        }
        nng_stream_close(stream); nng_stream_free(stream);
    }
    nng_aio_free(aio);
}
static void successful_exchange(bool tls, bool v5)
{
    exchange_peer peer = {0};
    nng_socket sock; nng_dialer d; nng_thread *thread;
    nng_tls_config *sc = NULL, *cc = NULL;
    nng_aio *send; nng_msg *m;
    NUTS_PASS(nng_stream_listener_alloc(&peer.listener, tls ? "tls+tcp://127.0.0.1:0" : "tcp://127.0.0.1:0"));
    if (tls) {
        NUTS_PASS(nng_tls_config_alloc(&sc, NNG_TLS_MODE_SERVER));
        NUTS_PASS(nng_tls_config_own_cert(sc, nuts_server_crt, nuts_server_key, NULL));
        NUTS_PASS(nng_stream_listener_set_ptr(peer.listener, NNG_OPT_TLS_CONFIG, sc));
    }
    NUTS_PASS(nng_stream_listener_listen(peer.listener));
    int port; char url[80];
    NUTS_PASS(nng_stream_listener_get_int(peer.listener, NNG_OPT_TCP_BOUND_PORT, &port));
    snprintf(url, sizeof(url), "%s://127.0.0.1:%d", tls ? "tls+mqtt-tcp" : "mqtt-tcp", port);
    NUTS_PASS(nng_thread_create(&thread, exchange_peer_run, &peer));
    NUTS_PASS(v5 ? nng_mqttv5_client_open(&sock) : nng_mqtt_client_open(&sock));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 5000));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECONNMINT, 10));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECONNMAXT, 20));
    NUTS_PASS(nng_dialer_create(&d, sock, url));
    if (tls) {
        NUTS_PASS(nng_tls_config_alloc(&cc, NNG_TLS_MODE_CLIENT));
        NUTS_PASS(nng_tls_config_ca_chain(cc, nuts_server_crt, NULL));
        NUTS_PASS(nng_tls_config_server_name(cc, "localhost"));
        NUTS_PASS(nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, cc));
    }
    NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
    nng_mqtt_msg_set_packet_type(m, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(m, v5 ? 5 : 4);
    nng_mqtt_msg_set_connect_clean_session(m, true);
    NUTS_PASS(nng_dialer_set_ptr(d, NNG_OPT_MQTT_CONNMSG, m));
    NUTS_PASS(nng_dialer_start(d, 0));
    NUTS_PASS(nng_aio_alloc(&send, NULL, NULL)); nng_aio_set_timeout(send, 5000);
    for (unsigned round = 0; round < 2; ++round) {
        nng_pipe pipe = {0};
        for (unsigned op = 0; op < 5; ++op) {
            NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
            nng_mqtt_packet_type expected = 0;
            if (op == 0) {
                nng_mqtt_msg_set_packet_type(m, NNG_MQTT_SUBSCRIBE);
                nng_mqtt_topic_qos *t = nng_mqtt_topic_qos_array_create(1);
                nng_mqtt_topic_qos_array_set(t, 0, "wire/test", 9, 2, 0, 0, 0);
                nng_mqtt_msg_set_subscribe_topics(m, t, 1); nng_mqtt_topic_qos_array_free(t, 1);
                expected = NNG_MQTT_SUBACK;
            } else if (op == 4) {
                nng_mqtt_msg_set_packet_type(m, NNG_MQTT_UNSUBSCRIBE);
                nng_mqtt_topic *t = nng_mqtt_topic_array_create(1);
                nng_mqtt_topic_array_set(t, 0, "wire/test");
                nng_mqtt_msg_set_unsubscribe_topics(m, t, 1); nng_mqtt_topic_array_free(t, 1);
                expected = NNG_MQTT_UNSUBACK;
            } else {
                nng_mqtt_msg_set_packet_type(m, NNG_MQTT_PUBLISH);
                nng_mqtt_msg_set_publish_qos(m, op - 1);
                nng_mqtt_msg_set_publish_topic(m, "wire/test");
                uint8_t data[4096]; memset(data, (int)(op + round * 8), sizeof(data));
                nng_mqtt_msg_set_publish_payload(m, data, sizeof(data));
                if (op > 1) expected = op == 2 ? NNG_MQTT_PUBACK : NNG_MQTT_PUBCOMP;
            }
            nng_aio_set_msg(send, m); nng_send_aio(sock, send); nng_aio_wait(send);
            NUTS_PASS(nng_aio_result(send)); m = nng_aio_get_msg(send);
            if (expected) NUTS_TRUE(m && nng_mqtt_msg_get_packet_type(m) == expected);
            else NUTS_TRUE(m == NULL);
            nng_msg_free(m); nng_aio_set_msg(send, NULL);
            if (op > 0 && op < 4) {
                NUTS_PASS(nng_recvmsg(sock, &m, 0));
                NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBLISH);
                NUTS_TRUE(nng_mqtt_msg_get_publish_qos(m) == op - 1);
                uint32_t len; uint8_t *data = nng_mqtt_msg_get_publish_payload(m, &len);
                uint8_t expected_data[4096]; memset(expected_data, (int)(op + round * 8), sizeof(expected_data));
                NUTS_TRUE(len == sizeof(expected_data) && memcmp(data, expected_data, len) == 0);
                pipe = nng_msg_get_pipe(m); nng_msg_free(m);
            }
        }
        if (round == 0) NUTS_PASS(nng_pipe_close(pipe));
    }
    nng_aio_free(send); NUTS_PASS(nng_close(sock));
    nng_thread_destroy(thread);
    NUTS_PASS(peer.error);
    NUTS_TRUE(peer.connects == 2 && peer.publishes == 6 && peer.subscribes == 2 && peer.unsubscribes == 2);
    nng_stream_listener_free(peer.listener);
    if (sc) nng_tls_config_free(sc);
    if (cc) nng_tls_config_free(cc);
}
static void test_tcp_success_v4(void) { successful_exchange(false, false); }
static void test_tcp_success_v5(void) { successful_exchange(false, true); }
#ifdef NNG_SUPP_TLS
static void test_tls_success_v4(void) { successful_exchange(true, false); }
static void test_tls_success_v5(void) { successful_exchange(true, true); }
#endif

NUTS_TESTS = {
#ifdef NNG_SUPP_SQLITE
    { "SQLite configuration remains caller owned after close", test_sqlite_ownership },
#endif
    { "TCP MQTT 3.1.1 subscribe QoS unsubscribe reconnect", test_tcp_success_v4 },
    { "TCP MQTT 5 subscribe QoS unsubscribe reconnect", test_tcp_success_v5 },
#ifdef NNG_SUPP_TLS
    { "TLS MQTT 3.1.1 subscribe QoS unsubscribe reconnect", test_tls_success_v4 },
    { "TLS MQTT 5 subscribe QoS unsubscribe reconnect", test_tls_success_v5 },
#endif
	{ "fragmented CONNACK body", test_fragmented_connack },
	{ "QoS send timeout", test_qos_timeout },
	{ "pending QoS completed on pipe close", test_pending_pipe_close },
	{ "pending QoS completed on socket close", test_pending_socket_close },
	{ "CONNECT setter owns message and permits same pointer", test_connect_ownership },
	{ "MQTT v5 server keepalive zero", test_v5_keepalive },
#ifdef NNG_SUPP_TLS
	{ "TLS fragmented CONNACK and QoS timeout", test_tls_fragmented_connack },
	{ "TLS rejected CONNACK", test_tls_rejected_connack },
#endif
	{ "invalid CONNACK type", test_invalid_connack_type },
	{ "offline send timeout", test_offline_send_timeout },
	{ "null send before connect", test_null_send },
	{ "rejected CONNACK fails negotiation",
	    test_rejected_connack_fails_negotiation },
	{ NULL, NULL },
};
