// Author: wangha <wanghamax at gmail dot com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

//
// This is just a simple MQTT client demonstration application.
//
// The application has two sub-commands: `pub` and `sub`. The `pub`
// sub-command publishes a given message to the server and then exits.
// The `sub` sub-command subscribes to the given topic filter and blocks
// waiting for incoming messages.
//
// # Example:
//
// Publish 'hello' to `topic` with QoS `0`:
// ```
// $ ./mqtt_client pub mqtt-tcp://127.0.0.1:1883 0 topic hello
// ```
//
// Subscribe to `topic` with QoS `0` and waiting for messages:
// ```
// $ ./mqtt_client sub mqtt-tcp://127.0.0.1:1883 0 topic
// ```
//

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/nng.h>
#include <nng/supplemental/util/platform.h>

// Subcommands
#define PUBLISH "pub"
#define SUBSCRIBE "sub"

void
fatal(const char *msg, int rv)
{
	fprintf(stderr, "%s: %s\n", msg, nng_strerror(rv));
}

int keepRunning = 1;
void
intHandler(int dummy)
{
	keepRunning = 0;
	fprintf(stderr, "\nclient exit(0).\n");
	exit(0);
}

// Print the given string limited to 80 columns.
//
// The `prefix` should be a null terminated string much smaller than 80,
// `str` and `len` designates the string to be printed, `quote` specifies
// whether to print in single quotes.
void
print80(const char *prefix, const char *str, size_t len, bool quote)
{
	size_t max_len = 80 - strlen(prefix) - (quote ? 2 : 0);
	char * q       = quote ? "'" : "";
	if (len <= max_len) {
		// case the output fit in a line
		printf("%s%s%.*s%s\n", prefix, q, (int) len, str, q);
	} else {
		// case we truncate the payload with ellipses
		printf(
		    "%s%s%.*s%s...\n", prefix, q, (int) (max_len - 3), str, q);
	}
}

static void
disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	int reason = 0;
	// get connect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_DISCONNECT_REASON, &reason);
	// property *prop;
	// nng_pipe_get_ptr(p, NNG_OPT_MQTT_DISCONNECT_PROPERTY, &prop);
	// nng_socket_get?
	printf("%s: disconnected! RC [%d] \n", __FUNCTION__, reason);
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
	printf("%s: connected! RC [%d] \n", __FUNCTION__, reason);
}

// Connect to the given address.
int
client_connect(nng_socket *sock, const char *url, bool verbose)
{
	nng_dialer dialer;
	int        rv;

	if ((rv = nng_mqttv5_client_open(sock)) != 0) {
		fatal("nng_socket", rv);
	}

	if ((rv = nng_dialer_create(&dialer, *sock, url)) != 0) {
		fatal("nng_dialer_create", rv);
	}

	// create a CONNECT message
	/* CONNECT */
	nng_msg *connmsg;
	nng_mqtt_msg_alloc(&connmsg, 0);
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(connmsg, 5);
	nng_mqtt_msg_set_connect_keep_alive(connmsg, 600);
	nng_mqtt_msg_set_connect_user_name(connmsg, "nng_mqtt_client");
	nng_mqtt_msg_set_connect_password(connmsg, "secrets");
	nng_mqtt_msg_set_connect_will_msg(
	    connmsg, (uint8_t *) "bye-bye", strlen("bye-bye"));
	nng_mqtt_msg_set_connect_will_topic(connmsg, "will_topic");
	nng_mqtt_msg_set_connect_clean_session(connmsg, true);

	property * p = mqtt_property_alloc();
	property *p1 = mqtt_property_set_value_u32(MAXIMUM_PACKET_SIZE, 120);
	property *p2 = mqtt_property_set_value_u16(TOPIC_ALIAS_MAXIMUM, 65535);
	mqtt_property_append(p, p1);
	mqtt_property_append(p, p2);
	nng_mqtt_msg_set_connect_property(connmsg, p);

	property *will_prop = mqtt_property_alloc();
	property *will_up   = mqtt_property_set_value_strpair(USER_PROPERTY,
            "user", strlen("user"), "pass", strlen("pass"), true);
	mqtt_property_append(will_prop, will_up);
	nng_mqtt_msg_set_connect_will_property(connmsg, will_prop);

	nng_mqtt_set_connect_cb(*sock, connect_cb, sock);
	nng_mqtt_set_disconnect_cb(*sock, disconnect_cb, connmsg);

	uint8_t buff[1024] = { 0 };

	if (verbose) {
		nng_mqtt_msg_dump(connmsg, buff, sizeof(buff), true);
		printf("%s\n", buff);
	}

	printf("Connecting to server ...\n");
	nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg);
	nng_dialer_start(dialer, NNG_FLAG_NONBLOCK);

	return (0);
}

void print_property(property *prop)
{
	if (prop == NULL) {
		return;
	}

	// printf("%d \n", prop->id);

	uint8_t type      = prop->data.p_type;
	uint8_t prop_id   = prop->id;
	switch (type) {
	case U8:
		printf(
		    "id: %d, value: %d (U8)\n", prop_id, prop->data.p_value.u8);
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

static void
send_callback(nng_mqtt_client *client, nng_msg *msg, void *arg) {
	nng_aio *        aio    = client->send_aio;
	uint32_t         count;
	uint8_t *        code;
	uint8_t          type;

	if (msg == NULL)
		return;
	switch (nng_mqtt_msg_get_packet_type(msg)) {
	case NNG_MQTT_SUBACK:
		code = (reason_code *) nng_mqtt_msg_get_suback_return_codes(
		    msg, &count);
		printf("SUBACK reason codes are");
		for (int i = 0; i < count; ++i)
			printf("%d ", code[i]);
		printf("\n");
		break;
	case NNG_MQTT_UNSUBACK:
		code = (reason_code *) nng_mqtt_msg_get_unsuback_return_codes(
		    msg, &count);
		printf("UNSUBACK reason codes are");
		for (int i = 0; i < count; ++i)
			printf("%d ", code[i]);
		printf("\n");
		break;
	case NNG_MQTT_PUBACK:
		printf("PUBACK");
		break;
	default:
		printf("Sending in async way is done.\n");
		break;
	}
	printf("aio mqtt result %d \n", nng_aio_result(aio));
	// printf("suback %d \n", *code);
	nng_msg_free(msg);
}

// Publish a message to the given topic and with the given QoS.
int
client_publish(nng_socket sock, const char *topic, uint8_t *payload,
    uint32_t payload_len, uint8_t qos, bool verbose)
{
	int rv;

	// create a PUBLISH message
	nng_msg *pubmsg;
	nng_mqtt_msg_alloc(&pubmsg, 0);
	nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_dup(pubmsg, 0);
	nng_mqtt_msg_set_publish_qos(pubmsg, qos);
	nng_mqtt_msg_set_publish_retain(pubmsg, 0);
	nng_mqtt_msg_set_publish_payload(
	    pubmsg, (uint8_t *) payload, payload_len);
	nng_mqtt_msg_set_publish_topic(pubmsg, topic);

	property *plist = mqtt_property_alloc();
	property *p1 = mqtt_property_set_value_u8(PAYLOAD_FORMAT_INDICATOR, 1);
	mqtt_property_append(plist, p1);
	property *p2 = mqtt_property_set_value_u16(TOPIC_ALIAS, 10);
	mqtt_property_append(plist, p2);
	property *p3 = mqtt_property_set_value_u32(MESSAGE_EXPIRY_INTERVAL, 10);
	mqtt_property_append(plist, p3);
	property *p4 = mqtt_property_set_value_str(RESPONSE_TOPIC, "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p4);
	property *p5 = mqtt_property_set_value_binary(
	    CORRELATION_DATA, (uint8_t *) "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p5);
	property *p6 = mqtt_property_set_value_strpair(USER_PROPERTY, "aaaaaa", strlen("aaaaaa"), "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p6);
	property *p7 = mqtt_property_set_value_str(CONTENT_TYPE, "aaaaaa", strlen("aaaaaa"), true);
	mqtt_property_append(plist, p7);

	nng_mqtt_msg_set_publish_property(pubmsg, plist);

	if (verbose) {
		uint8_t print[1024] = { 0 };
		nng_mqtt_msg_dump(pubmsg, print, 1024, true);
		printf("%s\n", print);
	}

	property *pl = nng_mqtt_msg_get_publish_property(pubmsg);
	if (pl != NULL) {
		mqtt_property_foreach(pl, print_property);
	}

	printf("Publishing to '%s' ...\n", topic);
	if ((rv = nng_sendmsg(sock, pubmsg, NNG_FLAG_NONBLOCK)) != 0) {
		fatal("nng_sendmsg", rv);
	}

	return rv;
}

struct pub_params {
	nng_socket *sock;
	const char *topic;
	uint8_t *   data;
	uint32_t    data_len;
	uint8_t     qos;
	bool        verbose;
	uint32_t    interval;
};

void msg_recv_deal(nng_msg *msg, bool verbose)
{
		uint32_t topic_len = 0;
		uint32_t payload_len = 0;
		const char *topic = nng_mqtt_msg_get_publish_topic(msg, &topic_len);
	        char *      payload =
	            (char *) nng_mqtt_msg_get_publish_payload(
	                msg, &payload_len);

	        printf("Receive \'%.*s\' from \'%.*s\'\n", payload_len, payload, topic_len, topic);
		property *pl = nng_mqtt_msg_get_publish_property(msg);
		if (pl != NULL) {
			mqtt_property_foreach(pl, print_property);
		}

		if (verbose) {
			uint8_t buff[1024] = { 0 };
			memset(buff, 0, sizeof(buff));
			nng_mqtt_msg_dump(msg, buff, sizeof(buff), true);
			printf("%s\n", buff);
		}

		nng_msg_free(msg);
}

void
publish_cb(void *args)
{
	int                rv;
	struct pub_params *params = args;
	do {
		client_publish(*params->sock, params->topic, params->data,
		    params->data_len, params->qos, params->verbose);
		nng_msleep(params->interval);
	} while (params->interval > 0);
	printf("thread_exit\n");
}

struct pub_params params;

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
	nng_mqtt_sqlite_db_init(sqlite, "mqttv5_client.db", proto_ver);

	// set sqlite option pointer to socket
	return nng_socket_set_ptr(*sock, NNG_OPT_MQTT_SQLITE, sqlite);
#else
	return (0);
#endif
}

int
main(const int argc, const char **argv)
{
	nng_socket sock;

	const char *exe = argv[0];

	const char *cmd;

	if (5 == argc && 0 == strcmp(argv[1], SUBSCRIBE)) {
		cmd = SUBSCRIBE;
	} else if (6 <= argc && 0 == strcmp(argv[1], PUBLISH)) {
		cmd = PUBLISH;
	} else {
		goto error;
	}

	const char *url         = argv[2];
	uint8_t     qos         = atoi(argv[3]);
	const char *topic       = argv[4];
	int         rv          = 0;
	char *      verbose_env = getenv("VERBOSE");
	bool        verbose     = verbose_env && strlen(verbose_env) > 0;

	client_connect(&sock, url, verbose);
	nng_msleep(1000);

	signal(SIGINT, intHandler);

	if (strcmp(PUBLISH, cmd) == 0) {
		const char *data     = argv[5];
		uint32_t    interval = 0;
		uint32_t    nthread  = 1;

		if (argc >= 7) {
			interval = atoi(argv[6]);
		}
		if (argc >= 8) {
			nthread = atoi(argv[7]);
		}
		nng_thread *threads[nthread];

		params.sock = &sock, params.topic = topic;
		params.data     = (uint8_t *) data;
		params.data_len = strlen(data);
		params.qos      = qos;
		params.interval = interval;
		params.verbose  = verbose;

		char thread_name[20];

		sqlite_config(params.sock, MQTT_PROTOCOL_VERSION_v5);

		size_t i = 0;
		for (i = 0; i < nthread; i++) {
			nng_thread_create(&threads[i], publish_cb, &params);
		}

		for (i = 0; i < nthread; i++) {
			nng_thread_destroy(threads[i]);
		}
	} else if (strcmp(SUBSCRIBE, cmd) == 0) {
		nng_mqtt_topic_qos subscriptions[] = {
			{
			    .qos   = qos,
			    .topic = { 
					.buf    = (uint8_t *) topic,
			        .length = strlen(topic), 
				},
				.nolocal         = 1,
				.rap             = 1,
				.retain_handling = 0,
			},
		};
		nng_mqtt_topic unsubscriptions[] = {
			{
			    .buf    = (uint8_t *) topic,
			    .length = strlen(topic),
			},
		};

		property *plist = mqtt_property_alloc();
		mqtt_property_append(plist,
		    mqtt_property_set_value_varint(
		        SUBSCRIPTION_IDENTIFIER, 120));
		property *unsub_plist = NULL;
		mqtt_property_dup(&unsub_plist, plist);

		// Sync subscription
		// rv = nng_mqtt_subscribe(sock, subscriptions, 1, plist);

		// Asynchronous subscription
		nng_mqtt_client *client = nng_mqtt_client_alloc(sock, &send_callback, NULL, true);
		nng_mqtt_subscribe_async(client, subscriptions,
		    sizeof(subscriptions) / sizeof(nng_mqtt_topic_qos), plist);

		printf("Start receiving loop:\n");
		while (true) {
			nng_msg *msg;
			if ((rv = nng_recvmsg(sock, &msg, 0)) != 0) {
				fatal("nng_recvmsg", rv);
				continue;
			}

			// we should only receive publish messages
			assert(nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH);
			msg_recv_deal(msg, verbose);
		}

		// Sync unsubscription
		// rv = nng_mqtt_unsubscribe(sock, subscriptions, 1, plist);
		// Asynchronous unsubscription
		nng_mqtt_unsubscribe_async(client, unsubscriptions,
		    sizeof(unsubscriptions) / sizeof(nng_mqtt_topic),
		    unsub_plist);
		nng_mqtt_client_free(client, true);
	}

	// disconnect 
	property *plist = mqtt_property_alloc();
	property *p     = mqtt_property_set_value_strpair(
            USER_PROPERTY, "aaa", strlen("aaa"), "aaa", strlen("aaa"), true);
	mqtt_property_append(plist, p);
	nng_mqtt_disconnect(&sock, 5, plist);
	return 0;

error:
	fprintf(stderr,
	    "Usage: %s %s <URL> <QOS> <TOPIC> <data> <interval> <parallel>\n"
	    "       %s %s <URL> <QOS> <TOPIC>\n",
	    exe, PUBLISH, exe, SUBSCRIBE);
	return 1;
}
