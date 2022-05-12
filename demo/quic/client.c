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
#include <stdio.h>
#include <stdlib.h>

void
fatal(int rv)
{
	fprintf(stderr, "%s\n", nng_strerror(rv));
	exit(1);
}

static int
connect_cb(void * arg)
{

	if ((rv = nng_msg_alloc(&msg, 0)) != 0) {
		fatal("nng_msg_alloc", rv);
	}

	if ((rv = nng_msg_append_u32(msg, msec)) != 0) {
		fatal("nng_msg_append_u32", rv);
	}

	if ((rv = nng_sendmsg(sock, msg, 0)) != 0) {
		fatal("nng_send", rv);
	}

	if ((rv = nng_recvmsg(sock, &msg, 0)) != 0) {
		fatal("nng_recvmsg", rv);
	}
	nng_msg_free(msg);

}

static int
recv_cb(void * arg)
{

	if ((rv = nng_msg_alloc(&msg, 0)) != 0) {
		fatal("nng_msg_alloc", rv);
	}

	if ((rv = nng_msg_append_u32(msg, msec)) != 0) {
		fatal("nng_msg_append_u32", rv);
	}

	if ((rv = nng_sendmsg(sock, msg, 0)) != 0) {
		fatal("nng_send", rv);
	}

	if ((rv = nng_recvmsg(sock, &msg, 0)) != 0) {
		fatal("nng_recvmsg", rv);
	}
	nng_msg_free(msg);

}

int
client(const char *type, const char *url)
{
	nng_socket sock;
	int        rv;
	nng_msg *  msg;

	if ((rv = quic_open()) != 0) {
		fatal("quic_open", rv);
	}

	if ((rv = quic_connect(sock, url, NULL, 0)) != 0) {
		fatal("quic_connect", rv);
	}

	if ((rv = quic_mqtt_set_connect_cb(connect_cb, NULL)) != 0) {
		fatal("set_connect_cb", rv);
	}

	if ((rv = quic_mqtt_set_recv_cb(recv_cb, NULL)) != 0) {
		fatal("set_recv_cb", rv);
	}

	if ((rv = quic_mqtt_connect(sock, url) != 0) {
		fatal("mqtt_connect", rv);
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
