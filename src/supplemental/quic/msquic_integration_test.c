// Local MsQuic peer: these tests exercise the real UDP/TLS/stream stack.
#include "nuts.h"
#include "nng/mqtt/mqtt_client.h"
#include "nng/mqtt/mqtt_quic.h"
#include "mqtt_frame.h"
#include "msquic.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const QUIC_API_TABLE *api;
static HQUIC registration, configuration, listener;
static char certfile[128], keyfile[128], url[128];
static bool echo_mode;
static bool drop_ack;
static bool collision_mode;
static HQUIC collision_stream[2];
static unsigned collision_received;
static nni_atomic_int fault, peer_connections;
static uint8_t connect_reason;
static nni_atomic_int unexpected_pubrel;
static nni_atomic_int mqtt_version;
typedef struct {
	HQUIC handle;
	HQUIC connection;
	uint8_t data[131072];
	size_t len;
} peer_stream;
typedef struct {
	QUIC_BUFFER buffer;
} peer_send;

static void
reply(HQUIC stream, const void *bytes, size_t len)
{
	peer_send *s = calloc(1, sizeof(*s));
	s->buffer.Buffer = malloc(len);
	s->buffer.Length = (uint32_t)len;
	memcpy(s->buffer.Buffer, bytes, len);
	if (QUIC_FAILED(api->StreamSend(stream, &s->buffer, 1, QUIC_SEND_FLAG_NONE, s))) {
		free(s->buffer.Buffer);
		free(s);
	}
}
static void
packet(HQUIC connection, HQUIC stream, uint8_t *data, size_t len, size_t h)
{
	uint8_t type = data[0] >> 4;
	uint8_t ack[5] = { 0 };
	if (type == 1) {
		int ver = data[h + 6];
		nni_atomic_set(&mqtt_version, ver);
		uint8_t connack[] = { 0x20, ver == 5 ? 3 : 2, 0, connect_reason, 0 };
		/* Deliberately split the MQTT fixed header and variable header. */
		for (size_t i = 0; i < (ver == 5 ? 5u : 4u); ++i) reply(stream, connack + i, 1);
	} else if (type == 3) {
        int inject = nni_atomic_swap(&fault, 0);
        if (inject == 1) {
            api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            return;
        }
        if (inject == 5) {
            // Reset only this data stream; keep the QUIC connection alive.
            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return;
        }
        if (inject == 2) {
            uint8_t malformed[] = {0x30, 0xff, 0xff, 0xff, 0xff};
            reply(stream, malformed, sizeof(malformed));
            return;
        }
        if (inject == 3 || inject == 4) {
            size_t off = h + 2 + ((size_t)data[h] << 8) + data[h+1];
            uint8_t rec[] = {0x50, 3, data[off], data[off+1], inject == 3 ? 0x87 : 0x10};
            reply(stream, rec, sizeof(rec));
            return;
        }
		reply(stream, data, len);
		uint8_t qos = (data[0] >> 1) & 3;
		if (qos && !drop_ack) {
			size_t off = h + 2 + ((size_t)data[h] << 8) + data[h+1];
			ack[0] = qos == 1 ? 0x40 : 0x50;
			ack[1] = 2; ack[2] = data[off]; ack[3] = data[off+1];
			reply(stream, ack, 4);
		}
	} else if (type == 8) {
        uint8_t suback4[] = {0x90, 3, data[h], data[h+1], 2};
        uint8_t suback5[] = {0x90, 4, data[h], data[h+1], 0, 2};
        if (nni_atomic_get(&mqtt_version) == 5) reply(stream, suback5, sizeof(suback5));
        else reply(stream, suback4, sizeof(suback4));
        if (collision_mode) {
            uint64_t id; uint32_t size = sizeof(id);
            api->GetParam(stream, QUIC_PARAM_STREAM_ID, &size, &id);
            uint8_t pub[] = {0x34, 11, 0, 6, 't','o','p','i','c', id == 4 ? '1' : '2', 0, 77, 'x'};
            reply(stream, pub, sizeof(pub));
        }
    } else if (type == 5 && collision_mode) {
        collision_stream[collision_received++] = stream;
        if (collision_received == 2) {
            uint8_t rel[] = {0x62, 2, 0, 77};
            reply(collision_stream[0], rel, sizeof(rel));
            reply(collision_stream[1], rel, sizeof(rel));
        }
    } else if (type == 5 || type == 6) {
        if (type == 6 && len > h + 2 && data[h+2] != 0) {
            nni_atomic_inc(&unexpected_pubrel);
        }
		ack[0] = type == 5 ? 0x62 : 0x70;
		ack[1] = 2; ack[2] = data[h]; ack[3] = data[h+1];
		reply(stream, ack, 4);
	} else if (type == 12) {
		uint8_t pong[] = { 0xd0, 0 };
		reply(stream, pong, 2);
	}
}
static QUIC_STATUS QUIC_API
peer_stream_cb(HQUIC handle, void *arg, QUIC_STREAM_EVENT *event)
{
	peer_stream *s = arg;
	switch (event->Type) {
	case QUIC_STREAM_EVENT_RECEIVE:
		for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
			const QUIC_BUFFER *b = &event->RECEIVE.Buffers[i];
			if (echo_mode) reply(handle, b->Buffer, b->Length);
			else {
				if (s->len + b->Length > sizeof(s->data)) return QUIC_STATUS_OUT_OF_MEMORY;
				memcpy(s->data + s->len, b->Buffer, b->Length);
				s->len += b->Length;
			}
		}
		if (!echo_mode) {
			size_t h, b;
			while (mqtt_quic_frame(s->data, s->len, &h, &b) == 0) {
				packet(s->connection, handle, s->data, h + b, h);
				s->len -= h + b;
				memmove(s->data, s->data + h + b, s->len);
			}
		}
		break;
	case QUIC_STREAM_EVENT_SEND_COMPLETE: {
		peer_send *send = event->SEND_COMPLETE.ClientContext;
		free(send->buffer.Buffer); free(send);
		break;
	}
	case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
		api->StreamClose(handle);
		free(s);
		break;
	default: break;
	}
	return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API
peer_connection_cb(HQUIC handle, void *arg, QUIC_CONNECTION_EVENT *event)
{
	(void)arg;
	if (event->Type == QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED) {
		peer_stream *s = calloc(1, sizeof(*s));
		s->handle = event->PEER_STREAM_STARTED.Stream;
		s->connection = handle;
		api->SetCallbackHandler(s->handle, (void *)peer_stream_cb, s);
	} else if (event->Type == QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE) {
		api->ConnectionClose(handle);
	}
	return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API
peer_listener_cb(HQUIC handle, void *arg, QUIC_LISTENER_EVENT *event)
{
	(void)handle; (void)arg;
	if (event->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        nni_atomic_inc(&peer_connections);
		api->SetCallbackHandler(event->NEW_CONNECTION.Connection, (void *)peer_connection_cb, NULL);
		return api->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection, configuration);
	}
	return QUIC_STATUS_SUCCESS;
}
static void
start_peer(bool echo)
{
	echo_mode = echo;
	drop_ack = false;
    collision_mode = false;
    collision_received = 0;
	connect_reason = 0;
	nni_atomic_init(&mqtt_version);
	nni_atomic_init(&fault);
    nni_atomic_init(&peer_connections);
    nni_atomic_init(&unexpected_pubrel);
	snprintf(certfile, sizeof(certfile), "/tmp/nanosdk-quic-%ld.crt", (long)getpid());
	snprintf(keyfile, sizeof(keyfile), "/tmp/nanosdk-quic-%ld.key", (long)getpid());
	FILE *f = fopen(certfile, "w"); NUTS_TRUE(f != NULL);
	fputs(nuts_server_crt, f); fclose(f);
	f = fopen(keyfile, "w"); NUTS_TRUE(f != NULL);
	fputs(nuts_server_key, f); fclose(f);
	NUTS_TRUE(QUIC_SUCCEEDED(MsQuicOpen2(&api)));
	QUIC_REGISTRATION_CONFIG reg = { "NanoSDK-test", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
	NUTS_TRUE(QUIC_SUCCEEDED(api->RegistrationOpen(&reg, &registration)));
	QUIC_BUFFER alpn = { 4, (uint8_t *)"mqtt" };
	QUIC_SETTINGS settings = { 0 };
	settings.IsSet.PeerBidiStreamCount = TRUE;
	settings.PeerBidiStreamCount = 16;
	NUTS_TRUE(QUIC_SUCCEEDED(api->ConfigurationOpen(registration, &alpn, 1, &settings, sizeof(settings), NULL, &configuration)));
	QUIC_CERTIFICATE_FILE cert = { keyfile, certfile };
	QUIC_CREDENTIAL_CONFIG cred = { 0 };
	cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE; cred.CertificateFile = &cert;
	NUTS_TRUE(QUIC_SUCCEEDED(api->ConfigurationLoadCredential(configuration, &cred)));
	NUTS_TRUE(QUIC_SUCCEEDED(api->ListenerOpen(registration, peer_listener_cb, NULL, &listener)));
	QUIC_ADDR addr = { 0 };
	QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET);
	QuicAddrSetPort(&addr, 0);
	NUTS_TRUE(QUIC_SUCCEEDED(api->ListenerStart(listener, &alpn, 1, &addr)));
	uint32_t size = sizeof(addr);
	NUTS_TRUE(QUIC_SUCCEEDED(api->GetParam(listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &size, &addr)));
	snprintf(url, sizeof(url), "mqtt-quic://127.0.0.1:%u", QuicAddrGetPort(&addr));
}
static void
stop_peer(void)
{
	api->ListenerClose(listener);
	api->RegistrationShutdown(registration, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
	api->ConfigurationClose(configuration);
	api->RegistrationClose(registration);
	MsQuicClose(api);
	unlink(certfile); unlink(keyfile);
}
static void test_stream_dialer_ownership(void)
{
    start_peer(true);
    nng_stream_dialer *d; nng_aio *aio[2]; nng_stream *streams[2] = {NULL, NULL};
    NUTS_PASS(nng_stream_dialer_alloc(&d, url));
    for (unsigned i = 0; i < 2; ++i) {
        NUTS_PASS(nng_aio_alloc(&aio[i], NULL, NULL));
        nng_aio_set_timeout(aio[i], 2000);
        nng_stream_dialer_dial(d, aio[i]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        nng_aio_wait(aio[i]); NUTS_PASS(nng_aio_result(aio[i]));
        if (nng_aio_result(aio[i]) == 0) streams[i] = nng_aio_get_output(aio[i], 0);
    }
    nng_stream_dialer_free(d);
    for (unsigned i = 0; i < 2; ++i) {
        if (streams[i]) {
            char out[] = "independent", in[sizeof(out)] = {0};
            nng_iov iov = {out, sizeof(out)};
            NUTS_PASS(nng_aio_set_iov(aio[i], 1, &iov));
            nng_stream_send(streams[i], aio[i]); nng_aio_wait(aio[i]);
            NUTS_PASS(nng_aio_result(aio[i]));
            size_t got = 0;
            while (got < sizeof(in)) {
                iov.iov_buf = in + got; iov.iov_len = sizeof(in) - got;
                NUTS_PASS(nng_aio_set_iov(aio[i], 1, &iov));
                nng_stream_recv(streams[i], aio[i]); nng_aio_wait(aio[i]);
                NUTS_PASS(nng_aio_result(aio[i]));
                if (nng_aio_result(aio[i]) != 0) break;
                got += nng_aio_count(aio[i]);
            }
            NUTS_TRUE(memcmp(out, in, sizeof(out)) == 0);
            nng_stream_free(streams[i]);
        }
        nng_aio_free(aio[i]);
    }
    stop_peer();
}
static void test_cancel_then_redial(void)
{
    start_peer(true);
    nng_stream_dialer *d; nng_aio *old, *next;
    NUTS_PASS(nng_stream_dialer_alloc(&d, url));
    NUTS_PASS(nng_aio_alloc(&old, NULL, NULL));
    NUTS_PASS(nng_aio_alloc(&next, NULL, NULL));
    nng_aio_set_timeout(old, 2000); nng_aio_set_timeout(next, 2000);
    for (unsigned i = 0; i < 20; ++i) {
        nng_stream_dialer_dial(d, old);
        nng_aio_abort(old, NNG_ECANCELED);
        nng_stream_dialer_dial(d, next);
        nng_aio_wait(old); nng_aio_wait(next);
        NUTS_TRUE(nng_aio_result(old) == 0 || nng_aio_result(old) == NNG_ECANCELED);
        NUTS_PASS(nng_aio_result(next));
        if (nng_aio_result(old) == 0) nng_stream_free(nng_aio_get_output(old, 0));
        if (nng_aio_result(next) == 0) nng_stream_free(nng_aio_get_output(next, 0));
    }
    nng_stream_dialer_free(d); nng_aio_free(old); nng_aio_free(next); stop_peer();
}

static void test_dialer_free_pending(void)
{
    nng_stream_dialer *d; nng_aio *aio[2];
    NUTS_PASS(nng_stream_dialer_alloc(&d, "mqtt-quic://127.0.0.1:9"));
    for (unsigned i = 0; i < 2; ++i) {
        NUTS_PASS(nng_aio_alloc(&aio[i], NULL, NULL));
        nng_aio_set_timeout(aio[i], 1000);
        nng_stream_dialer_dial(d, aio[i]);
    }
    nng_stream_dialer_free(d);
    for (unsigned i = 0; i < 2; ++i) {
        nng_aio_wait(aio[i]); NUTS_FAIL(nng_aio_result(aio[i]), NNG_ECLOSED);
        nng_aio_free(aio[i]);
    }
}

static void
test_stream(void)
{
	start_peer(true);
	nng_stream_dialer *d;
	nng_stream *s;
	nng_aio *aio;
	NUTS_PASS(nng_stream_dialer_alloc(&d, url));
	NUTS_PASS(nng_stream_dialer_set_bool(d, NNG_OPT_QUIC_ENABLE_MULTISTREAM, true));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 5000);
	nng_stream_dialer_dial(d, aio); nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	s = nng_aio_get_output(aio, 0);
	uint8_t tx[32768], rx[32768];
	for (size_t i = 0; i < sizeof(tx); ++i) tx[i] = (uint8_t)i;
	for (int id = 0; id <= 4; ++id) {
		nni_aio_set_prov_data(aio, &id);
		nng_iov iov = { tx, sizeof(tx) };
		nng_aio_set_iov(aio, 1, &iov);
		nng_stream_send(s, aio); nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
		size_t got = 0;
		while (got < sizeof(rx)) {
			iov.iov_buf = rx + got; iov.iov_len = sizeof(rx) - got;
			nng_aio_set_iov(aio, 1, &iov);
			nng_stream_recv(s, aio); nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
			NUTS_TRUE(nng_aio_count(aio) > 0);
			got += nng_aio_count(aio);
		}
		NUTS_TRUE(memcmp(tx, rx, sizeof(tx)) == 0);
	}
	nni_aio_set_prov_data(aio, NULL);
	nng_iov iov = { rx, sizeof(rx) }; nng_aio_set_iov(aio, 1, &iov);
	nng_aio_set_timeout(aio, 20);
	nng_stream_recv(s, aio); nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ETIMEDOUT);
	nng_stream_dialer_free(d);
	nng_stream_free(s); nng_aio_free(aio);
	stop_peer();
}
static nni_atomic_int callback_count, callback_errors, disconnect_count;
static int receive_callback(void *arg, void *unused)
{
	(void)unused;
	nng_msg *msg = arg;
	uint32_t len;
	const void *data = nng_mqtt_msg_get_publish_payload(msg, &len);
	if (len != 7 || memcmp(data, "payload", 7)) nni_atomic_inc(&callback_errors);
	nni_atomic_inc(&callback_count);
	return 0;
}
static int disconnect_callback(void *arg, void *unused)
{
	(void)arg; (void)unused;
	nni_atomic_inc(&disconnect_count);
	return 0;
}
static void
mqtt_test(int version, bool multi)
{
	start_peer(false);
	nng_socket sock;
	nng_dialer dialer;
	nng_msg *msg;
	NUTS_PASS(version == 5 ? nng_mqttv5_quic_client_open(&sock) : nng_mqtt_quic_client_open(&sock));
	NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 5000));
	NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_SENDTIMEO, 5000));
	NUTS_PASS(nng_dialer_create(&dialer, sock, url));
	NUTS_PASS(nng_dialer_set_bool(dialer, NNG_OPT_QUIC_ENABLE_MULTISTREAM, multi));
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(msg, version);
	nng_mqtt_msg_set_connect_keep_alive(msg, 0);
	nng_mqtt_msg_set_connect_client_id(msg, "local-quic-test");
	nng_mqtt_msg_set_connect_clean_session(msg, true);
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
	NUTS_PASS(nng_dialer_start(dialer, 0));
	NUTS_PASS(nng_recvmsg(sock, &msg, 0));
	NUTS_TRUE(nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNACK);
	nng_msg_free(msg);
	nni_atomic_init(&callback_count);
	nni_atomic_init(&callback_errors);
	nni_atomic_init(&disconnect_count);
	NUTS_PASS(nng_mqtt_quic_set_msg_recv_cb(&sock, receive_callback, NULL));
	NUTS_PASS(nng_mqtt_quic_set_disconnect_cb(&sock, disconnect_callback, NULL));
	nng_aio *receive;
	NUTS_PASS(nng_aio_alloc(&receive, NULL, NULL));
	nng_aio_set_timeout(receive, 5000);
	for (int qos = 0; qos <= 2; ++qos) {
		nng_recv_aio(sock, receive);
		NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
		nng_mqtt_msg_set_publish_qos(msg, qos);
		nng_mqtt_msg_set_publish_topic(msg, "test/topic");
		nng_mqtt_msg_set_publish_payload(msg, (uint8_t *)"payload", 7);
		NUTS_PASS(nng_sendmsg(sock, msg, 0));
		nng_aio_wait(receive);
		NUTS_PASS(nng_aio_result(receive));
		msg = nng_aio_get_msg(receive);
		nng_aio_set_msg(receive, NULL);
		NUTS_TRUE(nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH);
		uint32_t len;
		void *data = nng_mqtt_msg_get_publish_payload(msg, &len);
		NUTS_TRUE(len == 7 && memcmp(data, "payload", 7) == 0);
		nng_msg_free(msg);
	}
	for (int i = 0; i < 100 && nni_atomic_get(&callback_count) != 3; ++i) nni_msleep(1);
	NUTS_TRUE(nni_atomic_get(&callback_count) == 3);
	NUTS_TRUE(nni_atomic_get(&callback_errors) == 0);
	nng_aio_free(receive);
	NUTS_PASS(nng_close(sock));
	NUTS_TRUE(nni_atomic_get(&disconnect_count) == 1);
	stop_peer();
}
static void test_v311(void) { mqtt_test(4, true); }
static void test_v5(void) { mqtt_test(5, true); }
static void test_single(void) { mqtt_test(5, false); }
static void test_offline_timeout(void)
{
	nng_socket s; nng_msg *msg;
	NUTS_PASS(nng_mqtt_quic_client_open(&s));
	NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, 20));
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_qos(msg, 1);
	nng_mqtt_msg_set_publish_topic(msg, "timeout");
	NUTS_FAIL(nng_sendmsg(s, msg, 0), NNG_ETIMEDOUT);
	nng_msg_free(msg);
	NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 20));
	NUTS_FAIL(nng_recvmsg(s, &msg, 0), NNG_ETIMEDOUT);
	NUTS_PASS(nng_close(s));
}
static void test_reconnect_and_tls(void)
{
	start_peer(true);
	char ca_path[128];
	snprintf(ca_path, sizeof(ca_path), "/tmp/nanosdk-quic-%ld-ca.crt", (long)getpid());
	FILE *f = fopen(ca_path, "w"); NUTS_TRUE(f != NULL);
	fputs(nuts_server_crt, f); fclose(f);
	char secure_url[128];
	snprintf(secure_url, sizeof(secure_url), "mqtt-quic://localhost:%s", strrchr(url, ':') + 1);
	nng_stream_dialer *d;
	nng_aio *aio;
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	NUTS_PASS(nng_stream_dialer_alloc(&d, secure_url));
	NUTS_PASS(nng_stream_dialer_set_bool(d, NNG_OPT_QUIC_TLS_VERIFY_PEER, true));
	NUTS_PASS(nng_stream_dialer_set_string(d, NNG_OPT_QUIC_TLS_CA_PATH, ca_path));
	nng_aio_set_timeout(aio, 5000);
	for (int i = 0; i < 2; ++i) {
		/* A closed connection's shutdown callback may still be draining. */
		int rv = NNG_EBUSY;
		for (int j = 0; j < 100 && rv == NNG_EBUSY; ++j) {
			nng_stream_dialer_dial(d, aio); nng_aio_wait(aio);
			rv = nng_aio_result(aio);
			if (rv == NNG_EBUSY) nni_msleep(1);
		}
		NUTS_PASS(rv);
		nng_stream *stream = nng_aio_get_output(aio, 0);
		nng_stream_close(stream);
		nng_stream_free(stream);
	}
	nng_stream_dialer_free(d);
	nng_aio_free(aio);
	stop_peer();
	unlink(ca_path);
}

static void test_close_pending_qos(void)
{
	start_peer(false);
	drop_ack = true;
	nng_socket sock; nng_dialer dialer; nng_msg *msg; nng_aio *aio;
	NUTS_PASS(nng_mqtt_quic_client_open(&sock));
	NUTS_PASS(nng_dialer_create(&dialer, sock, url));
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(msg, 4);
	nng_mqtt_msg_set_connect_clean_session(msg, true);
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
	NUTS_PASS(nng_dialer_start(dialer, 0));
	NUTS_PASS(nng_recvmsg(sock, &msg, 0));
	nng_msg_free(msg);
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_qos(msg, 1);
	nng_mqtt_msg_set_publish_topic(msg, "close");
	nng_aio_set_msg(aio, msg);
	nng_send_aio(sock, aio);
	NUTS_PASS(nng_recvmsg(sock, &msg, 0)); /* peer received/echoed PUBLISH */
	nng_msg_free(msg);
	NUTS_PASS(nng_close(sock));
	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);
	NUTS_TRUE(nng_aio_get_msg(aio) != NULL);
	nng_msg_free(nng_aio_get_msg(aio));
	nng_aio_set_msg(aio, NULL);
	nng_aio_free(aio);
	stop_peer();
}
static void test_rejected_connect(void)
{
	start_peer(false);
	connect_reason = 0x87;
	nng_socket sock; nng_dialer dialer; nng_msg *msg;
	NUTS_PASS(nng_mqttv5_quic_client_open(&sock));
	NUTS_PASS(nng_dialer_create(&dialer, sock, url));
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(msg, 5);
	nng_mqtt_msg_set_connect_clean_session(msg, true);
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
	NUTS_FAIL(nng_dialer_start(dialer, 0), NNG_ECONNREFUSED);
	int reason;
	NUTS_PASS(nng_dialer_get_int(dialer, NNG_OPT_MQTT_CONNECT_REASON, &reason));
	NUTS_TRUE(reason == 0x87);
	NUTS_PASS(nng_close(sock));
	stop_peer();
}

static void open_client_version(nng_socket *s, nng_dialer *d, int version)
{
    nng_msg *m;
    NUTS_PASS(version == 5 ? nng_mqttv5_quic_client_open(s) : nng_mqtt_quic_client_open(s));
    NUTS_PASS(nng_socket_set_ms(*s, NNG_OPT_SENDTIMEO, 5000));
    NUTS_PASS(nng_socket_set_ms(*s, NNG_OPT_RECVTIMEO, 5000));
    NUTS_PASS(nng_socket_set_ms(*s, NNG_OPT_RECONNMINT, 10));
    NUTS_PASS(nng_socket_set_ms(*s, NNG_OPT_RECONNMAXT, 20));
    NUTS_PASS(nng_dialer_create(d, *s, url));
    NUTS_PASS(nng_dialer_set_bool(*d, NNG_OPT_QUIC_ENABLE_MULTISTREAM, true));
    NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
    nng_mqtt_msg_set_packet_type(m, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(m, version);
    nng_mqtt_msg_set_connect_clean_session(m, true);
    NUTS_PASS(nng_dialer_set_ptr(*d, NNG_OPT_MQTT_CONNMSG, m));
    NUTS_PASS(nng_dialer_start(*d, 0));
    NUTS_PASS(nng_recvmsg(*s, &m, 0));
    NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_CONNACK);
    nng_msg_free(m);
}
static void open_client(nng_socket *s, nng_dialer *d)
{ open_client_version(s, d, 4); }

static nng_msg *publication(void)
{
    nng_msg *m;
    NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
    nng_mqtt_msg_set_packet_type(m, NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_qos(m, 1);
    nng_mqtt_msg_set_publish_topic(m, "fault/test");
    nng_mqtt_msg_set_publish_payload(m, (uint8_t *)"owned", 5);
    return m;
}
static void test_substream_recovery(void)
{
    start_peer(false);
    nng_socket s; nng_dialer d; nng_aio *aio; nng_msg *m;
    open_client(&s, &d);
    NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_MQTT_RETRY_INTERVAL, 100));
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL)); nng_aio_set_timeout(aio, 10000);
    for (int round = 0; round < 4; ++round) {
        nni_atomic_set(&fault, 5);
        m = publication(); nng_mqtt_msg_set_publish_qos(m, round % 2 + 1);
        NUTS_PASS(nng_mqtt_quic_msg_set_stream(m, 3 + round % 2));
        nng_aio_set_msg(aio, m); nng_send_aio(s, aio);
        nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
        nng_msg_free(nng_aio_get_msg(aio)); nng_aio_set_msg(aio, NULL);
        NUTS_PASS(nng_recvmsg(s, &m, 0)); nng_msg_free(m);
        NUTS_TRUE(nni_atomic_get(&peer_connections) == 1);
    }
    nng_aio_free(aio); NUTS_PASS(nng_close(s)); stop_peer();
}

static void test_pubrec_reason_codes(void)
{
    start_peer(false);
    nng_socket s; nng_dialer d; nng_msg *m; nng_aio *aio;
    open_client_version(&s, &d, 5);
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
    nng_aio_set_timeout(aio, 1000);
    for (int inject = 3; inject <= 4; ++inject) {
        nni_atomic_set(&fault, inject);
        m = publication(); nng_mqtt_msg_set_publish_qos(m, 2);
        nng_aio_set_msg(aio, m); nng_send_aio(s, aio);
        nng_aio_wait(aio);
        NUTS_PASS(nng_aio_result(aio));
        m = nng_aio_get_msg(aio);
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) ==
            (inject == 3 ? NNG_MQTT_PUBREC : NNG_MQTT_PUBCOMP));
        if (inject == 3) {
            NUTS_TRUE(nng_msg_len(m) >= 3 && ((uint8_t *)nng_msg_body(m))[2] == 0x87);
        }
        nng_msg_free(m); nng_aio_set_msg(aio, NULL);
    }
    NUTS_TRUE(nni_atomic_get(&unexpected_pubrel) == 0);
    nng_aio_free(aio); NUTS_PASS(nng_close(s)); stop_peer();
}

static void test_online_timeout_ownership(void)
{
    start_peer(false);
    drop_ack = true;
    nng_socket s; nng_dialer d; nng_msg *rx;
    open_client(&s, &d);
    NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, 50));
    nng_msg *m = publication();
    NUTS_FAIL(nng_sendmsg(s, m, 0), NNG_ETIMEDOUT);
    /* nng_sendmsg errors leave the original message with the caller. */
    nng_msg_free(m);
    NUTS_PASS(nng_recvmsg(s, &rx, 0));
    nng_msg_free(rx);
    NUTS_PASS(nng_close(s));
    stop_peer();
}
static void test_online_cancel_ownership(void)
{
    start_peer(false);
    drop_ack = true;
    nng_socket s; nng_dialer d; nng_msg *rx; nng_aio *aio;
    open_client(&s, &d);
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
    nng_msg *m = publication();
    nng_aio_set_msg(aio, m);
    nng_send_aio(s, aio);
    NUTS_PASS(nng_recvmsg(s, &rx, 0)); /* wire send finished, waiting for ACK */
    nng_msg_free(rx);
    nng_aio_cancel(aio);
    nng_aio_wait(aio);
    NUTS_FAIL(nng_aio_result(aio), NNG_ECANCELED);
    NUTS_TRUE(nng_aio_get_msg(aio) == m);
    nng_msg_free(m);
    nng_aio_set_msg(aio, NULL);
    nng_aio_free(aio);
    NUTS_PASS(nng_close(s));
    stop_peer();
}
static void test_peer_fault_reconnect(void)
{
    start_peer(false);
    nng_socket s; nng_dialer d; nng_msg *rx;
    open_client(&s, &d);
    for (int i = 0; i < 20; ++i) {
        nng_msg *m = publication();
        nni_atomic_set(&fault, i % 2 + 1); /* shutdown or malformed framing */
        NUTS_TRUE(nng_sendmsg(s, m, 0) != 0);
        nng_msg_free(m);
        NUTS_PASS(nng_recvmsg(s, &rx, 0));
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(rx) == NNG_MQTT_CONNACK);
        nng_msg_free(rx);
        NUTS_PASS(nng_sendmsg(s, publication(), 0));
        NUTS_PASS(nng_recvmsg(s, &rx, 0));
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(rx) == NNG_MQTT_PUBLISH);
        nng_msg_free(rx);
    }
    NUTS_PASS(nng_close(s));
    stop_peer();
}
static void test_invalid_retry_interval(void)
{
    nng_socket s;
    NUTS_PASS(nng_mqtt_quic_client_open(&s));
    NUTS_FAIL(nng_socket_set_ms(s, NNG_OPT_MQTT_RETRY_INTERVAL, 0), NNG_EINVAL);
    NUTS_FAIL(nng_socket_set_ms(s, NNG_OPT_MQTT_RETRY_INTERVAL, NNG_DURATION_INFINITE), NNG_EINVAL);
    NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_MQTT_RETRY_INTERVAL, 100));
    NUTS_PASS(nng_close(s));
}


static void context_close_test(bool online)
{
    nng_socket s; nng_dialer d; nng_ctx ctx; nng_aio *aio; nng_msg *rx;
    if (online) {
        start_peer(false);
        drop_ack = true;
        open_client(&s, &d);
    } else NUTS_PASS(nng_mqtt_quic_client_open(&s));
    NUTS_PASS(nng_ctx_open(&ctx, s));
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
    nng_aio_set_timeout(aio, 100);
    nng_msg *m = publication();
    nng_aio_set_msg(aio, m);
    nng_ctx_send(ctx, aio);
    if (online) {
        NUTS_PASS(nng_recvmsg(s, &rx, 0));
        nng_msg_free(rx);
    }
    NUTS_PASS(nng_ctx_close(ctx));
    nng_aio_wait(aio);
    NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);
    NUTS_TRUE(nng_aio_get_msg(aio) == m);
    nng_msg_free(m);
    nng_aio_set_msg(aio, NULL);
    nng_aio_free(aio);
    NUTS_PASS(nng_close(s));
    if (online) stop_peer();
}
static void test_context_close_offline(void) { context_close_test(false); }
static void test_context_close_online(void) { context_close_test(true); }
static void test_burst_order(void)
{
    start_peer(false);
    nng_socket s; nng_dialer d; nng_aio *aio[64]; nng_msg *rx;
    open_client(&s, &d);
    for (uint8_t i = 0; i < 64; ++i) {
        NUTS_PASS(nng_aio_alloc(&aio[i], NULL, NULL));
        nng_aio_set_timeout(aio[i], 5000);
        nng_msg *m = publication();
        nng_mqtt_msg_set_publish_payload(m, &i, 1);
        nng_aio_set_msg(aio[i], m);
        nng_send_aio(s, aio[i]);
    }
    for (uint8_t i = 0; i < 64; ++i) {
        nng_aio_wait(aio[i]);
        NUTS_PASS(nng_aio_result(aio[i]));
        nng_msg_free(nng_aio_get_msg(aio[i]));
        nng_aio_set_msg(aio[i], NULL);
        nng_aio_free(aio[i]);
        NUTS_PASS(nng_recvmsg(s, &rx, 0));
        uint32_t size;
        uint8_t *payload = nng_mqtt_msg_get_publish_payload(rx, &size);
        NUTS_TRUE(size == 1 && payload[0] == i);
        nng_msg_free(rx);
    }
    NUTS_PASS(nng_close(s));
    stop_peer();
}


static void test_stream_packet_id_collision(void)
{
    start_peer(false);
    collision_mode = true;
    nng_socket sock; nng_dialer d; nng_aio *aio[2];
    open_client(&sock, &d);
    for (unsigned i = 0; i < 2; ++i) {
        nng_msg *m;
        NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
        nng_mqtt_msg_set_packet_type(m, NNG_MQTT_SUBSCRIBE);
        nng_mqtt_topic_qos *t = nng_mqtt_topic_qos_array_create(1);
        nng_mqtt_topic_qos_array_set(t, 0, i ? "topic2" : "topic1", 6, 2, 0, 0, 0);
        nng_mqtt_msg_set_subscribe_topics(m, t, 1);
        nng_mqtt_topic_qos_array_free(t, 1);
        NUTS_PASS(nng_mqtt_quic_msg_set_stream(m, i + 1));
        NUTS_PASS(nng_aio_alloc(&aio[i], NULL, NULL));
        nng_aio_set_timeout(aio[i], 5000);
        nng_aio_set_msg(aio[i], m);
        nng_send_aio(sock, aio[i]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        nng_aio_wait(aio[i]); NUTS_PASS(nng_aio_result(aio[i]));
        nng_msg_free(nng_aio_get_msg(aio[i]));
        nng_aio_set_msg(aio[i], NULL); nng_aio_free(aio[i]);
    }
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        nng_msg *m; NUTS_PASS(nng_recvmsg(sock, &m, 0));
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBLISH);
        NUTS_TRUE(nng_mqtt_msg_get_publish_packet_id(m) == 77);
        unsigned stream = nng_mqtt_quic_msg_get_stream(m);
        NUTS_TRUE(stream == 1 || stream == 2);
        uint32_t len;
        const char *topic = nng_mqtt_msg_get_publish_topic(m, &len);
        NUTS_TRUE(len == 6 && topic[5] == (stream == 1 ? '1' : '2'));
        seen |= 1u << stream;
        nng_msg_free(m);
    }
    NUTS_TRUE(seen == 6);
    NUTS_PASS(nng_close(sock));
    stop_peer();
}
static void test_stream_metadata(void)
{
    nng_msg *m = publication(), *copy;
    NUTS_FAIL(nng_mqtt_quic_msg_set_stream(m, 5), NNG_EINVAL);
    NUTS_PASS(nng_mqtt_quic_msg_set_stream(m, 4));
    NUTS_PASS(nng_msg_dup(&copy, m));
    NUTS_TRUE(nng_mqtt_quic_msg_get_stream(copy) == 4);
    NUTS_PASS(nng_mqtt_quic_msg_set_stream(copy, 0));
    NUTS_TRUE(nng_mqtt_quic_msg_get_stream(m) == 4);
    nng_msg_free(copy); nng_msg_free(m);
}

static nni_atomic_int legacy_ack_count;
static nng_socket legacy_callback_socket;
static void legacy_ack_callback(void *arg)
{
    nng_aio *aio = arg;
    if (nng_aio_result(aio) != 0) return;
    nng_msg *m = nng_aio_get_msg(aio);
    nng_aio_set_msg(aio, NULL);
    if (m) { nng_msg_free(m); nni_atomic_inc(&legacy_ack_count); }
    nng_recv_aio(legacy_callback_socket, aio);
}
static void legacy_test(int version)
{
    start_peer(false);
    nni_atomic_init(&legacy_ack_count);
    nng_socket s; nng_msg *m;
    if (version == 4) NUTS_PASS(nng_mqtt_quic_client_open(&s, url));
    else {
        conf_quic *c = NULL;
        conf_quic_tls_create(&c, NULL, NULL, NULL, NULL);
        NUTS_TRUE(c != NULL);
        c->tls.enable = false;
        c->multi_stream = true;
        c->qos_first = true;
        NUTS_PASS(nng_mqttv5_quic_client_open_conf(&s, url, c));
        conf_quic_tls_free(c);
    }
    NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, 5000));
    NUTS_PASS(nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 5000));
    legacy_callback_socket = s;
    NUTS_PASS(nng_mqtt_quic_ack_callback_set(&s, legacy_ack_callback, NULL));
    NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
    nng_mqtt_msg_set_packet_type(m, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(m, version);
    nng_mqtt_msg_set_connect_clean_session(m, true);
    NUTS_PASS(nng_sendmsg(s, m, 0));
    NUTS_PASS(nng_recvmsg(s, &m, 0));
    NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_CONNACK);
    nng_msg_free(m);
    for (int qos = 1; qos <= 2; ++qos) {
        for (int i = 0; i < 500 && nni_atomic_get(&legacy_ack_count) < qos; ++i) nni_msleep(1);
        NUTS_TRUE(nni_atomic_get(&legacy_ack_count) == qos);
        m = publication(); nng_mqtt_msg_set_publish_qos(m, qos);
        NUTS_PASS(nng_sendmsg(s, m, 0));
        NUTS_PASS(nng_recvmsg(s, &m, 0));
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) == NNG_MQTT_PUBLISH);
        nng_msg_free(m);
    }
    for (int i = 0; i < 500 && nni_atomic_get(&legacy_ack_count) < 3; ++i) nni_msleep(1);
    NUTS_TRUE(nni_atomic_get(&legacy_ack_count) == 3);
    NUTS_PASS(nng_close(s)); stop_peer();
}
static void test_legacy_v4(void) { legacy_test(4); }
static void test_legacy_v5(void) { legacy_test(5); }

/* Fixed-rate send schedule, independent of ACK completion. The local peer
 * echoes each MQTT packet and completes both directions of the QoS handshake. */
static void single_rate_test(int version)
{
    start_peer(false);
    nng_socket sock; nng_dialer d; nng_msg *m;
    nng_aio *sends[200];
    NUTS_PASS(version == 5 ? nng_mqttv5_quic_client_open(&sock) : nng_mqtt_quic_client_open(&sock));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 5000));
    NUTS_PASS(nng_dialer_create(&d, sock, url));
    NUTS_PASS(nng_dialer_set_bool(d, NNG_OPT_QUIC_ENABLE_MULTISTREAM, false));
    NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
    nng_mqtt_msg_set_packet_type(m, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(m, version);
    nng_mqtt_msg_set_connect_clean_session(m, true);
    NUTS_PASS(nng_dialer_set_ptr(d, NNG_OPT_MQTT_CONNMSG, m));
    NUTS_PASS(nng_dialer_start(d, 0));
    NUTS_PASS(nng_recvmsg(sock, &m, 0)); nng_msg_free(m);
    uint64_t start = nni_clock();
    for (unsigned i = 0; i < 200; ++i) {
        uint64_t due = start + i * 100, now = nni_clock();
        if (now < due) nni_msleep((nng_duration)(due - now));
        m = publication();
        nng_mqtt_msg_set_publish_qos(m, i < 100 ? 1 : 2);
        uint8_t payload[1024]; memset(payload, (int)i, sizeof(payload));
        nng_mqtt_msg_set_publish_payload(m, payload, sizeof(payload));
        NUTS_PASS(nng_aio_alloc(&sends[i], NULL, NULL));
        nng_aio_set_timeout(sends[i], 5000);
        nng_aio_set_msg(sends[i], m); nng_send_aio(sock, sends[i]);
    }
    bool seen[200] = {false};
    for (unsigned i = 0; i < 200; ++i) {
        nng_aio_wait(sends[i]); NUTS_PASS(nng_aio_result(sends[i]));
        m = nng_aio_get_msg(sends[i]);
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(m) == (i < 100 ? NNG_MQTT_PUBACK : NNG_MQTT_PUBCOMP));
        nng_msg_free(m); nng_aio_set_msg(sends[i], NULL); nng_aio_free(sends[i]);
        NUTS_PASS(nng_recvmsg(sock, &m, 0));
        uint32_t len; uint8_t *payload = nng_mqtt_msg_get_publish_payload(m, &len);
        NUTS_TRUE(len == 1024 && payload[0] < 200);
        unsigned seq = payload[0]; NUTS_TRUE(!seen[seq]); seen[seq] = true;
        uint8_t expected[1024]; memset(expected, (int)seq, sizeof(expected));
        NUTS_TRUE(memcmp(payload, expected, sizeof(expected)) == 0);
        NUTS_TRUE(nng_mqtt_msg_get_publish_qos(m) == (seq < 100 ? 1 : 2));
        nng_msg_free(m);
    }
    NUTS_PASS(nng_close(sock)); stop_peer();
}
static void test_single_rate_v4(void) { single_rate_test(4); }
static void test_single_rate_v5(void) { single_rate_test(5); }

static void test_offline_stream_on_single(void)
{
    start_peer(false);
    nng_socket sock; nng_dialer d; nng_aio *aio, *queued; nng_msg *m, *rx;
    NUTS_PASS(nng_mqtt_quic_client_open(&sock));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 5000));
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
    nng_aio_set_timeout(aio, 5000);
    m = publication(); NUTS_PASS(nng_mqtt_quic_msg_set_stream(m, 1));
    nng_aio_set_msg(aio, m); nng_send_aio(sock, aio);
    // A rejected offline message must not strand the next queued send.
    NUTS_PASS(nng_aio_alloc(&queued, NULL, NULL));
    nng_aio_set_timeout(queued, 5000);
    nng_aio_set_msg(queued, publication()); nng_send_aio(sock, queued);
    NUTS_PASS(nng_dialer_create(&d, sock, url));
    NUTS_PASS(nng_dialer_set_bool(d, NNG_OPT_QUIC_ENABLE_MULTISTREAM, false));
    NUTS_PASS(nng_mqtt_msg_alloc(&rx, 0));
    nng_mqtt_msg_set_packet_type(rx, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(rx, 4);
    nng_mqtt_msg_set_connect_clean_session(rx, true);
    NUTS_PASS(nng_dialer_set_ptr(d, NNG_OPT_MQTT_CONNMSG, rx));
    NUTS_PASS(nng_dialer_start(d, 0));
    nng_aio_wait(aio); NUTS_FAIL(nng_aio_result(aio), NNG_ENOTSUP);
    NUTS_TRUE(nng_aio_get_msg(aio) == m);
    nng_msg_free(m); nng_aio_set_msg(aio, NULL); nng_aio_free(aio);
    NUTS_PASS(nng_recvmsg(sock, &rx, 0));
    NUTS_TRUE(nng_mqtt_msg_get_packet_type(rx) == NNG_MQTT_CONNACK);
    nng_msg_free(rx);
    nng_aio_wait(queued); NUTS_PASS(nng_aio_result(queued));
    rx = nng_aio_get_msg(queued);
    NUTS_TRUE(rx != NULL && nng_mqtt_msg_get_packet_type(rx) == NNG_MQTT_PUBACK);
    nng_msg_free(rx); nng_aio_set_msg(queued, NULL); nng_aio_free(queued);
    NUTS_PASS(nng_recvmsg(sock, &rx, 0));
    NUTS_TRUE(nng_mqtt_msg_get_packet_type(rx) == NNG_MQTT_PUBLISH);
    nng_msg_free(rx);
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_SENDTIMEO, 5000));
    NUTS_PASS(nng_sendmsg(sock, publication(), 0));
    NUTS_PASS(nng_recvmsg(sock, &rx, 0)); nng_msg_free(rx);
    NUTS_PASS(nng_close(sock)); stop_peer();
}

static void test_quic_reinitialize(void)
{
    for (unsigned i = 0; i < 10; ++i) {
        start_peer(false);
        nng_socket sock; nng_dialer d; nng_msg *m;
        open_client(&sock, &d);
        NUTS_PASS(nng_sendmsg(sock, publication(), 0));
        NUTS_PASS(nng_recvmsg(sock, &m, 0)); nng_msg_free(m);
        NUTS_PASS(nng_close(sock));
        // Finalize while the native peer is still alive, so late MsQuic
        // shutdown cannot be hidden by stopping the test peer first.
        nng_fini();
        stop_peer();
    }
}

// Follow NanoMQ bridge.c: subscribe from the pipe connect notification,
// publish via independent AIOs, then recreate subscriptions after reconnect.
typedef struct {
    nng_mqtt_client *client;
    nni_atomic_int connected, disconnected, subscribed, pipe_id, error;
} bridge_test_state;
static void bridge_send_done(nng_mqtt_client *client, nng_msg *msg, void *arg, int rv)
{
    (void)client;
    bridge_test_state *s = arg;
    if (rv || msg == NULL || nng_mqtt_msg_get_packet_type(msg) != NNG_MQTT_SUBACK)
        nni_atomic_set(&s->error, rv ? rv : NNG_EPROTO);
    else nni_atomic_inc(&s->subscribed);
    nng_msg_free(msg);
}
static void bridge_connected(nng_pipe pipe, nng_pipe_ev ev, void *arg)
{
    (void)ev;
    bridge_test_state *s = arg;
    int reason = -1;
    int rv = nng_pipe_get_int(pipe, NNG_OPT_MQTT_CONNECT_REASON, &reason);
    if (rv || reason) { nni_atomic_set(&s->error, rv ? rv : reason); return; }
    nng_mqtt_topic_qos *t = nng_mqtt_topic_qos_array_create(1);
    nng_mqtt_topic_qos_array_set(t, 0, "bridge/#", 8, 2, 0, 0, 0);
    rv = nng_mqtt_subscribe_async(s->client, t, 1, NULL);
    nng_mqtt_topic_qos_array_free(t, 1);
    if (rv != 0 && rv != 1) nni_atomic_set(&s->error, rv);
    nni_atomic_set(&s->pipe_id, nng_pipe_id(pipe));
    nni_atomic_inc(&s->connected);
}
static void bridge_disconnected(nng_pipe pipe, nng_pipe_ev ev, void *arg)
{
    (void)pipe; (void)ev;
    bridge_test_state *s = arg; nni_atomic_inc(&s->disconnected);
}
static void bridge_lifecycle(int version)
{
    start_peer(false);
    nng_socket sock; nng_dialer dialer; nng_msg *msg;
    bridge_test_state s = {0};
    nni_atomic_init(&s.connected); nni_atomic_init(&s.disconnected);
    nni_atomic_init(&s.subscribed); nni_atomic_init(&s.pipe_id); nni_atomic_init(&s.error);
    NUTS_PASS(version == 5 ? nng_mqttv5_quic_client_open(&sock) : nng_mqtt_quic_client_open(&sock));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 5000));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECONNMINT, 10));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECONNMAXT, 20));
    s.client = nng_mqtt_client_alloc(sock, bridge_send_done, NULL, true);
    NUTS_TRUE(s.client != NULL); s.client->obj = &s;
    NUTS_PASS(nng_mqtt_set_connect_cb(sock, bridge_connected, &s));
    NUTS_PASS(nng_mqtt_set_disconnect_cb(sock, bridge_disconnected, &s));
    NUTS_PASS(nng_dialer_create(&dialer, sock, url));
    NUTS_PASS(nng_dialer_set_bool(dialer, NNG_OPT_QUIC_ENABLE_MULTISTREAM, true));
    NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
    nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(msg, version);
    nng_mqtt_msg_set_connect_clean_session(msg, true);
    NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
    NUTS_PASS(nng_dialer_start(dialer, NNG_FLAG_NONBLOCK));
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned i = 0; i < 5000 && nni_atomic_get(&s.subscribed) < (int)round + 1 &&
            nni_atomic_get(&s.error) == 0; i++) nni_msleep(1);
        NUTS_TRUE(nni_atomic_get(&s.error) == 0);
        NUTS_TRUE(nni_atomic_get(&s.subscribed) == (int)round + 1);
        NUTS_PASS(nng_recvmsg(sock, &msg, 0));
        NUTS_TRUE(nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNACK); nng_msg_free(msg);
        nng_aio *send[8];
        for (uint8_t i = 0; i < 8; i++) {
            NUTS_PASS(nng_aio_alloc(&send[i], NULL, NULL)); nng_aio_set_timeout(send[i], 5000);
            msg = publication(); nng_mqtt_msg_set_publish_qos(msg, 1 + i % 2);
            nng_mqtt_msg_set_publish_topic(msg, i % 2 ? "bridge/a" : "bridge/b");
            nng_mqtt_msg_set_publish_payload(msg, &i, 1);
            nng_aio_set_msg(send[i], msg); nng_send_aio(sock, send[i]);
        }
        unsigned seen = 0;
        for (unsigned i = 0; i < 8; i++) {
            nng_aio_wait(send[i]); NUTS_PASS(nng_aio_result(send[i]));
            nng_msg_free(nng_aio_get_msg(send[i])); nng_aio_set_msg(send[i], NULL); nng_aio_free(send[i]);
            NUTS_PASS(nng_recvmsg(sock, &msg, 0));
            uint32_t len; uint8_t *payload = nng_mqtt_msg_get_publish_payload(msg, &len);
            NUTS_TRUE(len == 1 && payload[0] < 8 && !(seen & (1u << payload[0])));
            seen |= 1u << payload[0]; nng_msg_free(msg);
        }
        NUTS_TRUE(seen == 255);
        if (round < 2) {
            nng_pipe pipe = {(uint32_t)nni_atomic_get(&s.pipe_id)}; NUTS_PASS(nng_pipe_close(pipe));
        }
    }
    NUTS_TRUE(nni_atomic_get(&s.connected) == 3 && nni_atomic_get(&s.disconnected) >= 2);
    NUTS_PASS(nng_close(sock)); nng_mqtt_client_free(s.client, true); stop_peer();
}
static void test_bridge_v4(void) { bridge_lifecycle(4); }
static void test_bridge_v5(void) { bridge_lifecycle(5); }

#if defined(NNG_SUPP_SQLITE)
static void test_sqlite_replay_v5(void)
{
    start_peer(false);
    nng_socket sock; nng_dialer dialer; nng_msg *msg;
    nng_mqtt_sqlite_option *sqlite;
    char path[] = "/tmp/nanosdk-quic-cache-XXXXXX";
    NUTS_TRUE(mkdtemp(path) != NULL);
    NUTS_PASS(nng_mqtt_alloc_sqlite_opt(&sqlite));
    nng_mqtt_set_sqlite_db_dir(sqlite, path); nng_mqtt_set_sqlite_flush_threshold(sqlite, 2);
    nng_mqtt_sqlite_db_init(sqlite, "replay.db", 5);
    NUTS_PASS(nng_mqttv5_quic_client_open(&sock));
    NUTS_PASS(nng_socket_set_ptr(sock, NNG_OPT_MQTT_SQLITE, sqlite));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_MQTT_RETRY_INTERVAL, 10));
    NUTS_PASS(nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 3000));
    for (uint8_t i = 0; i < 8; i++) {
        msg = publication(); nng_mqtt_msg_set_publish_qos(msg, 0);
        nng_mqtt_msg_set_publish_payload(msg, &i, 1); NUTS_PASS(nng_sendmsg(sock, msg, 0));
    }
    NUTS_TRUE(nng_mqtt_sqlite_db_get_cached_size(sqlite) == 8);
    NUTS_PASS(nng_dialer_create(&dialer, sock, url));
    NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0)); nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(msg, 5); nng_mqtt_msg_set_connect_clean_session(msg, true);
    NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg));
    NUTS_PASS(nng_dialer_start(dialer, 0));
    NUTS_PASS(nng_recvmsg(sock, &msg, 0)); nng_msg_free(msg);
    for (uint8_t i = 0; i < 8; i++) {
        NUTS_PASS(nng_recvmsg(sock, &msg, 0));
        uint32_t len; uint8_t *payload = nng_mqtt_msg_get_publish_payload(msg, &len);
        NUTS_TRUE(len == 1 && payload[0] == i); nng_msg_free(msg);
    }
    NUTS_TRUE(nng_mqtt_sqlite_db_get_cached_size(sqlite) == 0);
    NUTS_PASS(nng_close(sock)); nng_mqtt_sqlite_db_fini(sqlite); nng_mqtt_free_sqlite_opt(sqlite);
    char file[256]; snprintf(file, sizeof(file), "%s/replay.db", path); unlink(file); rmdir(path);
    stop_peer();
}
#endif

TEST_LIST = {
    { "bridge pipe callbacks subscribe publish and reconnect MQTT4", test_bridge_v4 },
    { "bridge pipe callbacks subscribe publish and reconnect MQTT5", test_bridge_v5 },
#if defined(NNG_SUPP_SQLITE)
    { "MQTT5 SQLite offline replay", test_sqlite_replay_v5 },
#endif
    { "cancel dial then immediately redial", test_cancel_then_redial },
    { "stream survives dialer free and parallel dials", test_stream_dialer_ownership },
    { "dialer free completes pending user aios", test_dialer_free_pending },
    { "data stream reset recovers without reconnect", test_substream_recovery },
    { "MQTT5 PUBREC rejection and success reasons", test_pubrec_reason_codes },
    { "QUIC close finalize and initialize ten times", test_quic_reinitialize },
    { "offline explicit stream rejected on single stream connection", test_offline_stream_on_single },
    { "single stream 10 msg per second MQTT 3.1.1", test_single_rate_v4 },
    { "single stream 10 msg per second MQTT 5", test_single_rate_v5 },
    { "legacy MQTT 3.1.1 open URL and ACK callback", test_legacy_v4 },
    { "legacy MQTT 5 configuration and ACK callback", test_legacy_v5 },
    { "QoS2 equal packet IDs on two streams", test_stream_packet_id_collision },
    { "stream selection metadata and duplication", test_stream_metadata },
    { "close context with offline send", test_context_close_offline },
    { "close context with unacknowledged send", test_context_close_online },
    { "64 concurrent sends preserve topic order", test_burst_order },
    { "online timeout preserves caller message", test_online_timeout_ownership },
    { "online cancellation preserves caller message", test_online_cancel_ownership },
    { "peer shutdown and malformed frame reconnect twenty times", test_peer_fault_reconnect },
    { "invalid retry interval", test_invalid_retry_interval },
	{ "native multistream byte transfer and cancel", test_stream },
	{ "native MQTT 3.1.1 QoS 0/1/2", test_v311 },
	{ "native MQTT 5 QoS 0/1/2", test_v5 },
	{ "native MQTT single stream", test_single },
	{ "offline send and receive timeout", test_offline_timeout },
	{ "reconnect and CA-only TLS validation", test_reconnect_and_tls },
	{ "close with unacknowledged QoS", test_close_pending_qos },
	{ "rejected MQTT 5 CONNACK", test_rejected_connect },
	{ NULL, NULL }
};
