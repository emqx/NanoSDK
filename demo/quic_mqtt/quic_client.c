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
// The `pub` sub-command publishes a given message to the server and then exits.
// The `sub` sub-command subscribes to the given topic filter and blocks
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

#include <nng/nng.h>
#include <nng/supplemental/util/platform.h>
#include <nng/mqtt/mqtt_quic.h>
#include <nng/mqtt/mqtt_client.h>

#include "msquic.h"

#include <stdio.h>
#include <stdlib.h>

#define CONN 1
#define SUB 2
#define PUB 3

static nng_socket * g_sock;

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

static nng_msg *
mqtt_msg_compose(int type, int qos, char *topic, char *payload)
{
	// Mqtt connect message
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);

	if (type == CONN) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);

		nng_mqtt_msg_set_connect_proto_version(msg, 4);
		nng_mqtt_msg_set_connect_keep_alive(msg, 30);
		nng_mqtt_msg_set_connect_clean_session(msg, true);
	} else if (type == SUB) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

		nng_mqtt_topic_qos subscriptions[] = {
			{
				.qos   = qos,
				.topic = {
					.buf    = (uint8_t *) topic,
					.length = strlen(topic)
				}
			},
		};
		int count = sizeof(subscriptions) / sizeof(nng_mqtt_topic_qos);

		nng_mqtt_msg_set_subscribe_topics(msg, subscriptions, count);
	} else if (type == PUB) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);

		nng_mqtt_msg_set_publish_dup(msg, 0);
		nng_mqtt_msg_set_publish_qos(msg, qos);
		nng_mqtt_msg_set_publish_retain(msg, 0);
		nng_mqtt_msg_set_publish_topic(msg, topic);
		nng_mqtt_msg_set_publish_payload(
		    msg, (uint8_t *) payload, strlen(payload));
	}

	return msg;
}

static int
connect_cb(void *rmsg, void * arg)
{
	printf("[Connected][%s]...\n", (char *)arg);
	return 0;
}

static int
disconnect_cb(void *rmsg, void * arg)
{
	printf("[Disconnected][%s]...\n", (char *)arg);
	return 0;
}

static int
msg_send_cb(void *rmsg, void * arg)
{
	printf("[Msg Sent][%s]...\n", (char *)arg);
	return 0;
}

static int
msg_recv_cb(void *rmsg, void * arg)
{
	printf("[Msg Arrived][%s]...\n", (char *)arg);
	nng_msg *msg = rmsg;
	uint32_t topicsz, payloadsz;

	char *topic   = (char *)nng_mqtt_msg_get_publish_topic(msg, &topicsz);
	char *payload = (char *)nng_mqtt_msg_get_publish_payload(msg, &payloadsz);

	printf("topic   => %.*s\n"
	       "payload => %.*s\n",topicsz, topic, payloadsz, payload);
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
	nng_msg *msg = mqtt_msg_compose(3, 1, "topic123", "hello quic");

	for (;;) {
		nng_msleep(1000);
		nng_msg *smsg;
		nng_msg_dup(&smsg, msg);
		nng_sendmsg(*sock, smsg, NNG_FLAG_NONBLOCK);
	}
}

int
client(int type, const char *url, const char *qos, const char *topic, const char *data)
{
	nng_socket  sock;
	int         rv, sz, q;
	nng_msg *   msg;
	const char *arg = "CLIENT FOR QUIC";

	/*
	// Open a quic socket without configuration
	if ((rv = nng_mqtt_quic_client_open(&sock, url)) != 0) {
		printf("error in quic client open.\n");
	}
	*/

	if ((rv = nng_mqtt_quic_client_open_conf(&sock, url, &config_user)) != 0) {
		printf("error in quic client open.\n");
	}

#if defined(NNG_SUPP_SQLITE)
	sqlite_config(&sock, MQTT_PROTOCOL_VERSION_v311);
#endif

	if (0 != nng_mqtt_quic_set_connect_cb(&sock, connect_cb, (void *)arg) ||
	    0 != nng_mqtt_quic_set_disconnect_cb(&sock, disconnect_cb, (void *)arg) ||
	    0 != nng_mqtt_quic_set_msg_recv_cb(&sock, msg_recv_cb, (void *)arg) ||
	    0 != nng_mqtt_quic_set_msg_send_cb(&sock, msg_send_cb, (void *)arg)) {
		printf("error in quic client cb set.\n");
	}
	g_sock = &sock;

	// MQTT Connect...
	msg = mqtt_msg_compose(CONN, 0, NULL, NULL);
	nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);

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
		msg = mqtt_msg_compose(SUB, q, (char *)topic, NULL);
		nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);

		break;
	case PUB:
		msg = mqtt_msg_compose(PUB, q, (char *)topic, (char *)data);
		nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);

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

	nng_close(sock);
	fprintf(stderr, "Done.\n");

	return (0);
}

static void
printf_helper(char *exec)
{
	fprintf(stderr, "Usage: %s conn <url>\n"
	                "       %s sub  <url> <qos> <topic>\n"
	                "       %s pub  <url> <qos> <topic> <data>\n", exec, exec, exec);
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
	}
	else if (0 == strncmp(argv[1], "sub", 3)  && argc == 5) {
		client(SUB, argv[2], argv[3], argv[4], NULL);
	}
	else if (0 == strncmp(argv[1], "pub", 3)  && argc == 6) {
		client(PUB, argv[2], argv[3], argv[4], argv[5]);
	}
	else {
		goto error;
	}

	return 0;

error:

	printf_helper(argv[0]);
	return 0;
}
