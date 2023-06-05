// Author: wangha <wangwei at emqx dot io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

//
// This is just a simple MQTT client demonstration application.
//
// The application has three sub-commands: `conn` `pub` and `sub`.
// The `conn` sub-command connects to the server.
// The `pub` sub-command publishes a given message to the server and then
// exits. The `sub` sub-command subscribes to the given topic filter and blocks
// waiting for incoming messages.
//
// # Example:
//
// Connect to the specific server:
// ```
// $ ./quic_client conn 'mqtt-quic://127.0.0.1:14567'
// ```
//
// Subscribe to `topic` and waiting for messages:
// ```
// $ ./quic_client sub 'mqtt-tcp://127.0.0.1:14567' topic
// ```
//
// Publish 'hello' to `topic`:
// ```
// $ ./quic_client pub 'mqtt-tcp://127.0.0.1:14567' topic hello
// ```
//

#include <nng/mqtt/mqtt_client.h>
#include <nng/mqtt/mqtt_quic.h>
#include <nng/nng.h>
#include <nng/supplemental/util/platform.h>

#include "msquic.h"

#include <stdio.h>
#include <stdlib.h>

#define CONN 1
#define SUB 2
#define PUB 3

static nng_socket *g_sock;

conf_quic config_user = {
	.tls = {
		.enable = false,
		.cafile = "",
		.certfile = "",
		.keyfile  = "",
		.key_password = "",
		.verify_peer = true,
		.set_fail = true,
	},
	.multi_stream = false,
	.qos_first  = false,
	.qkeepalive = 30,
	.qconnect_timeout = 60,
	.qdiscon_timeout = 30,
	.qidle_timeout = 30,
};

static void
fatal(const char *msg, int rv)
{
	fprintf(stderr, "%s: %s\n", msg, nng_strerror(rv));
}

void
print_property(property *prop)
{
	if (prop == NULL) {
		return;
	}

	// printf("%d \n", prop->id);

	uint8_t type    = prop->data.p_type;
	uint8_t prop_id = prop->id;
	switch (type) {
	case U8:
		printf("id: %d, value: %d (U8)\n", prop_id,
		    prop->data.p_value.u8);
		break;
	case U16:
		printf("id: %d, value: %d (U16)\n", prop_id,
		    prop->data.p_value.u16);
		break;
	case U32:
		printf("id: %d, value: %u (U32)\n", prop_id,
		    prop->data.p_value.u32);
		break;
	case VARINT:
		printf("id: %d, value: %d (VARINT)\n", prop_id,
		    prop->data.p_value.varint);
		break;
	case BINARY:
		printf("id: %d, value pointer: %p (BINARY)\n", prop_id,
		    prop->data.p_value.binary.buf);
		break;
	case STR:
		printf("id: %d, value: %.*s (STR)\n", prop_id,
		    prop->data.p_value.str.length,
		    (const char *) prop->data.p_value.str.buf);
		break;
	case STR_PAIR:
		printf("id: %d, value: '%.*s -> %.*s' (STR_PAIR)\n", prop_id,
		    prop->data.p_value.strpair.key.length,
		    prop->data.p_value.strpair.key.buf,
		    prop->data.p_value.strpair.value.length,
		    prop->data.p_value.strpair.value.buf);
		break;

	default:
		break;
	}
}

static nng_msg*
compose_connect()
{
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	// test property
	property *p  = mqtt_property_alloc();
	property *p1 = mqtt_property_set_value_u32(MAXIMUM_PACKET_SIZE, 120);
	property *p2 =
	    mqtt_property_set_value_u32(SESSION_EXPIRY_INTERVAL, 120);
	property *p3 = mqtt_property_set_value_u16(RECEIVE_MAXIMUM, 120);
	property *p4 = mqtt_property_set_value_u32(MAXIMUM_PACKET_SIZE, 120);
	property *p5 = mqtt_property_set_value_u16(TOPIC_ALIAS_MAXIMUM, 120);
	mqtt_property_append(p, p1);
	mqtt_property_append(p, p2);
	mqtt_property_append(p, p3);
	mqtt_property_append(p, p4);
	mqtt_property_append(p, p5);
	nng_mqtt_msg_set_connect_property(msg, p);

	nng_mqtt_msg_set_connect_proto_version(msg, MQTT_PROTOCOL_VERSION_v5);
	nng_mqtt_msg_set_connect_keep_alive(msg, 30);
	nng_mqtt_msg_set_connect_clean_session(msg, true);

	return msg;
}

static nng_msg *
compose_subscribe(int qos, char *topic)
{
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);


	nng_mqtt_topic_qos subscriptions[] = {
		{ .qos     = qos,
		    .topic = { .buf = (uint8_t *) topic,
		        .length     = strlen(topic) } },
	};

	int count = sizeof(subscriptions) / sizeof(nng_mqtt_topic_qos);

	nng_mqtt_msg_set_subscribe_topics(msg, subscriptions, count);
	property *p = mqtt_property_alloc();
	property *p1 =
	    mqtt_property_set_value_varint(SUBSCRIPTION_IDENTIFIER, 120);
	mqtt_property_append(p, p1);
	nng_mqtt_msg_set_subscribe_property(msg, p);
	return msg;
}

static nng_msg *
compose_publish(int qos, char *topic, char *payload)
{
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
	property *plist = mqtt_property_alloc();
	property *p1 = mqtt_property_set_value_u8(PAYLOAD_FORMAT_INDICATOR, 1);
	mqtt_property_append(plist, p1);
	property *p2 = mqtt_property_set_value_u16(TOPIC_ALIAS, 10);
	mqtt_property_append(plist, p2);
	property *p3 =
	    mqtt_property_set_value_u32(MESSAGE_EXPIRY_INTERVAL, 10);
	mqtt_property_append(plist, p3);
	property *p4 = mqtt_property_set_value_str(
	    RESPONSE_TOPIC, "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p4);
	property *p5 = mqtt_property_set_value_binary(
	    CORRELATION_DATA, (uint8_t *) "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p5);
	property *p6 = mqtt_property_set_value_strpair(USER_PROPERTY, "aaaaaa",
	    strlen("aaaaaa"), "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p6);
	property *p7 = mqtt_property_set_value_str(
	    CONTENT_TYPE, "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p7);

	nng_mqtt_msg_set_publish_property(msg, plist);

	nng_mqtt_msg_set_publish_dup(msg, 0);
	nng_mqtt_msg_set_publish_qos(msg, qos);
	nng_mqtt_msg_set_publish_retain(msg, 0);
	nng_mqtt_msg_set_publish_topic(msg, topic);
	nng_mqtt_msg_set_publish_payload(
	    msg, (uint8_t *) payload, strlen(payload));
	
	return msg;
}

static int
connect_cb(void *rmsg, void *arg)
{
	printf("[Connected][%s]...\n", (char *) arg);
	return 0;
}

static int
disconnect_cb(void *rmsg, void *arg)
{
	printf("[Disconnected][%s]...\n", (char *) arg);
	return 0;
}

static int
msg_send_cb(void *rmsg, void *arg)
{
	printf("[Msg Sent][%s]...\n", (char *) arg);
	return 0;
}

static int
msg_recv_cb(void *rmsg, void *arg)
{
	printf("[Msg Arrived][%s]...\n", (char *) arg);
	nng_msg *msg = rmsg;
	uint32_t topicsz, payloadsz;

	char *topic = (char *) nng_mqtt_msg_get_publish_topic(msg, &topicsz);
	char *payload =
	    (char *) nng_mqtt_msg_get_publish_payload(msg, &payloadsz);

	printf("topic   => %.*s\n"
	       "payload => %.*s\n",
	    topicsz, topic, payloadsz, payload);

	property *pl = nng_mqtt_msg_get_publish_property(msg);
	if (pl != NULL) {
		mqtt_property_foreach(pl, print_property);
	}
	return 0;
}

static int
sqlite_config(nng_socket *sock, uint8_t proto_ver)
{
#if defined(NNG_SUPP_SQLITE)
	int rv;
	// create sqlite option
	nng_mqtt_sqlite_option *sqlite;
	if ((rv = nng_mqtt_alloc_sqlite_opt(&sqlite)) != 0) {
		fatal("nng_mqtt_alloc_sqlite_opt", rv);
	}
	// set sqlite option
	nng_mqtt_set_sqlite_enable(sqlite, true);
	nng_mqtt_set_sqlite_flush_threshold(sqlite, 10);
	nng_mqtt_set_sqlite_max_rows(sqlite, 20);
	nng_mqtt_set_sqlite_db_dir(sqlite, "/tmp/nanomq");

	// init sqlite db
	nng_mqtt_sqlite_db_init(sqlite, "mqtt_quic_client.db", proto_ver);

	// set sqlite option pointer to socket
	return nng_socket_set_ptr(*sock, NNG_OPT_MQTT_SQLITE, sqlite);
#else
	return (0);
#endif
}

static void
sendmsg_func(void *arg)
{
	nng_socket *sock = arg;
	nng_msg    *msg  = compose_publish(1, "topic123", "hello quic");

	for (;;) {
		nng_msleep(1000);
		nng_msg *smsg;
		nng_msg_dup(&smsg, msg);
		nng_sendmsg(*sock, smsg, NNG_FLAG_NONBLOCK);
	}
}

int
client(int type, const char *url, const char *qos, const char *topic,
    const char *data)
{
	nng_socket  sock;
	int         rv, sz, q;
	nng_msg    *msg;
	const char *arg = "CLIENT FOR QUIC";

	if ((rv = nng_mqttv5_quic_client_open_conf(
	         &sock, url, &config_user)) != 0) {
		printf("error in quic client open.\n");
	}

#if defined(NNG_SUPP_SQLITE)
	sqlite_config(&sock, MQTT_PROTOCOL_VERSION_v311);
#endif

	if (0 !=
	        nng_mqtt_quic_set_connect_cb(
	            &sock, connect_cb, (void *) arg) ||
	    0 !=
	        nng_mqtt_quic_set_disconnect_cb(
	            &sock, disconnect_cb, (void *) arg) ||
	    0 !=
	        nng_mqtt_quic_set_msg_recv_cb(
	            &sock, msg_recv_cb, (void *) arg) ||
	    0 !=
	        nng_mqtt_quic_set_msg_send_cb(
	            &sock, msg_send_cb, (void *) arg)) {
		printf("error in quic client cb set.\n");
	}
	g_sock = &sock;

	// MQTT Connect...
	msg = compose_connect();
	nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);

	uint8_t buff[1024] = { 0 };
	nng_mqtt_msg_dump(msg, buff, sizeof(buff), true);
	printf("%s\n", buff);

	property *pl = nng_mqtt_msg_get_connect_property(msg);
	if (pl != NULL) {
		mqtt_property_foreach(pl, print_property);
	}

	if (qos) {
		q = atoi(qos);
		if (q < 0 || q > 2) {
			printf("Qos should be in range(0~2).\n");
			q = 0;
		}
	}

	switch (type) {
	case CONN:
		break;
	case SUB:
		msg = compose_subscribe(q, (char *) topic);
		nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);

		break;
	case PUB:
		if (!strcmp("-l", data) || !strcmp("--stdin-line", data)) {
			while (1) {
				uint8_t *payload     = NULL;
				size_t msg_len = 0;
				size_t len;
				if ((msg_len = getline((char**)&(payload), &len, stdin)) == -1) {
					fprintf(stderr, "Read line error!\n");
				}
				payload[msg_len-1] = '\0';
				if (msg_len > 1)  {
					msg = compose_publish(q, (char *) topic, (char *) payload);
					// pl = nng_mqtt_msg_get_publish_property(msg);
					// if (pl != NULL) {
					// 	mqtt_property_foreach(pl, print_property);
					// }
					nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);
				}
			}
		}
		msg = compose_publish(q, (char *) topic, (char *) data);
		pl = nng_mqtt_msg_get_publish_property(msg);
		if (pl != NULL) {
			mqtt_property_foreach(pl, print_property);
		}
		nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);
		goto done;

#if defined(NNG_SUPP_SQLITE)
		nng_thread *thr;
		nng_thread_create(&thr, sendmsg_func, &sock);
#endif

		break;
	default:
		printf("Unknown command.\n");
	}

	for (;;)
		nng_msleep(1000);

done:
	nng_close(sock);
	fprintf(stderr, "Done.\n");

	return (0);
}

static void
printf_helper(char *exec)
{
	fprintf(stderr,
	    "Usage: %s conn <url>\n"
	    "       %s sub  <url> <qos> <topic>\n"
	    "       %s pub  <url> <qos> <topic> <data> or <stdin-line>\n",
	    exec, exec, exec);
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	int rc;

	if (argc < 3) {
		goto error;
	}
	if (0 == strncmp(argv[1], "conn", 4) && argc == 3) {
		client(CONN, argv[2], NULL, NULL, NULL);
	} else if (0 == strncmp(argv[1], "sub", 3) && argc == 5) {
		client(SUB, argv[2], argv[3], argv[4], NULL);
	} else if (0 == strncmp(argv[1], "pub", 3) && argc == 6) {
		client(PUB, argv[2], argv[3], argv[4], argv[5]);
	} else {
		goto error;
	}

	return 0;

error:

	printf_helper(argv[0]);
	return 0;
}
