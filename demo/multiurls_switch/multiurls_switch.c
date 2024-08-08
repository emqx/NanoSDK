// Author: wangha <wangwei at emqx dot io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

//
// This is just a simple MQTT client demonstration application to
// show how to switch to different url when connect to current url failed.
//

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "nng/mqtt/mqtt_client.h"
#include "nng/nng.h"
#include "nng/supplemental/util/platform.h"

#define ROUND_ROBIN true

void
print_helper()
{
	printf("Introduce:\n");
	printf("It's a simple MQTT client to demonstrate how to\n");
	printf("switch to a different url when connect to current\n");
	printf("url failed.\n\n");
};

void
fatal(const char *msg, int rv)
{
	fprintf(stderr, "%s: %s\n", msg, nng_strerror(rv));
}

int keepRunning = 1;
void
intHandler(int dummy)
{
	(void) dummy;
	keepRunning = 0;
	fprintf(stderr, "\nclient exit(0).\n");
	// nng_closeall();
	exit(0);
}

static nng_cv  *switch_cv;
static nng_mtx *switch_mtx;

static void
disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	int reason = 0;
	// get connect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_DISCONNECT_REASON, &reason);
	// property *prop;
	// nng_pipe_get_ptr(p, NNG_OPT_MQTT_DISCONNECT_PROPERTY, &prop);
	// nng_socket_get?
	printf("%s: disconnected!\n", __FUNCTION__);

	// Wake to reconnect
	nng_mtx_lock(switch_mtx);
	nng_cv_wake(switch_cv);
	nng_mtx_unlock(switch_mtx);
	(void) ev;
	(void) arg;
}

static void
connect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	int reason;
	// get connect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_CONNECT_REASON, &reason);
	// get property for MQTT V5
	// property *prop;
	// nng_pipe_get_ptr(p, NNG_OPT_MQTT_CONNECT_PROPERTY, &prop);
	printf("%s: connected!\n", __FUNCTION__);
	(void) ev;
	(void) arg;
}

int
client_connect(const char **urls, int len)
{
	int        rv;

	// create a CONNECT message
	nng_msg *connmsg;
	nng_mqtt_msg_alloc(&connmsg, 0);
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(connmsg, 4);
	nng_mqtt_msg_set_connect_keep_alive(connmsg, 60);
	nng_mqtt_msg_set_connect_user_name(connmsg, "nng_mqtt_client");
	nng_mqtt_msg_set_connect_password(connmsg, "secrets");
	nng_mqtt_msg_set_connect_will_msg(
	    connmsg, (uint8_t *) "bye-bye", strlen("bye-bye"));
	nng_mqtt_msg_set_connect_will_topic(connmsg, "will_topic");
	nng_mqtt_msg_set_connect_clean_session(connmsg, true);

	uint8_t buff[1024] = { 0 };
	nng_mqtt_msg_dump(connmsg, buff, sizeof(buff), true);
	//printf("%s\n", buff);

	int cnt = -1;

	while (1) {
		nng_socket sock;
		if ((rv = nng_mqtt_client_open(&sock)) != 0) {
			fatal("nng_socket", rv);
		}
		nng_mqtt_set_connect_cb(sock, connect_cb, (void *)&sock);
		nng_mqtt_set_disconnect_cb(sock, disconnect_cb, connmsg);

		cnt = (cnt + 1) % len;
		const char *url = urls[cnt];
	
		nng_dialer dialer;
		if ((rv = nng_dialer_create(&dialer, sock, url)) != 0) {
			fatal("nng_dialer_create", rv);
		}
		nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg);

		printf("Connecting to server [%d]%s ...\n", cnt, url);
		if ((rv = nng_dialer_start(dialer, NNG_FLAG_ALLOC)) != 0) {
			printf("Failed to connect to %s rv%d\n", url, rv);
			continue;
		}
		// connected
		// Wait for disconnect
		nng_mtx_lock(switch_mtx);
		nng_cv_wait(switch_cv);
		nng_mtx_unlock(switch_mtx);
		// close socket
		nng_close(sock);
		if (ROUND_ROBIN == false) {
			cnt = -1; // Always from the first url
		}
	}

	return (0);
}

int
main()
{
	int rv;
	print_helper();
	const char *urls[] = {
		"mqtt-tcp://example.io:1883",
		"mqtt-tcp://127.0.0.1:1883",
		"mqtt-tcp://broker.emqx.io:1883",
	};
	int len = sizeof(urls) / sizeof(char *);

	if ((0 != (rv = nng_mtx_alloc(&switch_mtx))) ||
	    (0 != (rv = nng_cv_alloc(&switch_cv, switch_mtx)))) {
		fatal("Failed to init switch mtx or cv", rv);
	}
	client_connect(urls, len);

	signal(SIGINT, intHandler);

	while (keepRunning) {
		nng_msleep(1000);
	}

	return 0;
}
