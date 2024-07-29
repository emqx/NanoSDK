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
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/nng.h>
#include <nng/supplemental/util/platform.h>
#include <nng/supplemental/tls/tls.h>

// Subcommands
#define PUBLISH       "pub"
#define SUBSCRIBE     "sub"
#define TLS_PUBLISH   "pubtls"
#define TLS_SUBSCRIBE "subtls"

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
	exit(0);
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
	printf("%s: connected! RC [%d] \n", __FUNCTION__, reason);
	(void) ev;
	(void) arg;
}

void loadfile(const char *path, void **datap, size_t *lenp)
{
    FILE * f;
    size_t total_read      = 0;
    size_t allocation_size = BUFSIZ;
    char * fdata;
    char * realloc_result;

    if ((f = fopen(path, "rb")) == NULL) {
        fprintf(stderr, "Cannot open file %s: %s", path,
                strerror(errno));
        exit(1);
    }

    if ((fdata = malloc(allocation_size + 1)) == NULL) {
        fprintf(stderr, "Out of memory.");
    }

    while (1) {
        total_read += fread(
                fdata + total_read, 1, allocation_size - total_read, f);
        if (ferror(f)) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "Read from %s failed: %s", path,
                    strerror(errno));
            exit(1);
        }
        if (feof(f)) {
            break;
        }
        if (total_read == allocation_size) {
            if (allocation_size > SIZE_MAX / 2) {
                fprintf(stderr, "Out of memory.");
            }
            allocation_size *= 2;
            if ((realloc_result = realloc(
                    fdata, allocation_size + 1)) == NULL) {
                free(fdata);
                fprintf(stderr, "Out of memory.");
                exit(1);
            }
            fdata = realloc_result;
        }
    }
    if (f != stdin) {
        fclose(f);
    }
    fdata[total_read] = '\0';
    *datap            = fdata;
    *lenp             = total_read;
}

int init_dialer_tls(nng_dialer d, const char *cacert, const char *cert,
        const char *key, const char *pass)
{
    nng_tls_config *cfg;
    int             rv;

    if ((rv = nng_tls_config_alloc(&cfg, NNG_TLS_MODE_CLIENT)) != 0) {
        return (rv);
    }

    if (cert != NULL && key != NULL) {
        nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_REQUIRED);
        if ((rv = nng_tls_config_own_cert(cfg, cert, key, pass)) !=
                0) {
            goto out;
        }
    } else {
        nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_NONE);
    }

    if (cacert != NULL) {
        if ((rv = nng_tls_config_ca_chain(cfg, cacert, NULL)) != 0) {
            goto out;
        }
    }

    rv = nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, cfg);

out:
    nng_tls_config_free(cfg);
    return (rv);
}

// Connect to the given address.
int
client_connect(nng_socket *sock, const char *url, bool verbose,
		int istls, const char *capath, const char *certpath, const char *keypath, const char *pwd)
{
	nng_dialer dialer;
	int        rv;

	// To enable SCRAM open a MQTTv5 socket
	if ((rv = nng_mqttv5_client_open(sock)) != 0) {
		fatal("nng_socket", rv);
	}

	if ((rv = nng_dialer_create(&dialer, *sock, url)) != 0) {
		fatal("nng_dialer_create", rv);
	}
	// Enable scram
	bool enable_scram = true;
	nng_dialer_set(dialer, NNG_OPT_MQTT_ENABLE_SCRAM, &enable_scram, sizeof(bool));

	// create a CONNECT message
	/* CONNECT */
	nng_msg *connmsg;
	nng_mqtt_msg_alloc(&connmsg, 0);
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	// To enable SCRAM, version must be MQTTv5
	nng_mqtt_msg_set_connect_proto_version(connmsg, 5);
	nng_mqtt_msg_set_connect_keep_alive(connmsg, 600);
	nng_mqtt_msg_set_connect_user_name(connmsg, "admin");
	nng_mqtt_msg_set_connect_password(connmsg, "public");
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

	// Init tls dialer
    if(istls)
    {
		char *ca; size_t calen;
		char *cert; size_t certlen;
		char *key; size_t keylen;
		if (capath)
			loadfile(capath, (void**)&ca, &calen);
		if (certpath)
			loadfile(certpath, (void**)&cert, &certlen);
		if (keypath)
			loadfile(keypath, (void**)&key, &keylen);
		if (capath && calen == 0) {
            printf("init_dialer_tls: CA is unavailable");
            return -1;
		}
		if (certpath && certlen == 0) {
            printf("init_dialer_tls: Cert is unavailable");
            return -1;
		}
		if (keypath && keylen == 0) {
            printf("init_dialer_tls: Key is unavailable");
            return -1;
		}

        if ((rv = init_dialer_tls(dialer, ca, cert, key, pwd)) != 0)
        {
            printf("init_dialer_tls: %s", nng_strerror(rv));
            return -1;
        }
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

	if (msg == NULL)
		return;
	switch (nng_mqtt_msg_get_packet_type(msg)) {
	case NNG_MQTT_SUBACK:
		code = nng_mqtt_msg_get_suback_return_codes(
		    msg, &count);
		printf("SUBACK reason codes are: ");
		for (int i = 0; i < (int)count; ++i)
			printf("[%d] ", code[i]);
		printf("\n");
		break;
	case NNG_MQTT_UNSUBACK:
		code = nng_mqtt_msg_get_unsuback_return_codes(
		    msg, &count);
		printf("UNSUBACK reason codes are: ");
		for (int i = 0; i < (int)count; ++i)
			printf("[%d] ", code[i]);
		printf("\n");
		break;
	case NNG_MQTT_PUBACK:
		printf("PUBACK");
		break;
	default:
		printf("Sending in async way is done.\n");
		break;
	}
	printf("Aio mqtt result %d \n", nng_aio_result(aio));
	// printf("suback %d \n", *code);
	nng_msg_free(msg);
	(void) arg;
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
	struct pub_params *params = args;
	do {
		client_publish(*params->sock, params->topic, params->data,
		    params->data_len, params->qos, params->verbose);
		nng_msleep(params->interval);
	} while (params->interval > 0);
	printf("thread_exit\n");
}

struct pub_params params;

int
main(const int argc, const char **argv)
{
	nng_socket sock;

	const char *exe = argv[0];

	const char *cmd;
	int   istls = 0;
	const char  *ca = NULL, *cert = NULL, *key = NULL, *pwd = NULL;

	if (5 == argc && 0 == strcmp(argv[1], SUBSCRIBE)) {
		cmd = SUBSCRIBE;
	} else if (6 <= argc && 0 == strcmp(argv[1], PUBLISH)) {
		cmd = PUBLISH;
	} else if (7 == argc && 0 == strcmp(argv[1], TLS_PUBLISH)) {
		cmd = TLS_PUBLISH;
		istls = 1;
		ca = argv[6];
	} else if (9 == argc && 0 == strcmp(argv[1], TLS_PUBLISH)) {
		cmd = TLS_PUBLISH;
		istls = 1;
		ca = argv[6];
		cert = argv[7];
		key = argv[8];
	} else if (10 == argc && 0 == strcmp(argv[1], TLS_PUBLISH)) {
		cmd = TLS_PUBLISH;
		istls = 1;
		ca = argv[6];
		cert = argv[7];
		key = argv[8];
		pwd = argv[9];
	} else if (6 == argc && 0 == strcmp(argv[1], TLS_SUBSCRIBE)) {
		cmd = TLS_SUBSCRIBE;
		istls = 1;
		ca = argv[5];
	} else if (8 == argc && 0 == strcmp(argv[1], TLS_SUBSCRIBE)) {
		cmd = TLS_SUBSCRIBE;
		istls = 1;
		ca = argv[5];
		cert = argv[6];
		key = argv[7];
	} else if (9 == argc && 0 == strcmp(argv[1], TLS_SUBSCRIBE)) {
		cmd = TLS_SUBSCRIBE;
		istls = 1;
		ca = argv[5];
		cert = argv[6];
		key = argv[7];
		pwd = argv[8];
	} else {
		goto error;
	}

	const char *url         = argv[2];
	uint8_t     qos         = atoi(argv[3]);
	const char *topic       = argv[4];
	int         rv          = 0;
	char *      verbose_env = getenv("VERBOSE");
	bool        verbose     = verbose_env && strlen(verbose_env) > 0;

	nng_duration retry = 10000;
	nng_socket_set_ms(sock, NNG_OPT_MQTT_RETRY_INTERVAL, retry);
	client_connect(&sock, url, verbose, istls, ca, cert, key, pwd);
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
	    "       %s %s <URL> <QOS> <TOPIC>\n"
	    "       %s %s <URL> <QOS> <TOPIC> <data> <CA>\n"
	    "       %s %s <URL> <QOS> <TOPIC> <data> <CA> <CERT> <KEY>\n"
	    "       %s %s <URL> <QOS> <TOPIC> <data> <CA> <CERT> <KEY> <KEY_PASSWORD>\n"
	    "       %s %s <URL> <QOS> <TOPIC> <CA>\n"
	    "       %s %s <URL> <QOS> <TOPIC> <CA> <CERT> <KEY>\n"
	    "       %s %s <URL> <QOS> <TOPIC> <CA> <CERT> <KEY> <KEY_PASSWORD>\n"
	    "Example: %s %s 'mqtt-tcp://127.0.0.1:1883' 1 topic\n"
	    "         %s %s 'tls+mqtt-tcp://127.0.0.1:8883' 1 topic /etc/cert/ca.pem\n",
	    exe, PUBLISH,
	    exe, SUBSCRIBE,
	    exe, TLS_PUBLISH,
	    exe, TLS_PUBLISH,
	    exe, TLS_PUBLISH,
	    exe, TLS_SUBSCRIBE,
	    exe, TLS_SUBSCRIBE,
	    exe, TLS_SUBSCRIBE,
	    exe, SUBSCRIBE,
	    exe, TLS_SUBSCRIBE
		);
	return 1;
}
