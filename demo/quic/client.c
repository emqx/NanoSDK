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

#include "msquic.h"

#include <stdio.h>
#include <stdlib.h>

// Config for msquic
const QUIC_API_TABLE* MsQuic;
HQUIC Registration;
HQUIC Configuration;
HQUIC GStream;

void
fatal(char *msg, int rv)
{
	fprintf(stderr, "%s, %s\n", msg, nng_strerror(rv));
	exit(1);
}

void
QuicMqttSend(
    _In_ HQUIC Connection,
	_In_ HQUIC Stream,
	_In_ int type
    )
{
	/*
    QUIC_STATUS Status;
    uint8_t* SendBufferRaw;
    QUIC_BUFFER* SendBuffer;

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

		int qos = 0;
		int count = 1;

		nng_mqtt_topic_qos *topics_qos =
		    nng_mqtt_topic_qos_array_create(count);
		char * topic1 = "topic1";
		struct topic * topics;
		topics = malloc(sizeof(*topics));
		topics->next = NULL;
		topics->val  = topic1;

		size_t i = 0;
		for (struct topic *tp = topics;
		     tp != NULL && i < 1;
		     tp = tp->next, i++) {
			nng_mqtt_topic_qos_array_set(
			    topics_qos, i, tp->val, qos);
		}

		nng_mqtt_msg_set_subscribe_topics(
		    msg, topics_qos, count);
	}

	nng_mqtt_msg_encode(msg);

	int header_len = nng_msg_header_len(msg);
	int body_len   = nng_msg_len(msg);
	char * header  = nng_msg_header(msg);
	char * body    = nng_msg_body(msg);
	int msg_len    = header_len + body_len;

	printf("msg_len %d header_len %d body_len %d .\n", msg_len, header_len, body_len);
	printf("header [%x%x] boyd [%x%x%x] .\n", header[0], header[1], body[0], body[1], body[2]);

    SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + msg_len);
    if (SendBufferRaw == NULL) {
        printf("SendBuffer allocation failed!\n");
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

	memcpy(SendBufferRaw+sizeof(QUIC_BUFFER), header, header_len);
	memcpy(SendBufferRaw+sizeof(QUIC_BUFFER)+header_len, body, body_len);

    SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
    SendBuffer->Buffer = SendBufferRaw+sizeof(QUIC_BUFFER);
    SendBuffer->Length = msg_len;

    printf("[strm][%p] Sending data...\n", Stream);

    //
    // Sends the buffer over the stream. Note the FIN flag is passed along with
    // the buffer. This indicates this is the last buffer on the stream and the
    // the stream is shut down (in the send direction) immediately after.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
        free(SendBufferRaw);
        goto Error;
    }

Error:

    if (QUIC_FAILED(Status)) {
		printf("EXITTT...........\n");
        MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
	*/
}

static int
connect_cb(void * arg)
{

}

static int
recv_cb(void * arg)
{

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

	char *bufr;
	size_t szr;
	for (;;) {
		getline(&bufr, &szr, stdin);
		if (0 == strcmp(bufr, "exit")) {
			exit(0);
		}
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
