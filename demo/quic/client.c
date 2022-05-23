//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// This is a very simple HTTP client.  It only performs HTTP GET
// operations, and does not follow HTTP redirects.  Think of it as
// a trivialized version of CURL.  It is super simple, taking the
// URL on the command line, and emitting the results to stdout.
// For clarity, we are eliding TLS support.

// It may not work on all systems, but it should work anywhere that
// both the standard C library and nng itself are available.

// We check for errors, but no effort is made to clean up resources,
// since this program just exits.  In longer running programs or libraries,
// callers should take care to clean up things that they allocate.

// Unfortunately many famous sites use redirects, so you won't see that
// emitted.

// Example usage:
//
// % export CPPFLAGS="-I /usr/local/include"
// % export LDFLAGS="-L /usr/local/lib -lnng"
// % export CC="cc"
// % ./quic_client conn url
// % ./quic_client sub  url
// % ./quic_client pub  url
//

#include <nng/nng.h>
#include <nng/mqtt/mqtt_quic.h>
#include <nng/mqtt/mqtt_client.h>

#include "msquic.h"

#include <stdio.h>
#include <stdlib.h>

void
fatal(char *msg, int rv)
{
	fprintf(stderr, "%s, %s\n", msg, nng_strerror(rv));
	exit(1);
}

static nng_msg *
mqtt_msg_compose(int type)
{
	// Mqtt connect message
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);

	if (type == 1) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);

		nng_mqtt_msg_set_connect_keep_alive(msg, 180);
		nng_mqtt_msg_set_connect_clean_session(msg, true);

		nng_mqtt_msg_set_connect_will_topic(msg, "topic");
		char *willmsg = "will \n test";
		nng_mqtt_msg_set_connect_will_msg(msg, willmsg, 12);
		nng_mqtt_msg_set_connect_keep_alive(msg, 180);
		nng_mqtt_msg_set_connect_clean_session(msg, true);
	} else if (type == 2) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

		int qos   = 0;
		int count = 1;
		char *topic1 = "topic1";

		nng_mqtt_topic_qos subscriptions[] = {
			{
				.qos   = qos,
				.topic = {
					.buf    = (uint8_t *) topic1,
					.length = strlen(topic1)
				}
			},
		};

		nng_mqtt_msg_set_subscribe_topics(msg, subscriptions, count);
	} else if (type == 3) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
		int qos   = 0;
		char *topic = "topic1";

		nng_mqtt_msg_set_publish_dup(msg, 0);
		nng_mqtt_msg_set_publish_qos(msg, qos);
		nng_mqtt_msg_set_publish_retain(msg, 0);
		nng_mqtt_msg_set_publish_payload(
		    msg, (uint8_t *) "Hllo world.", 11);
		nng_mqtt_msg_set_publish_topic(msg, topic);
	}

	nng_mqtt_msg_encode(msg);

	return msg;
}

static int
connect_cb(void * arg)
{
	printf("Connected.\n");
}

static int
msg_send_cb(void * arg)
{
	printf("Msg Sent.\n");
}

static int
msg_recv_cb(void * arg)
{
	printf("Msg Arrived.\n");
}

int
client(const char *type, const char *url)
{
	nng_socket sock;
	int        rv;
	nng_msg *  msg;

	if ((rv = nng_mqtt_quic_client_open(&sock, url)) != 0) {
		printf("error in quic client open.\n");
	}
	if (0 != nng_mqtt_quic_set_connect_cb(&sock, connect_cb) ||
	    0 != nng_mqtt_quic_set_msg_recv_cb(&sock, msg_recv_cb) ||
	    0 != nng_mqtt_quic_set_msg_send_cb(&sock, msg_send_cb)) {
		printf("error in quic client cb set.\n");
	}

	nng_msleep(3000);

	if (0 == strncmp(type, "conn", 4)) {
		msg = mqtt_msg_compose(1);
		nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);
	} else if (0 == strncmp(type, "sub", 3)) {
		msg = mqtt_msg_compose(1);
		nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);
		nng_msleep(2000);
		msg = mqtt_msg_compose(2);
		nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);
		// wait msg
		nng_recvmsg(sock, &msg, NNG_FLAG_ALLOC);
	} else if (0 == strncmp(type, "pub", 3)) {
		msg = mqtt_msg_compose(1);
		nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);
		nng_msleep(2000);
		msg = mqtt_msg_compose(3);
		nng_sendmsg(sock, msg, NNG_FLAG_ALLOC);
	} else {
		printf("Unknown command.\n");
	}

	nng_close(sock);

	return (0);
}

int
main(int argc, char **argv)
{
	int rc;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <conn|sub|pub> <url>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	rc = client(argv[1], argv[2]);
	exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
