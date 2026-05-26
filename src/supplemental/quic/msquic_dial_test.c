//
// Copyright 2023 NanoMQ Team, Inc. <wangwei@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <nuts.h>
#include "nng/mqtt/mqtt_client.h"
#include "nng/mqtt/mqtt_quic_client.h"
#include "nng/protocol/mqtt/mqtt_parser.h"

//static const char *quic_test_url  = "mqtt-quic://us.432121.xyz:14567";
static const char *quic_test_url  = "mqtt-quic://13.49.223.253:14567";
static const char *quic_test_url2 = "mqtt-quic://127.0.0.1:8";
static const char *quic_test_clientid = "quic-ut-clientid";
static const char *quic_test_topic = "quic-ut-topic1";
static const int   quic_test_qos = 0;
static const char *quic_test_payload = "quic-ut-payload";
static const uint8_t quic_test_rap = 0;
static const uint8_t quic_test_rh  = 0;
static const uint8_t quic_test_nolocal = 0;

void
test_msquic_stream_conn_refused(void)
{
	nng_stream_dialer *dialer;
	nng_aio *          aio;

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 5000); // 5 sec

	// port 8 is generally not used for anything.
	NUTS_PASS(nng_stream_dialer_alloc(&dialer, quic_test_url2));
	nng_stream_dialer_dial(dialer, aio);
	nng_aio_wait(aio);
	// NUTS_FAIL(nng_aio_result(aio), NNG_ECONNREFUSED);
	// SERVER_UNAVAILABLE is the reason code for MQTT.
	NUTS_FAIL(nng_aio_result(aio), SERVER_UNAVAILABLE);

	nng_aio_free(aio);
	nng_stream_dialer_free(dialer);
}

void
test_msquic_stream_connect(void)
{
	nng_stream_dialer *dialer;
	nng_aio *          aio;
	nng_stream *       s;
	void *             t;
	uint8_t *          buf;
	size_t             size = 4;

	// allocate messages
	NUTS_ASSERT((buf = nng_alloc(size)) != NULL);
	uint8_t connect[] = {
		0x10, 0x3f, 0x00, 0x04, 0x4d, 0x51, 0x54, 0x54, 0x04, 0xc6,
		0x00, 0x3c, 0x00, 0x0c, 0x54, 0x65, 0x73, 0x74, 0x2d, 0x43,
		0x6c, 0x69, 0x65, 0x6e, 0x74, 0x31, 0x00, 0x0a, 0x77, 0x69,
		0x6c, 0x6c, 0x5f, 0x74, 0x6f, 0x70, 0x69, 0x63, 0x00, 0x07,
		0x62, 0x79, 0x65, 0x2d, 0x62, 0x79, 0x65, 0x00, 0x05, 0x61,
		0x6c, 0x76, 0x69, 0x6e, 0x00, 0x09, 0x48, 0x48, 0x48, 0x31,
		0x32, 0x33, 0x34, 0x35, 0x36
	};
	size_t sz_connect = sizeof(connect) / sizeof(uint8_t);

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 5000); // 5 sec

	NUTS_PASS(nng_stream_dialer_alloc(&dialer, quic_test_url));
	nng_stream_dialer_dial(dialer, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));

	NUTS_ASSERT((s = nng_aio_get_output(aio, 0)) != NULL);

	// Instead of quic echo server. We test a mqtt quic server.
	// To get some data from mqtt quic server. We need to send a connect msg first.
	t = nuts_stream_send_start(s, connect, sz_connect);
	NUTS_PASS(nuts_stream_wait(t));

	t = nuts_stream_recv_start(s, buf, size);
	NUTS_PASS(nuts_stream_wait(t));

	nng_free(buf, size);
	// nng_stream_free(s);
	nng_stream_dialer_free(dialer);
	nng_aio_free(aio);
}

// One main stream Two sub stream
void
test_msquic_stream_multi_stream(void)
{
	nng_stream_dialer *dialer;
	nng_aio *          aio, *aio2, *aio3;
	nng_stream *       s, *s2, *s3;

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	NUTS_PASS(nng_aio_alloc(&aio2, NULL, NULL));
	NUTS_PASS(nng_aio_alloc(&aio3, NULL, NULL));
	nng_aio_set_timeout(aio, 5000); // 5 sec
	nng_aio_set_timeout(aio2, 5000); // 5 sec
	nng_aio_set_timeout(aio3, 5000); // 5 sec

	NUTS_PASS(nng_stream_dialer_alloc(&dialer, quic_test_url));

	nng_stream_dialer_dial(dialer, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));

	NUTS_TRUE((s = nng_aio_get_output(aio, 0)) != NULL);
	nng_stream_close(s);
	// nng_stream_free(s);

	nng_stream_dialer_dial(dialer, aio2);
	nng_aio_wait(aio2);
	NUTS_PASS(nng_aio_result(aio2));

	NUTS_TRUE((s2 = nng_aio_get_output(aio2, 0)) != NULL);
	nng_stream_close(s2);
	// nng_stream_free(s2);

	nng_stream_dialer_dial(dialer, aio3);
	nng_aio_wait(aio3);
	NUTS_PASS(nng_aio_result(aio3));

	NUTS_TRUE((s3 = nng_aio_get_output(aio3, 0)) != NULL);
	nng_stream_close(s3);
	// nng_stream_free(s3);

	nng_aio_free(aio);
	nng_aio_free(aio2);
	nng_aio_free(aio3);
	nng_stream_dialer_free(dialer);
}

static inline nng_msg *
create_connect_msg(uint16_t ver, bool cs, char *clientid)
{
	// create a CONNECT message
	nng_msg *connmsg;
	nng_mqtt_msg_alloc(&connmsg, 0);
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_keep_alive(connmsg, 60);
	nng_mqtt_msg_set_connect_proto_version(connmsg, ver);
	nng_mqtt_msg_set_connect_clean_session(connmsg, cs);
	nng_mqtt_msg_set_connect_client_id(connmsg, clientid);

	if (ver == MQTT_PROTOCOL_VERSION_v5) {
		nng_mqttv5_msg_encode(connmsg);
	} else {
		nng_mqtt_msg_encode(connmsg);
	}
	return connmsg;
}

static inline nng_msg *
create_subscribe_msg(const char *topic, uint8_t qos, uint8_t nolocal,
	uint8_t rap, uint8_t rh, property *props)
{
	// create a SUBSCRIBE message
	nng_msg *submsg;
	int cnt = 1;
	nng_mqtt_msg_alloc(&submsg, 0);
	nng_mqtt_msg_set_packet_type(submsg, NNG_MQTT_SUBSCRIBE);
	nng_mqtt_topic_qos *topic_qos = nng_mqtt_topic_qos_array_create(cnt);
	nng_mqtt_topic_qos_array_set(topic_qos, 0, topic, qos, nolocal, rap, rh);
	nng_mqtt_msg_set_subscribe_topics(submsg, topic_qos, cnt);
	if (props)
		nng_mqtt_msg_set_subscribe_property(submsg, props);
	nng_mqtt_topic_qos_array_free(topic_qos, cnt);

	return submsg;
}

static inline nng_msg *
create_publish_msg(const char *topic, uint8_t *payload, uint32_t len, bool dup,
    uint8_t qos, bool retain, property *props)
{
	// create a PUBLISH message
	nng_msg *pubmsg;
	nng_mqtt_msg_alloc(&pubmsg, 0);
	nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_dup(pubmsg, dup);
	nng_mqtt_msg_set_publish_qos(pubmsg, qos);
	nng_mqtt_msg_set_publish_retain(pubmsg, retain);
	nng_mqtt_msg_set_publish_payload(pubmsg, payload, len);
	nng_mqtt_msg_set_publish_topic(pubmsg, topic);
	if (props) {
		nng_mqtt_msg_set_publish_property(pubmsg, props);
	}

	return pubmsg;
}

void
test_msquic_app_conn_refuse(void)
{
	nng_socket sock;
	nng_dialer dialer;
	nng_msg *connmsg = create_connect_msg(MQTT_PROTOCOL_VERSION_v311,
			true, (char *)quic_test_clientid);
	NUTS_ASSERT(connmsg != NULL);

	NUTS_PASS(nng_mqtt_quic_client_open(&sock));
	NUTS_PASS(nng_dialer_create(&dialer, sock, quic_test_url2));
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg));
	NUTS_PASS(nng_socket_set_ptr(sock, NNG_OPT_MQTT_CONNMSG, connmsg));
	//NUTS_PASS(nng_mqtt_set_connect_cb(sock, test_msquic_connect_cb, NULL));
	//NUTS_PASS(nng_mqtt_set_disconnect_cb(sock, test_msquic_disconnect_cb, NULL));
	// Wait connect failed
	NUTS_FAIL(nng_dialer_start(dialer, NNG_FLAG_ALLOC), SERVER_UNAVAILABLE);
}

static void
test_msquic_disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	(void)(p);
	(void)(ev);
	nng_msg *connmsg = arg;
	void    *cparam;

	NUTS_ASSERT((cparam = nng_msg_get_conn_param(connmsg)) != NULL);
	conn_param_free(cparam);
}

void
test_msquic_app_connect(void)
{
	nng_socket sock;
	nng_dialer dialer;
	nng_msg *connmsg = create_connect_msg(MQTT_PROTOCOL_VERSION_v311,
			true, (char *)quic_test_clientid);
	NUTS_ASSERT(connmsg != NULL);

	NUTS_PASS(nng_mqtt_quic_client_open(&sock));
	NUTS_PASS(nng_dialer_create(&dialer, sock, quic_test_url));
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg));
	NUTS_PASS(nng_socket_set_ptr(sock, NNG_OPT_MQTT_CONNMSG, connmsg));
	//NUTS_PASS(nng_mqtt_set_connect_cb(sock, test_msquic_connect_cb, NULL));
	NUTS_PASS(nng_mqtt_set_disconnect_cb(sock, test_msquic_disconnect_cb, connmsg));
	NUTS_PASS(nng_dialer_start(dialer, NNG_FLAG_ALLOC));
}

void
test_msquic_app_pub(void)
{
	// Connect...
	nng_socket sock;
	nng_dialer dialer;
	nng_msg *connmsg = create_connect_msg(MQTT_PROTOCOL_VERSION_v311,
			true, (char *)quic_test_clientid);
	NUTS_ASSERT(connmsg != NULL);

	NUTS_PASS(nng_mqtt_quic_client_open(&sock));
	NUTS_PASS(nng_dialer_create(&dialer, sock, quic_test_url));
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg));
	NUTS_PASS(nng_socket_set_ptr(sock, NNG_OPT_MQTT_CONNMSG, connmsg));
	//NUTS_PASS(nng_mqtt_set_connect_cb(sock, test_msquic_connect_cb, NULL));
	NUTS_PASS(nng_mqtt_set_disconnect_cb(sock, test_msquic_disconnect_cb, connmsg));
	NUTS_PASS(nng_dialer_start(dialer, NNG_FLAG_ALLOC));

	// Publish
	nng_msg *pubmsg = create_publish_msg(quic_test_topic, (uint8_t *)quic_test_payload,
		strlen(quic_test_payload), 0, quic_test_qos, 0, NULL);
	nng_mqttv5_msg_encode(pubmsg); // TODO Why no bug? It's a v311 client
	nng_sendmsg(sock, pubmsg, NNG_FLAG_ALLOC);
}

void
test_msquic_app_sub(void)
{
	// Connect...
	nng_socket sock;
	nng_dialer dialer;
	nng_msg *connmsg = create_connect_msg(MQTT_PROTOCOL_VERSION_v311,
			true, (char *)quic_test_clientid);
	NUTS_ASSERT(connmsg != NULL);

	NUTS_PASS(nng_mqtt_quic_client_open(&sock));
	NUTS_PASS(nng_dialer_create(&dialer, sock, quic_test_url));
	NUTS_PASS(nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg));
	NUTS_PASS(nng_socket_set_ptr(sock, NNG_OPT_MQTT_CONNMSG, connmsg));
	//NUTS_PASS(nng_mqtt_set_connect_cb(sock, test_msquic_connect_cb, NULL));
	NUTS_PASS(nng_mqtt_set_disconnect_cb(sock, test_msquic_disconnect_cb, connmsg));
	NUTS_PASS(nng_dialer_start(dialer, NNG_FLAG_ALLOC));

	// Subscribe
	nng_msg *submsg = create_subscribe_msg(quic_test_topic, quic_test_qos,
		quic_test_nolocal, quic_test_rap, quic_test_rh, NULL);
	NUTS_ASSERT(submsg != NULL);
	NUTS_PASS(nng_mqtt_msg_encode(submsg));
	printf("Wait here0\n");
	NUTS_ASSERT(0 == nng_sendmsg(sock, submsg, NNG_FLAG_ALLOC));

	// Publish
	nng_msg *pubmsg = create_publish_msg(quic_test_topic, (uint8_t *)quic_test_payload,
		strlen(quic_test_payload), 0, quic_test_qos, 0, NULL);
	NUTS_ASSERT(pubmsg != NULL);
	NUTS_PASS(nng_mqtt_msg_encode(pubmsg));
	NUTS_PASS(nng_sendmsg(sock, pubmsg, NNG_FLAG_ALLOC));

	// Start to receive connack and publish
	nng_msg *newmsg;
	NUTS_PASS(nng_recvmsg(sock, &newmsg, NNG_FLAG_ALLOC));
	NUTS_ASSERT(nng_mqtt_msg_get_packet_type(newmsg) == NNG_MQTT_CONNACK);
	nng_msg_free(newmsg);
	NUTS_PASS(nng_recvmsg(sock, &newmsg, NNG_FLAG_ALLOC));
	NUTS_ASSERT(nng_mqtt_msg_get_packet_type(newmsg) == NNG_MQTT_PUBLISH);
	nng_msg_free(newmsg);
	/*
	printf("Wait here4\n");
	NUTS_PASS(nng_recvmsg(sock, &newmsg, NNG_FLAG_ALLOC));
	NUTS_ASSERT(nng_mqtt_msg_get_packet_type(newmsg) == NNG_MQTT_SUBACK);
	nng_msg_free(newmsg);
	*/
}

TEST_LIST = {
	{ "msquic stream layer connect refused", test_msquic_stream_conn_refused },
	{ "msquic stream layer connection", test_msquic_stream_connect},
	{ "msquic stream layer multi-stream", test_msquic_stream_multi_stream },
	// And follow tests are app layer test.
	{ "msquic app layer connect refuse", test_msquic_app_conn_refuse },
	{ "msquic app layer connect", test_msquic_app_connect },
	{ "msquic app layer publish", test_msquic_app_pub },
	{ "msquic app layer subscribe", test_msquic_app_sub },
	{ NULL, NULL },
};
