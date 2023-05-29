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

#include <stdio.h>
#include <stdlib.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/mqtt/mqtt_quic.h>
#include <nng/nng.h>
#include <nng/supplemental/util/platform.h>

#include <msquic.h>

#include "property_type_map.h"

static nng_socket theSocket;

static conf_quic config_user = {
	.tls = {
		.enable = false,
		.cafile = "",
		.certfile = "",
		.keyfile = "",
		.key_password = "",
		.verify_peer = true,
		.set_fail = true,
	},
	.multi_stream = false,
	.qos_first = false,
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

void print_property(property *prop)
{
	if (prop == NULL)
		return;

	else
	{
		const uint8_t type = prop->data.p_type;
		const uint8_t prop_id = prop->id;
		const char *const propString = array_of_type_strings[prop_id];

		switch (type)
		{
		case U8:
			printf("%s, value: %d (U8)\n", propString, prop->data.p_value.u8);
			break;
		case U16:
			printf("%s, value: %d (U16)\n", propString, prop->data.p_value.u16);
			break;
		case U32:
			printf("%s, value: %u (U32)\n", propString, prop->data.p_value.u32);
			break;
		case VARINT:
			printf("%s, value: %d (VARINT)\n", propString, prop->data.p_value.varint);
			break;
		case BINARY:
			printf("%s, value pointer: %p (BINARY)\n", propString, prop->data.p_value.binary.buf);
			break;
		case STR:
			printf("%s, value: %.*s (STR)\n", propString, prop->data.p_value.str.length,
				   (const char *)prop->data.p_value.str.buf);
			break;
		case STR_PAIR:
			printf("%s, value: '%.*s -> %.*s' (STR_PAIR)\n", propString,
				   prop->data.p_value.strpair.key.length,
				   prop->data.p_value.strpair.key.buf,
				   prop->data.p_value.strpair.value.length,
				   prop->data.p_value.strpair.value.buf);
			break;

		default:
			fprintf(stderr, "UNKNOWN PROPERTY TYPE\n");
		}

		printf("---------------------------------------------------\n");
		fflush(stdout);
	}
}

static nng_msg *
compose_connect()
{
	nng_msg *connectionMessage;
	property *p1 = mqtt_property_set_value_u32(MAXIMUM_PACKET_SIZE, 120);
	property *p2 = mqtt_property_set_value_u32(SESSION_EXPIRY_INTERVAL, 120);
	property *p3 = mqtt_property_set_value_u16(RECEIVE_MAXIMUM, 120);
	property *p4 = mqtt_property_set_value_u32(MAXIMUM_PACKET_SIZE, 500000);
	property *p5 = mqtt_property_set_value_u16(TOPIC_ALIAS_MAXIMUM, 120);

	property *plist = mqtt_property_alloc();
	mqtt_property_append(plist, p1);
	mqtt_property_append(plist, p2);
	mqtt_property_append(plist, p3);
	mqtt_property_append(plist, p4);
	mqtt_property_append(plist, p5);

	nng_mqtt_msg_alloc(&connectionMessage, 0);
	//- define connection.
	nng_mqtt_msg_set_packet_type(connectionMessage, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_property(connectionMessage, plist);
	nng_mqtt_msg_set_connect_proto_version(connectionMessage, MQTT_PROTOCOL_VERSION_v5);
	nng_mqtt_msg_set_connect_keep_alive(connectionMessage, 30);
	nng_mqtt_msg_set_connect_clean_session(connectionMessage, true);

	return connectionMessage;
}

static nng_msg *
compose_subscribe(int qos, char *topic)
{
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

	nng_mqtt_topic_qos subscriptions[] = {
		{.qos = qos,
		 .topic = {.buf = (uint8_t *)topic,
				   .length = (1 + strlen(topic))}},
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
compose_publish(const uint8_t qos, char *topic, char *theCorrelation, char *thePayload)
{
	nng_msg *msg;
	property *plist = mqtt_property_alloc();
	property *p1 = mqtt_property_set_value_u8(PAYLOAD_FORMAT_INDICATOR, 1);
	property *p2 = mqtt_property_set_value_u16(TOPIC_ALIAS, 10);
	property *p3 = mqtt_property_set_value_u32(MESSAGE_EXPIRY_INTERVAL, 10);
	property *p4 = mqtt_property_set_value_str(RESPONSE_TOPIC, "response_topic", strlen("response_topic"), true);
	property *p5 = mqtt_property_set_value_binary(CORRELATION_DATA, (uint8_t *)theCorrelation, (uint32_t)strlen(theCorrelation), true);
	property *p6 = mqtt_property_set_value_strpair(USER_PROPERTY, "key1", strlen("key1"), "value1", strlen("value1"), true);
	property *p7 = mqtt_property_set_value_str(CONTENT_TYPE, "content_type", strlen("content_type"), true);

	mqtt_property_append(plist, p1);
	mqtt_property_append(plist, p2);
	mqtt_property_append(plist, p3);
	mqtt_property_append(plist, p4);
	mqtt_property_append(plist, p5);
	mqtt_property_append(plist, p6);
	mqtt_property_append(plist, p7);

	nng_mqtt_msg_alloc(&msg, 0);
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_property(msg, plist);
	nng_mqtt_msg_set_publish_dup(msg, 0);
	nng_mqtt_msg_set_publish_qos(msg, qos);
	nng_mqtt_msg_set_publish_retain(msg, 0);
	nng_mqtt_msg_set_publish_topic(msg, topic);
	nng_mqtt_msg_set_publish_payload(msg, (uint8_t *)thePayload, (1 + strlen(thePayload)));

	return msg;
}

static int
connect_cb(void *rmsg, void *arg)
{
	printf("[Connected][%s]...\n", (char *)arg);
	return 0;
}

static int
disconnect_cb(void *rmsg, void *arg)
{
	printf("[Disconnected][%s]...\n", (char *)arg);
	fflush(stdout);
	return 0;
}

static int
msg_send_cb(void *rmsg, void *arg)
{
	printf("[Msg Sent][%s]...\n", (char *)arg);
	fflush(stdout);
	return 0;
}

static int
msg_recv_cb(void *rmsg, void *arg)
{
	nng_msg *msg = rmsg;
	uint32_t topicsz, payloadsz;

	char *topic = (char *)nng_mqtt_msg_get_publish_topic(msg, &topicsz);
	char *payload =
		(char *)nng_mqtt_msg_get_publish_payload(msg, &payloadsz);

	printf("[Msg Arrived][%s]...\n", (char *)arg);
	printf("topic   => %.*s\n"
		   "payload => %.*s\n",
		   topicsz, topic, payloadsz, payload);

	property *pl = nng_mqtt_msg_get_publish_property(msg);

	if (pl != NULL)
		mqtt_property_foreach(pl, print_property);

	fflush(stdout);
	return 0;
}

void *setUpClient(const char *theURL)
{
	const char *arg = ">>>>>>>>>>>>>> AIL-SV client for QUIC";
	nng_msg *setupMessage;

	if (nng_mqttv5_quic_client_open_conf(&theSocket, theURL, &config_user) != 0)
	{
		printf("error in quic client open.\n");
		return NULL;
	}

	if (0 !=
			nng_mqtt_quic_set_connect_cb(
				&theSocket, connect_cb, (void *)arg) ||
		0 !=
			nng_mqtt_quic_set_disconnect_cb(
				&theSocket, disconnect_cb, (void *)arg) ||
		0 !=
			nng_mqtt_quic_set_msg_recv_cb(
				&theSocket, msg_recv_cb, (void *)arg) ||
		0 !=
			nng_mqtt_quic_set_msg_send_cb(
				&theSocket, msg_send_cb, (void *)arg))
	{
		printf("error in quic client cb set.\n");
	}

	// MQTT Connect...
	setupMessage = compose_connect();
	uint8_t buff[1024] = {0};
	nng_mqtt_msg_dump(setupMessage, buff, sizeof(buff), true);
	printf("%s\n", buff);

	property *pl = nng_mqtt_msg_get_connect_property(setupMessage);
	if (pl != NULL)
	{
		mqtt_property_foreach(pl, print_property);
	}
	nng_sendmsg(theSocket, setupMessage, NNG_FLAG_ALLOC);


	return (&theSocket);
}

void publishMessage(nng_socket *opaquePointer, char *theTopic, char *theCorrelation, char *theData)
{
	nng_socket *pointerToSocket = (nng_socket *)opaquePointer;
	const uint8_t theQoS = 0;
	nng_msg *messageToPublish = compose_publish(theQoS, theTopic, theCorrelation, theData);

	property *plist = nng_mqtt_msg_get_publish_property(messageToPublish);
	if (plist != NULL)
		mqtt_property_foreach(plist, print_property);
	nng_sendmsg(*pointerToSocket, messageToPublish, NNG_FLAG_ALLOC);
}

void shutdownClient(void *opaquePointer)
{
	nng_socket *pointerToSocket = (nng_socket *)opaquePointer;

	nng_close(*pointerToSocket);
}

/**
int
main(int argc, char **argv)
{
   char *theURL = "mqtt-quic://54.183.144.230:14567";
   char *theTopic = "Boc3HUvYZn";

   nng_socket *pointerToSocket = setUpClient(theURL);

   publishMessage(pointerToSocket, theTopic, sampleJsonInOneLine);

   shutdownClient(pointerToSocket);

   void *pointerToSocket = setUpClient(theURL);

   publishMessage(pointerToSocket, theTopic, sampleJsonInOneLine);

   shutdownClient(pointerToSocket);
}

*/
