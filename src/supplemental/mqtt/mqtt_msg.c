#include "mqtt_msg.h"
#include <stdlib.h>
#include <string.h>

static nni_proto_msg_ops proto_msg_ops = {

	.msg_free = nni_mqtt_msg_free,

	.msg_dup = nni_mqtt_msg_dup
};

// set default conn param in case User ignores it.
void
nni_proto_data_init(nni_mqtt_proto_data *proto_data)
{
	proto_data->var_header.connect.conn_flags.clean_session = true;
	proto_data->var_header.connect.keep_alive = 30;
	proto_data->var_header.connack.properties = NULL;
	proto_data->var_header.connect.properties = NULL;
	proto_data->var_header.disconnect.properties = NULL;
	proto_data->var_header.puback.properties = NULL;
	proto_data->var_header.pubcomp.properties = NULL;
	proto_data->var_header.publish.properties = NULL;
	proto_data->var_header.pubrec.properties = NULL;
	proto_data->var_header.pubrel.properties = NULL;
	proto_data->var_header.suback.properties = NULL;
	proto_data->var_header.subscribe.properties = NULL;
	proto_data->var_header.unsuback.properties = NULL;
	proto_data->var_header.unsubscribe.properties = NULL;
}

int
nni_mqtt_msg_proto_data_alloc(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data;

	if ((proto_data = NNI_ALLOC_STRUCT(proto_data)) == NULL) {
		return NNG_ENOMEM;
	}
	nni_proto_data_init(proto_data);
	proto_data->initialized = false;
	nni_msg_set_proto_data(msg, &proto_msg_ops, proto_data);

	return 0;
}

void
nni_mqtt_msg_proto_data_free(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	nni_mqtt_msg_free(proto_data);
}

int
nni_mqtt_msg_alloc(nni_msg **msg, size_t sz)
{
	int rv;

	if ((rv = nni_msg_alloc(msg, sz)) != 0) {
		return rv;
	}

	if ((rv = nni_mqtt_msg_proto_data_alloc(*msg)) != 0) {
		return rv;
	}

	return (0);
}

void
nni_mqtt_msg_set_packet_type(nni_msg *msg, nni_mqtt_packet_type packet_type)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	proto_data->fixed_header.common.packet_type = packet_type;
}

nni_mqtt_packet_type
nni_mqtt_msg_get_packet_type(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	return proto_data->fixed_header.common.packet_type;
}

void
nni_mqtt_msg_set_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	switch (proto_data->fixed_header.common.packet_type) {
	case NNG_MQTT_PUBACK:
		proto_data->var_header.puback.packet_id = packet_id;
		break;
	case NNG_MQTT_PUBCOMP:
		proto_data->var_header.pubcomp.packet_id = packet_id;
		break;
	case NNG_MQTT_PUBREC:
		proto_data->var_header.pubrec.packet_id = packet_id;
		break;
	case NNG_MQTT_PUBREL:
		proto_data->var_header.pubrel.packet_id = packet_id;
		break;
	case NNG_MQTT_PUBLISH:
		proto_data->var_header.publish.packet_id = packet_id;
		break;
	case NNG_MQTT_SUBACK:
		proto_data->var_header.suback.packet_id = packet_id;
		break;
	case NNG_MQTT_SUBSCRIBE:
		proto_data->var_header.subscribe.packet_id = packet_id;
		break;
	case NNG_MQTT_UNSUBACK:
		proto_data->var_header.unsuback.packet_id = packet_id;
		break;
	case NNG_MQTT_UNSUBSCRIBE:
		proto_data->var_header.unsubscribe.packet_id = packet_id;
		break;
	default:
		// logic error
		NNI_ASSERT(false);
	}
}

uint16_t
nni_mqtt_msg_get_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	switch (proto_data->fixed_header.common.packet_type) {
	case NNG_MQTT_PUBACK:
		return proto_data->var_header.puback.packet_id;
	case NNG_MQTT_PUBCOMP:
		return proto_data->var_header.pubcomp.packet_id;
	case NNG_MQTT_PUBREC:
		return proto_data->var_header.pubrec.packet_id;
	case NNG_MQTT_PUBREL:
		return proto_data->var_header.pubrel.packet_id;
	case NNG_MQTT_PUBLISH:
		return proto_data->var_header.publish.packet_id;
	case NNG_MQTT_SUBACK:
		return proto_data->var_header.suback.packet_id;
	case NNG_MQTT_SUBSCRIBE:
		return proto_data->var_header.subscribe.packet_id;
	case NNG_MQTT_UNSUBACK:
		return proto_data->var_header.unsuback.packet_id;
	case NNG_MQTT_UNSUBSCRIBE:
		return proto_data->var_header.unsubscribe.packet_id;
	default:
		// logic error
		NNI_ASSERT(false);
	}
	return 0;
}

property *
nni_mqtt_msg_get_publish_property(nng_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	return mqtt->var_header.publish.properties;
}

void
nni_mqtt_msg_set_publish_property(nng_msg *msg, property *prop)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	mqtt->var_header.publish.properties = prop;
}

void
nni_mqtt_msg_set_publish_qos(nni_msg *msg, uint8_t qos)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	proto_data->fixed_header.publish.qos = qos;
}

uint8_t
nni_mqtt_msg_get_publish_qos(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	return proto_data->fixed_header.publish.qos;
}

void
nni_mqtt_msg_set_publish_retain(nni_msg *msg, bool retain)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->fixed_header.publish.retain = (uint8_t) retain;
}

bool
nni_mqtt_msg_get_publish_retain(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->fixed_header.publish.retain;
}

void
nni_mqtt_msg_set_publish_dup(nni_msg *msg, bool dup)
{
	nni_mqtt_proto_data *proto_data      = nni_msg_get_proto_data(msg);
	proto_data->fixed_header.publish.dup = (uint8_t) dup;
}

bool
nni_mqtt_msg_get_publish_dup(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->fixed_header.publish.dup;
}

int
nni_mqtt_msg_set_publish_topic(nni_msg *msg, const char *topic)
{
	int rv;
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	rv = mqtt_buf_create(&proto_data->var_header.publish.topic_name,
	    (uint8_t *) topic, (uint32_t) strlen(topic));
	proto_data->is_copied = true;
	return rv;
}

const char *
nni_mqtt_msg_get_publish_topic(nni_msg *msg, uint32_t *topic_len)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*topic_len = proto_data->var_header.publish.topic_name.length;
	return (const char *) proto_data->var_header.publish.topic_name.buf;
}

void
nni_mqtt_msg_set_publish_payload(nni_msg *msg, uint8_t *payload, uint32_t len)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	mqtt_buf_create(
	    &proto_data->payload.publish.payload, payload, (uint32_t) len);
}

uint8_t *
nni_mqtt_msg_get_publish_payload(nni_msg *msg, uint32_t *outlen)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*outlen = proto_data->payload.publish.payload.length;
	return proto_data->payload.publish.payload.buf;
}

void
nni_mqtt_msg_set_publish_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data          = nni_msg_get_proto_data(msg);
	proto_data->var_header.publish.packet_id = packet_id;
}

uint16_t
nni_mqtt_msg_get_publish_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.publish.packet_id;
}

void
nni_mqtt_msg_set_puback_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->var_header.puback.packet_id = packet_id;
}

uint16_t
nni_mqtt_msg_get_puback_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.puback.packet_id;
}

property *
nni_mqtt_msg_get_puback_property(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	return mqtt->var_header.puback.properties;
}

void
nni_mqtt_msg_set_puback_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);
	mqtt->var_header.puback.properties = prop;
}

uint16_t
nni_mqtt_msg_get_pubrec_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.pubrec.packet_id;
}

void
nni_mqtt_msg_set_pubrec_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->var_header.pubrec.packet_id = packet_id;
}

property *
nni_mqtt_msg_get_pubrec_property(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	return mqtt->var_header.pubrec.properties;
}

void
nni_mqtt_msg_set_pubrec_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);
	mqtt->var_header.pubrec.properties = prop;
}

uint16_t
nni_mqtt_msg_get_pubrel_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.pubrel.packet_id;
}

void
nni_mqtt_msg_set_pubrel_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->var_header.pubrel.packet_id = packet_id;
}

property *
nni_mqtt_msg_get_pubrel_property(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	return mqtt->var_header.pubrel.properties;
}

void
nni_mqtt_msg_set_pubrel_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);
	mqtt->var_header.pubrel.properties = prop;
}

uint16_t
nni_mqtt_msg_get_pubcomp_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.pubcomp.packet_id;
}

void
nni_mqtt_msg_set_pubcomp_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data          = nni_msg_get_proto_data(msg);
	proto_data->var_header.pubcomp.packet_id = packet_id;
}

property *
nni_mqtt_msg_get_pubcomp_property(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	return mqtt->var_header.pubcomp.properties;
}

void
nni_mqtt_msg_set_pubcomp_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *mqtt           = nni_msg_get_proto_data(msg);
	mqtt->var_header.pubcomp.properties = prop;
}

uint16_t
nni_mqtt_msg_get_subscribe_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.subscribe.packet_id;
}

void
nni_mqtt_msg_set_subscribe_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.subscribe.packet_id = packet_id;
}

void
nni_mqtt_msg_set_subscribe_topics(
    nni_msg *msg, nni_mqtt_topic_qos *topics, uint32_t topic_count)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	proto_data->payload.subscribe.topic_arr =
	    nni_mqtt_topic_qos_array_create(topic_count);
	proto_data->payload.subscribe.topic_count = topic_count;

	for (size_t i = 0; i < topic_count; i++) {
		nni_mqtt_topic_qos_array_set(
		    proto_data->payload.subscribe.topic_arr, i,
		    (const char *) topics[i].topic.buf, topics[i].topic.length, topics[i].qos,
			topics[i].nolocal, topics[i].rap, topics[i].retain_handling);
	}
}

nni_mqtt_topic_qos *
nni_mqtt_msg_get_subscribe_topics(nni_msg *msg, uint32_t *topic_count)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*topic_count = proto_data->payload.subscribe.topic_count;
	return proto_data->payload.subscribe.topic_arr;
}

property *
nni_mqtt_msg_get_subscribe_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.subscribe.properties;
}

void
nni_mqtt_msg_set_subscribe_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.subscribe.properties = prop;
}

uint16_t
nni_mqtt_msg_get_suback_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.suback.packet_id;
}

void
nni_mqtt_msg_set_suback_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->var_header.suback.packet_id = packet_id;
}

void
nni_mqtt_msg_set_suback_return_codes(
    nni_msg *msg, uint8_t *ret_codes, uint32_t ret_codes_count)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->payload.suback.ret_code_arr = nni_alloc(ret_codes_count);
	memcpy(proto_data->payload.suback.ret_code_arr, ret_codes,
	    ret_codes_count);
	proto_data->payload.suback.ret_code_count = ret_codes_count;
}

uint8_t *
nni_mqtt_msg_get_suback_return_codes(nni_msg *msg, uint32_t *ret_codes_count)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*ret_codes_count = proto_data->payload.suback.ret_code_count;
	return proto_data->payload.suback.ret_code_arr;
}

property *
nni_mqtt_msg_get_suback_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.suback.properties;
}

void
nni_mqtt_msg_set_suback_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.suback.properties = prop;
}

uint16_t
nni_mqtt_msg_get_unsubscribe_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.unsubscribe.packet_id;
}

void
nni_mqtt_msg_set_unsubscribe_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.unsubscribe.packet_id = packet_id;
}

void
nni_mqtt_msg_set_unsubscribe_topics(
    nni_msg *msg, nni_mqtt_topic *topics, uint32_t topic_count)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	proto_data->payload.unsubscribe.topic_arr =
	    nni_mqtt_topic_array_create(topic_count);
	proto_data->payload.unsubscribe.topic_count = topic_count;

	for (size_t i = 0; i < topic_count; i++) {
		nni_mqtt_topic_array_set(
		    proto_data->payload.unsubscribe.topic_arr, i,
		    (const char *) topics[i].buf);
	}
}

nni_mqtt_topic *
nni_mqtt_msg_get_unsubscribe_topics(nni_msg *msg, uint32_t *topic_count)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*topic_count = proto_data->payload.unsubscribe.topic_count;
	return (nni_mqtt_topic *) proto_data->payload.unsubscribe.topic_arr;
}

property *
nni_mqtt_msg_get_unsubscribe_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.unsubscribe.properties;
}

void
nni_mqtt_msg_set_unsubscribe_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.unsubscribe.properties = prop;
}

void
nni_mqtt_msg_set_unsuback_packet_id(nni_msg *msg, uint16_t packet_id)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.unsuback.packet_id = packet_id;
}

uint16_t
nni_mqtt_msg_get_unsuback_packet_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.unsuback.packet_id;
}

void
nni_mqtt_msg_set_unsuback_return_codes(
    nni_msg *msg, uint8_t *ret_codes, uint32_t ret_codes_count)
{
	nni_mqtt_proto_data *proto_data         = nni_msg_get_proto_data(msg);
	proto_data->payload.unsuback.ret_code_arr = nni_alloc(ret_codes_count);
	memcpy(proto_data->payload.unsuback.ret_code_arr, ret_codes,
	    ret_codes_count);
	proto_data->payload.unsuback.ret_code_count = ret_codes_count;
}

uint8_t *
nni_mqtt_msg_get_unsuback_return_codes(nni_msg *msg, uint32_t *ret_codes_count)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*ret_codes_count = proto_data->payload.unsuback.ret_code_count;
	return proto_data->payload.unsuback.ret_code_arr;
}

property *
nni_mqtt_msg_get_unsuback_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.unsuback.properties;
}

void
nni_mqtt_msg_set_unsuback_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.unsuback.properties = prop;
}

void
nni_mqtt_msg_set_connect_clean_session(nni_msg *msg, bool clean_session)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connect.conn_flags.clean_session =
	    clean_session;
}

void
nni_mqtt_msg_set_connect_will_retain(nni_msg *msg, bool will_retain)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connect.conn_flags.will_retain = will_retain;
}

void
nni_mqtt_msg_set_connect_will_qos(nni_msg *msg, uint8_t will_qos)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connect.conn_flags.will_qos = will_qos;
}

void
nni_mqtt_msg_set_connect_will_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->payload.connect.will_properties = prop;
}

void
nni_mqtt_msg_set_connect_proto_version(nni_msg *msg, uint8_t version)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connect.protocol_version = version;
}

void
nni_mqtt_msg_set_disconnect_reason_code(nni_msg *msg, uint8_t reason_code)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.disconnect.reason_code = reason_code;
}

void
nni_mqtt_msg_set_disconnect_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.disconnect.properties = prop;
}

void
nni_mqtt_msg_set_connect_keep_alive(nni_msg *msg, uint16_t keep_alive)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connect.keep_alive = keep_alive;
}

bool
nni_mqtt_msg_get_connect_clean_session(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connect.conn_flags.clean_session;
}

bool
nni_mqtt_msg_get_connect_will_retain(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connect.conn_flags.will_retain;
}

uint8_t
nni_mqtt_msg_get_connect_will_qos(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connect.conn_flags.will_qos;
}

property *
nni_mqtt_msg_get_connect_will_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->payload.connect.will_properties;
}

uint8_t
nni_mqtt_msg_get_connect_proto_version(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connect.protocol_version;
}

uint16_t
nni_mqtt_msg_get_connect_keep_alive(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connect.keep_alive;
}

void
nni_mqtt_msg_set_connect_client_id(nni_msg *msg, const char *client_id)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	mqtt_buf_create(&proto_data->payload.connect.client_id,
	    (const uint8_t *) client_id, (uint32_t) strlen(client_id));
}

void
nni_mqtt_msg_set_connect_will_topic(nni_msg *msg, const char *will_topic)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	mqtt_buf_create(&proto_data->payload.connect.will_topic,
	    (const uint8_t *) will_topic, (uint32_t) strlen(will_topic));
}

void
nni_mqtt_msg_set_connect_will_msg(
    nni_msg *msg, uint8_t *will_msg, uint32_t len)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	mqtt_buf_create(&proto_data->payload.connect.will_msg,
	    (const uint8_t *) will_msg, len);
}

void
nni_mqtt_msg_set_connect_user_name(nni_msg *msg, const char *user_name)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	mqtt_buf_create(&proto_data->payload.connect.user_name,
	    (const uint8_t *) user_name, (uint32_t) strlen(user_name));
}

void
nni_mqtt_msg_set_connect_password(nni_msg *msg, const char *password)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	mqtt_buf_create(&proto_data->payload.connect.password,
	    (const uint8_t *) password, (uint32_t) strlen(password));
}

const char *
nni_mqtt_msg_get_connect_client_id(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return (const char *) proto_data->payload.connect.client_id.buf;
}

const char *
nni_mqtt_msg_get_connect_will_topic(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return (const char *) proto_data->payload.connect.will_topic.buf;
}

uint8_t *
nni_mqtt_msg_get_connect_will_msg(nni_msg *msg, uint32_t *len)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	*len = proto_data->payload.connect.will_msg.length;
	return proto_data->payload.connect.will_msg.buf;
}

const char *
nni_mqtt_msg_get_connect_user_name(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return (const char *) proto_data->payload.connect.user_name.buf;
}

const char *
nni_mqtt_msg_get_connect_password(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return (const char *) proto_data->payload.connect.password.buf;
}

void
nni_mqtt_msg_set_connack_return_code(nni_msg *msg, uint8_t code)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connack.conn_return_code = code;
}

void
nni_mqtt_msg_set_connack_flags(nni_msg *msg, uint8_t flags)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connack.connack_flags = flags;
}

void
nni_mqtt_msg_set_connack_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connack.properties = prop;
}

uint8_t
nni_mqtt_msg_get_connack_return_code(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connack.conn_return_code;
}

uint8_t
nni_mqtt_msg_get_connack_flags(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connack.connack_flags;
}

property *
nni_mqtt_msg_get_connack_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connack.properties;
}

property *
nni_mqtt_msg_get_disconnect_property(nng_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	return mqtt->var_header.disconnect.properties;
}

void
nni_mqtt_msg_dump(
    nni_msg *msg, uint8_t *buffer, uint32_t len, bool print_bytes)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);

	mqtt_buf mqbuf = { .buf = buffer, .length = len };

	nni_msg *mqtt_data;

	nni_msg_dup(&mqtt_data, msg);
	nni_msg_insert(
	    mqtt_data, nni_msg_header(msg), nni_msg_header_len(msg));

	mqtt_buf body = { .buf = nni_msg_body(mqtt_data),
		.length        = nni_msg_len(mqtt_data) };

	mqtt_msg_dump(proto_data, &mqbuf, &body, print_bytes);

	nni_msg_free(mqtt_data);
}

nni_mqtt_topic *
nni_mqtt_topic_array_create(size_t n)
{
	nni_mqtt_topic *topic;
	topic = (nni_mqtt_topic *) NNI_ALLOC_STRUCTS(topic, n);
	return topic;
}

void
nni_mqtt_topic_array_set(
    nni_mqtt_topic *topic, size_t index, const char *topic_name)
{
	topic[index].buf    = (uint8_t *) nni_strdup(topic_name);
	topic[index].length = (uint32_t) strlen(topic_name);
}

void
nni_mqtt_topic_array_free(nni_mqtt_topic *topic, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		nni_strfree((char *) topic[i].buf);
		topic[i].length = 0;
	}
	NNI_FREE_STRUCTS(topic, n);
}

nni_mqtt_topic_qos *
nni_mqtt_topic_qos_array_create(size_t n)
{
	nni_mqtt_topic_qos *tq;
	tq = NNI_ALLOC_STRUCTS(tq, n);
	return tq;
}

void
nni_mqtt_topic_qos_array_set(nni_mqtt_topic_qos *topic_qos, size_t index,
    const char *topic_name, uint32_t len, uint8_t qos, uint8_t nl, uint8_t rap, uint8_t rh)
{
	topic_qos[index].topic.buf       = (uint8_t *) nni_alloc(len * sizeof(uint8_t));
	memcpy(topic_qos[index].topic.buf, topic_name, len);
	topic_qos[index].topic.length    = len;
	topic_qos[index].qos             = qos;
	topic_qos[index].nolocal         = nl;
	topic_qos[index].rap             = rap;
	topic_qos[index].retain_handling = rh;
}

void
nni_mqtt_topic_qos_array_free(nni_mqtt_topic_qos *topic_qos, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		nni_free(topic_qos[i].topic.buf, topic_qos[i].topic.length);
		topic_qos[i].topic.length = 0;
	}
	NNI_FREE_STRUCTS(topic_qos, n);
}

nni_aio *
nni_mqtt_msg_get_aio(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->aio;
}

void
nni_mqtt_msg_set_aio(nni_msg *msg, nni_aio *aio)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->aio                 = aio;
}

void
mqtt_close_unack_msg_cb(void *key, void *val)
{
	NNI_ARG_UNUSED(key);

	nni_msg * msg = val;
	nni_aio * aio = NULL;

	aio = nni_mqtt_msg_get_aio(msg);
	if (aio && nni_aio_begin(aio) == true) {
		nni_aio_abort(&aio, NNG_ECLOSED);
	}
	nni_msg_free(msg);
}

void
nni_mqtt_msg_set_connect_property(nni_msg *msg, property *prop)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	proto_data->var_header.connect.properties = prop;
}

property *
nni_mqtt_msg_get_connect_property(nni_msg *msg)
{
	nni_mqtt_proto_data *proto_data = nni_msg_get_proto_data(msg);
	return proto_data->var_header.connect.properties;
}

int
mqtt_pipe_recv_msgq_putq(nni_lmq *lmq, nni_msg *msg)
{
	nni_msg *tmsg;
	// Dont resize lmq in sdk due to memory saving
	// Just make space for new Message
	if (nni_lmq_full(lmq)) {
		if (nni_lmq_get(lmq, &tmsg) == 0) {
			nni_println("Warning! msg lost due to busy socket");
			nni_msg_free(tmsg);
		}
	}
	if (0 != nni_lmq_put(lmq, msg)) {
		nni_println("Warning! msg enqueue failed");
		return -1;
	}
	return 0;
}
