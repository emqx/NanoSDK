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
#include <nng/mqtt/mqtt_client.h>

#if defined(SUPP_QUIC)
#include <nng/mqtt/mqtt_quic_client.h>
#endif




#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONN 1
#define SUB 2
#define PUB 3

static nng_socket * g_sock;

static void
fatal(const char *msg, int rv)
{
	fprintf(stderr, "%s: %s\n", msg, nng_strerror(rv));
}

static int
parse_proto_ver(const char *text, uint8_t *proto_ver)
{
	char *end = NULL;
	long  val;

	if (text == NULL) {
		*proto_ver = MQTT_PROTOCOL_VERSION_v311;
		return 0;
	}

	val = strtol(text, &end, 10);
	if (end == text || *end != '\0') {
		return -1;
	}

	if (val != MQTT_PROTOCOL_VERSION_v311 &&
	    val != MQTT_PROTOCOL_VERSION_v5) {
		return -1;
	}

	*proto_ver = (uint8_t) val;
	return 0;
}

static nng_msg *
mqtt_msg_compose(
	int type, uint8_t proto_ver, int qos, char *topic, char *payload)
{
	// Mqtt connect message
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);

	if (type == CONN) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);

		nng_mqtt_msg_set_connect_proto_version(msg, proto_ver);
		nng_mqtt_msg_set_connect_keep_alive(msg, 10);
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
quic_connect_cb(void *rmsg, void *arg)
{
	struct connect_param *param  = arg;
	int                   reason = 0;

	printf("%s: connect\n", __FUNCTION__);

	return 0;
}

static int
quic_disconnect_cb(void *rmsg, void *arg)
{
	printf("bridge client disconnected!\n");
	return 0;
}

static void
print_publish_msg(nng_msg *msg)
{
	uint32_t topicsz, payloadsz;

	char *topic   = (char *)nng_mqtt_msg_get_publish_topic(msg, &topicsz);
	char *payload = (char *)nng_mqtt_msg_get_publish_payload(msg, &payloadsz);

	printf("topic   => %.*s\n"
	       "payload => %.*s\n",topicsz, topic, payloadsz, payload);
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
	nng_msg *msg = mqtt_msg_compose(
		PUB, MQTT_PROTOCOL_VERSION_v311, 1, "topic123", "hello quic");

	for (;;) {
		nng_msleep(1000);
		nng_msg *smsg;
		nng_msg_dup(&smsg, msg);
		nng_sendmsg(*sock, smsg, NNG_FLAG_NONBLOCK);
	}
}

static	nng_msg *   conn_msg;

int
client(int type, uint8_t proto_ver, const char *url, const char *qos,
	const char *topic, const char *data)
{
	nng_socket  sock;
	nng_dialer  dialer;
	int         rv, sz, q;

	const char *arg = "CLIENT FOR QUIC";

	/*
	// Open a quic socket without configuration
	if ((rv = nng_mqtt_quic_client_open(&sock, url)) != 0) {
		printf("error in quic client open.\n");
	}
	*/
#if defined(SUPP_QUIC)
	if (proto_ver == MQTT_PROTOCOL_VERSION_v5) {
		if ((rv = nng_mqttv5_quic_client_open(&sock)) != 0) {
			fatal("nng_mqttv5_quic_client_open", rv);
			return rv;
		}

		if (0 != nng_mqttv5_quic_set_connect_cb(
			        &sock, quic_connect_cb, (void *) arg) ||
		    0 != nng_mqttv5_quic_set_disconnect_cb(
			        &sock, quic_disconnect_cb, (void *) arg)) {
			printf("error in mqtt v5 client cb set.\n");
		}
	} else {
		if ((rv = nng_mqtt_quic_client_open(&sock)) != 0) {
			fatal("nng_mqtt_quic_client_open", rv);
			return rv;
		}

		if (0 != nng_mqtt_quic_set_connect_cb(
			        &sock, quic_connect_cb, (void *) arg) ||
		    0 != nng_mqtt_quic_set_disconnect_cb(
			        &sock, quic_disconnect_cb, (void *) arg)) {
			printf("error in mqtt v4 client cb set.\n");
		}
	}
#endif

#if defined(NNG_SUPP_SQLITE)
	sqlite_config(&sock, proto_ver);
#endif

	g_sock = &sock;

	// MQTT Connect...
	if ((rv = nng_dialer_create(&dialer, sock, url)) != 0) {
		fatal("nng_dialer_create", rv);
		nng_close(sock);
		return rv;
	}

	conn_msg = mqtt_msg_compose(CONN, proto_ver, 0, NULL, NULL);

	if ((rv = nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, conn_msg)) != 0) {
		fatal("nng_dialer_set_ptr", rv);
		nng_close(sock);
		return rv;
	}

	if ((rv = nng_dialer_start(dialer, NNG_FLAG_ALLOC)) != 0) {
		fatal("nng_dialer_start", rv);
		nng_close(sock);
		return rv;
	}
	// CONNECT is sent by transport from NNG_OPT_MQTT_CONNMSG. Re-sending the
	// same message would race with async send completion and corrupt ownership.

	if (qos) {
		q = atoi(qos);
		if (q < 0 || q > 2) {
			printf("Qos should be in range(0~2).\n");
			q = 0;
		}
	}

	nng_msg *   msg;

	switch (type) {
	case CONN:
		break;
	case SUB:
		msg = mqtt_msg_compose(SUB, proto_ver, q, (char *) topic, NULL);
		nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);

		for (;;) {
			nng_msg *rmsg = NULL;
			if ((rv = nng_recvmsg(sock, &rmsg, 0)) != 0) {
				fatal("nng_recvmsg", rv);
				nng_msleep(1000);
				continue;
			}
			if (nng_mqtt_msg_get_packet_type(rmsg) == NNG_MQTT_PUBLISH) {
				printf("[Msg Arrived][%s]...\n", arg);
				print_publish_msg(rmsg);
			}
			nng_msg_free(rmsg);
		}
	case PUB:
		msg = mqtt_msg_compose(
			PUB, proto_ver, q, (char *) topic, (char *) data);
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
	fprintf(stderr, "Usage: %s conn <url> [proto_ver(4|5)]\n"
	                "       %s sub  <url> <qos> <topic> [proto_ver(4|5)]\n"
	                "       %s pub  <url> <qos> <topic> <data> [proto_ver(4|5)]\n", exec, exec, exec);
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	int rc;
	uint8_t proto_ver = MQTT_PROTOCOL_VERSION_v311;

	if (argc < 3) {
		goto error;
	}
	if (0 == strncmp(argv[1], "conn", 4) && (argc == 3 || argc == 4)) {
		if (parse_proto_ver((argc == 4) ? argv[3] : NULL, &proto_ver) != 0) {
			fprintf(stderr, "Invalid proto_ver, only 4 or 5 is supported.\n");
			goto error;
		}
		client(CONN, proto_ver, argv[2], NULL, NULL, NULL);
	}
	else if (0 == strncmp(argv[1], "sub", 3)  && (argc == 5 || argc == 6)) {
		if (parse_proto_ver((argc == 6) ? argv[5] : NULL, &proto_ver) != 0) {
			fprintf(stderr, "Invalid proto_ver, only 4 or 5 is supported.\n");
			goto error;
		}
		client(SUB, proto_ver, argv[2], argv[3], argv[4], NULL);
	}
	else if (0 == strncmp(argv[1], "pub", 3)  && (argc == 6 || argc == 7)) {
		if (parse_proto_ver((argc == 7) ? argv[6] : NULL, &proto_ver) != 0) {
			fprintf(stderr, "Invalid proto_ver, only 4 or 5 is supported.\n");
			goto error;
		}
		client(PUB, proto_ver, argv[2], argv[3], argv[4], argv[5]);
	}
	else {
		goto error;
	}

	return 0;

error:

	printf_helper(argv[0]);
	return 0;
}
