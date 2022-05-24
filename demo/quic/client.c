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
#include <nng/mqtt/mqtt_quic.h>
#include <nng/mqtt/mqtt_client.h>

#include "msquic.h"

#include <stdio.h>
#include <stdlib.h>

#define CLIENT_SEND_Q_SZ 4

static nng_msg * send_q[CLIENT_SEND_Q_SZ];
static int send_q_pos = 0;
static int send_q_sz = 0;

static nng_socket * g_sock;

static inline void
put_send_q(nng_msg *msg)
{
	if (send_q_sz == 4) {
		printf("Msg Send Queue Overflow.\n");
		return;
	}
	send_q[send_q_pos] = msg;
	send_q_pos = (++send_q_pos) % CLIENT_SEND_Q_SZ;
	send_q_sz ++;
}

static inline nng_msg *
get_send_q()
{
	nng_msg *msg;
	if (send_q_sz == 0) {
		printf("Msg Send Queue Is Empty.\n");
		return NULL;
	}
	send_q_pos = (--send_q_pos) % CLIENT_SEND_Q_SZ;
	msg = send_q[send_q_pos];
	send_q_sz --;
	return msg;
}

static nng_msg *
mqtt_msg_compose(int type, char *topic, char *payload)
{
	// Mqtt connect message
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);

	if (type == 1) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);

		nng_mqtt_msg_set_connect_keep_alive(msg, 180);
		nng_mqtt_msg_set_connect_clean_session(msg, true);

		nng_mqtt_msg_set_connect_keep_alive(msg, 180);
		nng_mqtt_msg_set_connect_clean_session(msg, true);
	} else if (type == 2) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

		int qos   = 0;
		int count = 1;

		nng_mqtt_topic_qos subscriptions[] = {
			{
				.qos   = qos,
				.topic = {
					.buf    = (uint8_t *) topic,
					.length = strlen(topic)
				}
			},
		};

		nng_mqtt_msg_set_subscribe_topics(msg, subscriptions, count);
	} else if (type == 3) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
		int qos   = 0;

		nng_mqtt_msg_set_publish_dup(msg, 0);
		nng_mqtt_msg_set_publish_qos(msg, qos);
		nng_mqtt_msg_set_publish_retain(msg, 0);
		nng_mqtt_msg_set_publish_topic(msg, topic);
		nng_mqtt_msg_set_publish_payload(
		    msg, (uint8_t *) payload, strlen(payload));
	}

	// nng_mqtt_msg_encode(msg);

	return msg;
}

static int
connect_cb(void * arg)
{
	printf("[Connected]...\n");

	nng_msg *msg;
	while (send_q_sz > 0) {
		msg = get_send_q();
		nng_sendmsg(*g_sock, msg, NNG_FLAG_ALLOC);
	}
}

static int
msg_send_cb(void * arg)
{
	printf("[Msg Sent]...\n");
}

static int
msg_recv_cb(void * arg)
{
	printf("[Msg Arrived]...\n");
	nng_msg *msg = arg;
	uint32_t topicsz, payloadsz;

	char *topic   = (char *)nng_mqtt_msg_get_publish_topic(msg, &topicsz);
	char *payload = nng_mqtt_msg_get_publish_payload(msg, &payloadsz);

	printf("topic   => %.*s\n"
	       "payload => %.*s\n",topicsz, topic, payloadsz, payload);
}

int
client(int type, const char *url, const char *topic, const char *data)
{
	nng_socket sock;
	int        rv, sz;
	nng_msg *  msg;

	if ((rv = nng_mqtt_quic_client_open(&sock, url)) != 0) {
		printf("error in quic client open.\n");
	}
	if (0 != nng_mqtt_quic_set_connect_cb(&sock, connect_cb) ||
	    0 != nng_mqtt_quic_set_msg_recv_cb(&sock, msg_recv_cb) ||
	    0 != nng_mqtt_quic_set_msg_send_cb(&sock, msg_send_cb)) {
		printf("error in quic client cb set.\n");
	}
	g_sock = &sock;

	// MQTT Connect...
	msg = mqtt_msg_compose(1, NULL, NULL);
	nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);

	switch (type) {
	case 1:
		break;
	case 2:
		msg = mqtt_msg_compose(2, (char *)topic, NULL);
		put_send_q(msg);
		break;
	case 3:
		msg = mqtt_msg_compose(3, (char *)topic, (char *)data);
		put_send_q(msg);
		break;
	default:
		printf("Unknown command.\n");
	}

	for (;;)
		nng_msleep(1000);

	nng_close(sock);

	return (0);
}

static void
printf_helper(char *exec)
{
	fprintf(stderr, "Usage: %s conn <url>\n"
	                "       %s sub  <url> <topic>\n"
	                "       %s pub  <url> <topic> <data>\n", exec, exec, exec);
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	int rc;
	memset(send_q, 0, sizeof(send_q));

	if (argc < 3)
		printf_helper(argv[0]);
	if (0 == strncmp(argv[1], "conn", 4) && argc == 3)
		client(1, argv[2], NULL, NULL);
	if (0 == strncmp(argv[1], "sub", 3)  && argc == 4)
		client(2, argv[2], argv[3], NULL);
	if (0 == strncmp(argv[1], "pub", 3)  && argc == 5)
		client(3, argv[2], argv[3], argv[4]);

	printf_helper(argv[1]);
}
