
#include "mqtt_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void nni_mqtt_msg_append_u8(nni_msg *, uint8_t);
static void nni_mqtt_msg_append_u16(nni_msg *, uint16_t);
static void nni_mqtt_msg_append_u32(nni_msg *, uint32_t);

static void nni_mqtt_msg_append_byte_str(nni_msg *, nni_mqtt_buffer *);

static void nni_mqtt_msg_encode_fixed_header(nni_msg *, nni_mqtt_proto_data *);
static int  nni_mqtt_msg_encode_connect(nni_msg *);
static int  nni_mqtt_msg_encode_connack(nni_msg *);
static int  nni_mqtt_msg_encode_subscribe(nni_msg *);
static int  nni_mqtt_msg_encode_suback(nni_msg *);
static int  nni_mqtt_msg_encode_publish(nni_msg *);
static int  nni_mqtt_msg_encode_puback(nni_msg *);
static int  nni_mqtt_msg_encode_pubrec(nni_msg *);
static int  nni_mqtt_msg_encode_pubrel(nni_msg *);
static int  nni_mqtt_msg_encode_pubcomp(nni_msg *);
static int  nni_mqtt_msg_encode_unsubscribe(nni_msg *);
static int  nni_mqtt_msg_encode_unsuback(nni_msg *);
static int  nni_mqtt_msg_encode_base(nni_msg *);

static int  nni_mqttv5_msg_encode_connect(nni_msg *);
static int  nni_mqttv5_msg_encode_connack(nni_msg *);
static int  nni_mqttv5_msg_encode_subscribe(nni_msg *);
static int  nni_mqttv5_msg_encode_suback(nni_msg *);
static int  nni_mqttv5_msg_encode_publish(nni_msg *);
static int  nni_mqttv5_msg_encode_puback(nni_msg *);
static int  nni_mqttv5_msg_encode_pubrec(nni_msg *);
static int  nni_mqttv5_msg_encode_pubrel(nni_msg *);
static int  nni_mqttv5_msg_encode_pubcomp(nni_msg *);
static int  nni_mqttv5_msg_encode_unsubscribe(nni_msg *);
static int  nni_mqttv5_msg_encode_unsuback(nni_msg *);
static int  nni_mqttv5_msg_encode_base(nni_msg *);
static int  nni_mqttv5_msg_encode_disconnect(nni_msg *);

static int nni_mqtt_msg_decode_fixed_header(nni_msg *);
static int nni_mqtt_msg_decode_connect(nni_msg *);
static int nni_mqtt_msg_decode_connack(nni_msg *);
static int nni_mqtt_msg_decode_subscribe(nni_msg *);
static int nni_mqtt_msg_decode_suback(nni_msg *);
static int nni_mqtt_msg_decode_publish(nni_msg *);
static int nni_mqtt_msg_decode_puback(nni_msg *);
static int nni_mqtt_msg_decode_pubrec(nni_msg *);
static int nni_mqtt_msg_decode_pubrel(nni_msg *);
static int nni_mqtt_msg_decode_pubcomp(nni_msg *);
static int nni_mqtt_msg_decode_unsubscribe(nni_msg *);
static int nni_mqtt_msg_decode_unsuback(nni_msg *);
static int nni_mqtt_msg_decode_base(nni_msg *);

static int nni_mqttv5_msg_decode_connect(nni_msg *);
static int nni_mqttv5_msg_decode_connack(nni_msg *);
static int nni_mqttv5_msg_decode_subscribe(nni_msg *);
static int nni_mqttv5_msg_decode_suback(nni_msg *);
static int nni_mqttv5_msg_decode_publish(nni_msg *);
static int nni_mqttv5_msg_decode_puback(nni_msg *);
static int nni_mqttv5_msg_decode_pubrec(nni_msg *);
static int nni_mqttv5_msg_decode_pubrel(nni_msg *);
static int nni_mqttv5_msg_decode_pubcomp(nni_msg *);
static int nni_mqttv5_msg_decode_unsubscribe(nni_msg *);
static int nni_mqttv5_msg_decode_unsuback(nni_msg *);
static int nni_mqttv5_msg_decode_base(nni_msg *);
static int  nni_mqttv5_msg_decode_disconnect(nni_msg *);

static void destory_connect(nni_mqtt_proto_data *);
static void destory_publish(nni_mqtt_proto_data *);
static void destory_subscribe(nni_mqtt_proto_data *);
static void destory_suback(nni_mqtt_proto_data *);
static void destory_unsubscribe(nni_mqtt_proto_data *);
static void destory_unsuback(nni_mqtt_proto_data *mqtt);

static void dup_connect(nni_mqtt_proto_data *, nni_mqtt_proto_data *);
static void dup_publish(nni_mqtt_proto_data *, nni_mqtt_proto_data *);
static void dup_subscribe(nni_mqtt_proto_data *, nni_mqtt_proto_data *);
static void dup_suback(nni_mqtt_proto_data *, nni_mqtt_proto_data *);
static void dup_unsubscribe(nni_mqtt_proto_data *, nni_mqtt_proto_data *);

static void mqtt_msg_content_free(nni_mqtt_proto_data *);

typedef struct {
	nni_mqtt_packet_type packet_type;
	int (*encode)(nni_msg *);
	int (*decode)(nni_msg *);
} mqtt_msg_codec_handler;

static mqtt_msg_codec_handler codec_handler[] = {
	{ NNG_MQTT_CONNECT, nni_mqtt_msg_encode_connect,
	    nni_mqtt_msg_decode_connect },
	{ NNG_MQTT_CONNACK, nni_mqtt_msg_encode_connack,
	    nni_mqtt_msg_decode_connack },
	{ NNG_MQTT_PUBLISH, nni_mqtt_msg_encode_publish,
	    nni_mqtt_msg_decode_publish },
	{ NNG_MQTT_PUBACK, nni_mqtt_msg_encode_puback,
	    nni_mqtt_msg_decode_puback },
	{ NNG_MQTT_PUBREC, nni_mqtt_msg_encode_pubrec,
	    nni_mqtt_msg_decode_pubrec },
	{ NNG_MQTT_PUBREL, nni_mqtt_msg_encode_pubrel,
	    nni_mqtt_msg_decode_pubrel },
	{ NNG_MQTT_PUBCOMP, nni_mqtt_msg_encode_pubcomp,
	    nni_mqtt_msg_decode_pubcomp },
	{ NNG_MQTT_SUBSCRIBE, nni_mqtt_msg_encode_subscribe,
	    nni_mqtt_msg_decode_subscribe },
	{ NNG_MQTT_SUBACK, nni_mqtt_msg_encode_suback,
	    nni_mqtt_msg_decode_suback },
	{ NNG_MQTT_UNSUBSCRIBE, nni_mqtt_msg_encode_unsubscribe,
	    nni_mqtt_msg_decode_unsubscribe },
	{ NNG_MQTT_UNSUBACK, nni_mqtt_msg_encode_unsuback,
	    nni_mqtt_msg_decode_unsuback },
	{ NNG_MQTT_PINGREQ, nni_mqtt_msg_encode_base,
	    nni_mqtt_msg_decode_base },
	{ NNG_MQTT_PINGRESP, nni_mqtt_msg_encode_base,
	    nni_mqtt_msg_decode_base },
	{ NNG_MQTT_DISCONNECT, nni_mqtt_msg_encode_base,
	    nni_mqtt_msg_decode_base }
};

static mqtt_msg_codec_handler codec_v5_handler[] = {
	{ NNG_MQTT_CONNECT, nni_mqttv5_msg_encode_connect,
	    nni_mqttv5_msg_decode_connect },
	{ NNG_MQTT_CONNACK, nni_mqttv5_msg_encode_connack,
	    nni_mqttv5_msg_decode_connack },
	{ NNG_MQTT_PUBLISH, nni_mqttv5_msg_encode_publish,
	    nni_mqttv5_msg_decode_publish },
	{ NNG_MQTT_PUBACK, nni_mqttv5_msg_encode_puback,
	    nni_mqttv5_msg_decode_puback },
	{ NNG_MQTT_PUBREC, nni_mqttv5_msg_encode_pubrec,
	    nni_mqttv5_msg_decode_pubrec },
	{ NNG_MQTT_PUBREL, nni_mqttv5_msg_encode_pubrel,
	    nni_mqttv5_msg_decode_pubrel },
	{ NNG_MQTT_PUBCOMP, nni_mqttv5_msg_encode_pubcomp,
	    nni_mqttv5_msg_decode_pubcomp },
	{ NNG_MQTT_SUBSCRIBE, nni_mqttv5_msg_encode_subscribe,
	    nni_mqttv5_msg_decode_subscribe },
	{ NNG_MQTT_SUBACK, nni_mqttv5_msg_encode_suback,
	    nni_mqttv5_msg_decode_suback },
	{ NNG_MQTT_UNSUBSCRIBE, nni_mqttv5_msg_encode_unsubscribe,
	    nni_mqttv5_msg_decode_unsubscribe },
	{ NNG_MQTT_UNSUBACK, nni_mqttv5_msg_encode_unsuback,
	    nni_mqttv5_msg_decode_unsuback },
	{ NNG_MQTT_PINGREQ, nni_mqttv5_msg_encode_base,
	    nni_mqttv5_msg_decode_base },
	{ NNG_MQTT_PINGRESP, nni_mqttv5_msg_encode_base,
	    nni_mqttv5_msg_decode_base },
	{ NNG_MQTT_DISCONNECT, nni_mqttv5_msg_encode_disconnect,
	    nni_mqttv5_msg_decode_disconnect }
};

int
nni_mqtt_msg_encode(nni_msg *msg)
{
	nni_msg_clear(msg);
	nni_msg_header_clear(msg);

	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	for (size_t i = 0;
	     i < sizeof(codec_handler) / sizeof(mqtt_msg_codec_handler); i++) {
		if (codec_handler[i].packet_type ==
		    mqtt->fixed_header.common.packet_type) {
			if (!mqtt->initialized) {
				mqtt->initialized = true;
				mqtt->is_copied   = true;
			}
			mqtt->is_decoded = false;
			return codec_handler[i].encode(msg);
		}
	}

	return MQTT_ERR_PROTOCOL;
}

int
nni_mqttv5_msg_encode(nni_msg *msg)
{
	nni_msg_clear(msg);
	nni_msg_header_clear(msg);

	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	for (size_t i = 0;
	     i < sizeof(codec_v5_handler) / sizeof(mqtt_msg_codec_handler); i++) {
		if (codec_v5_handler[i].packet_type ==
		    mqtt->fixed_header.common.packet_type) {
			if (!mqtt->initialized) {
				mqtt->initialized = true;
				mqtt->is_copied   = true;
			}
			mqtt->is_copied  = true;
			return codec_v5_handler[i].encode(msg);
		}
	}

	return MQTT_ERR_PROTOCOL;
}

int
nni_mqtt_msg_decode(nni_msg *msg)
{
	int ret;
	if ((ret = nni_mqtt_msg_decode_fixed_header(msg)) != MQTT_SUCCESS) {
		// nni_plat_printf("decode_fixed_header failed %d\n", ret);
		return ret;
	}
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	for (size_t i = 0;
	     i < sizeof(codec_handler) / sizeof(mqtt_msg_codec_handler); i++) {
		if (codec_handler[i].packet_type ==
		    mqtt->fixed_header.common.packet_type) {
			mqtt_msg_content_free(mqtt);
			if (!mqtt->initialized) {
				mqtt->initialized = true;
				mqtt->is_copied   = false;
			}
			mqtt->is_decoded = true;
			return codec_handler[i].decode(msg);
		}
	}

	return MQTT_ERR_PROTOCOL;
}

int
nni_mqttv5_msg_decode(nni_msg *msg)
{
	int ret;
	if ((ret = nni_mqtt_msg_decode_fixed_header(msg)) != MQTT_SUCCESS) {
		// nni_plat_printf("decode_fixed_header failed %d\n", ret);
		return ret;
	}
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	for (size_t i = 0;
	     i < sizeof(codec_v5_handler) / sizeof(mqtt_msg_codec_handler); i++) {
		if (codec_v5_handler[i].packet_type ==
		    mqtt->fixed_header.common.packet_type) {
			mqtt_msg_content_free(mqtt);
			if (!mqtt->initialized) {
				mqtt->initialized = true;
				mqtt->is_copied   = false;
			}
			mqtt->is_decoded = true;
			return codec_v5_handler[i].decode(msg);
		}
	}

	return MQTT_ERR_PROTOCOL;
}

int
nni_mqtt_msg_packet_validate(
    uint8_t *buf, size_t buf_len, size_t header_len, uint8_t mqtt_version)
{
	nni_msg *check_msg = NULL;

	if (buf == NULL || buf_len < header_len) {
		return MQTT_ERR_PROTOCOL;
	}

	int rv = nni_mqtt_msg_alloc(&check_msg, buf_len - header_len);

	if (rv != 0) {
		return MQTT_ERR_NOMEM;
	}

	nng_msg_header_append(check_msg, buf, header_len);

	if (buf_len > header_len) {
		memcpy(nni_msg_body(check_msg), buf + header_len, buf_len - header_len);
	}

	if (mqtt_version == MQTT_PROTOCOL_VERSION_v5) {
		rv = nni_mqttv5_msg_decode(check_msg);
	} else {
		rv = nni_mqtt_msg_decode(check_msg);
	}

	nni_msg_free(check_msg);

	return rv;
}

int
nni_mqtt_msg_validate(nni_msg *msg, uint8_t mqtt_version)
{
	nni_msg *check_msg = NULL;

	size_t msg_len = nni_msg_len(msg);

	int rv = nni_mqtt_msg_alloc(&check_msg, msg_len);

	if (rv != 0) {
		return MQTT_ERR_NOMEM;
	}

	nni_msg_header_append(
	    check_msg, nni_msg_header(msg), nni_msg_header_len(msg));
	if (msg_len > 0) {
		memcpy(nni_msg_body(check_msg), nni_msg_body(msg), msg_len);
	}

	if (mqtt_version == MQTT_PROTOCOL_VERSION_v5) {
		rv = nni_mqttv5_msg_decode(check_msg);
	} else {
		rv = nni_mqtt_msg_decode(check_msg);
	}

	nni_msg_free(check_msg);

	return rv;
}

static void
mqtt_msg_content_free(nni_mqtt_proto_data *mqtt)
{
	switch (mqtt->fixed_header.common.packet_type) {
	case NNG_MQTT_CONNECT:
		if (mqtt->is_copied) {
			destory_connect(mqtt);
		}
		if (mqtt->var_header.connect.properties) {
			property_free(mqtt->var_header.connect.properties);
		}
		if (mqtt->payload.connect.will_properties) {
			property_free(mqtt->payload.connect.will_properties);
		}
		break;
	case NNG_MQTT_CONNACK:
		if (mqtt->var_header.connack.properties) {
			property_free(mqtt->var_header.connack.properties);
		}
		break;
	case NNG_MQTT_PUBLISH:
		if (mqtt->is_copied) {
			destory_publish(mqtt);
		}
		if (mqtt->var_header.publish.properties) {
			property_free(mqtt->var_header.publish.properties);
		}
		break;
	case NNG_MQTT_PUBACK:
		if (mqtt->var_header.puback.properties) {
			property_free(mqtt->var_header.puback.properties);
		}
		break;
	case NNG_MQTT_PUBREC:
		if (mqtt->var_header.pubrec.properties) {
			property_free(mqtt->var_header.pubrec.properties);
		}
		break;
	case NNG_MQTT_PUBREL:
		if (mqtt->var_header.pubrel.properties) {
			property_free(mqtt->var_header.pubrel.properties);
		}
		break;
	case NNG_MQTT_PUBCOMP:
		if (mqtt->var_header.pubcomp.properties) {
			property_free(mqtt->var_header.pubcomp.properties);
		}
		break;
	case NNG_MQTT_SUBSCRIBE:
		if (mqtt->is_copied) {
			destory_subscribe(mqtt);
		} else {
			nni_free(mqtt->payload.subscribe.topic_arr,
			    mqtt->payload.subscribe.topic_count *
			        sizeof(nni_mqtt_topic_qos));
			mqtt->payload.subscribe.topic_count = 0;
		}
		if (mqtt->var_header.subscribe.properties) {
			property_free(mqtt->var_header.subscribe.properties);
		}
		break;
	case NNG_MQTT_SUBACK:
		destory_suback(mqtt);
		if (mqtt->var_header.suback.properties) {
			property_free(mqtt->var_header.suback.properties);
		}
		break;
	case NNG_MQTT_UNSUBSCRIBE:
		if (mqtt->is_copied) {
			destory_unsubscribe(mqtt);
		} else {
			nni_free(mqtt->payload.unsubscribe.topic_arr,
			    mqtt->payload.unsubscribe.topic_count *
			        sizeof(nni_mqtt_topic));
			mqtt->payload.unsubscribe.topic_count = 0;
		}
		if (mqtt->var_header.unsubscribe.properties) {
			property_free(mqtt->var_header.unsubscribe.properties);
		}
		break;

	case NNG_MQTT_UNSUBACK:
		destory_unsuback(mqtt);
		if (mqtt->var_header.unsuback.properties) {
			property_free(mqtt->var_header.unsuback.properties);
		}
		break;
		
	case NNG_MQTT_DISCONNECT:
		if (mqtt->var_header.disconnect.properties) {
			property_free(mqtt->var_header.disconnect.properties);
		}
		break;

		// TODO case NNG_MQTT_AUTH:
		//
		// break;

	default:
		break;
	}
}

int
nni_mqtt_msg_free(void *self)
{
	if (self) {
		nni_mqtt_proto_data *mqtt = self;
		mqtt_msg_content_free(mqtt);
		free(mqtt);
		mqtt = NULL;
		return (0);
	}
	return (1);
}

int
nni_mqtt_msg_dup(void **dest, const void *src)
{
	nni_mqtt_proto_data *mqtt = (nni_mqtt_proto_data *) *dest;
	nni_mqtt_proto_data *s    = (nni_mqtt_proto_data *) src;

	mqtt = NNI_ALLOC_STRUCT(mqtt);
	memcpy(mqtt, (nni_mqtt_proto_data *) src, sizeof(nni_mqtt_proto_data));
	mqtt->initialized = false;

	switch (mqtt->fixed_header.common.packet_type) {
	case NNG_MQTT_CONNECT:
		if (mqtt->is_copied) {
			dup_connect(mqtt, s);
		}
		if (s->var_header.connect.properties) {
			property_dup(&mqtt->var_header.connect.properties,
			    s->var_header.connect.properties);
		}
		if (s->payload.connect.will_properties) {
			property_dup(&mqtt->var_header.connect.properties,
			    s->payload.connect.will_properties);
		}
		break;
	case NNG_MQTT_CONNACK:
		if (s->var_header.connack.properties) {
			property_dup(&mqtt->var_header.connack.properties,
			    s->var_header.connack.properties);
		}
		break;
	case NNG_MQTT_PUBLISH:
		if (mqtt->is_copied) {
			dup_publish(mqtt, s);
		}
		if (s->var_header.publish.properties) {
			property_dup(&mqtt->var_header.publish.properties,
			    s->var_header.publish.properties);
		}
		break;
	case NNG_MQTT_PUBACK:
		if (s->var_header.puback.properties) {
			property_dup(&mqtt->var_header.puback.properties,
			    s->var_header.puback.properties);
		}
		break;
	case NNG_MQTT_PUBREC:
		if (s->var_header.pubrec.properties) {
			property_dup(&mqtt->var_header.pubrec.properties,
			    s->var_header.pubrec.properties);
		}
		break;
	case NNG_MQTT_PUBREL:
		if (s->var_header.pubrel.properties) {
			property_dup(&mqtt->var_header.pubrel.properties,
			    s->var_header.pubrel.properties);
		}
		break;
	case NNG_MQTT_PUBCOMP:
		if (s->var_header.pubcomp.properties) {
			property_dup(&mqtt->var_header.pubcomp.properties,
			    s->var_header.pubcomp.properties);
		}
		break;
	case NNG_MQTT_SUBSCRIBE:
		if (mqtt->is_copied) {
			dup_subscribe(mqtt, s);
		} else {
			mqtt->payload.subscribe.topic_arr =
			    nni_alloc(s->payload.subscribe.topic_count *
			        sizeof(nni_mqtt_topic_qos));
			mqtt->payload.subscribe.topic_count =
			    s->payload.subscribe.topic_count;
			memcpy(mqtt->payload.subscribe.topic_arr,
			    s->payload.subscribe.topic_arr,
			    s->payload.subscribe.topic_count *
			        sizeof(nni_mqtt_topic_qos));
		}
		if (s->var_header.subscribe.properties) {
			property_dup(&mqtt->var_header.subscribe.properties,
			    s->var_header.subscribe.properties);
		}
		break;
	case NNG_MQTT_SUBACK:
		dup_suback(mqtt, s);
		if (s->var_header.suback.properties) {
			property_dup(&mqtt->var_header.suback.properties,
			    s->var_header.suback.properties);
		}
		break;
	case NNG_MQTT_UNSUBSCRIBE:
		if (mqtt->is_copied) {
			dup_unsubscribe(mqtt, s);
		} else {
			mqtt->payload.unsubscribe.topic_arr =
			    nni_alloc(s->payload.unsubscribe.topic_count *
			        sizeof(nni_mqtt_topic));
			mqtt->payload.unsubscribe.topic_count =
			    s->payload.unsubscribe.topic_count;
			memcpy(mqtt->payload.unsubscribe.topic_arr,
			    s->payload.unsubscribe.topic_arr,
			    s->payload.unsubscribe.topic_count *
			        sizeof(nni_mqtt_topic));
		}
		if (s->var_header.unsubscribe.properties) {
			property_dup(&mqtt->var_header.unsubscribe.properties,
			    s->var_header.unsubscribe.properties);
		}
		break;

	case NNG_MQTT_UNSUBACK:
		if (mqtt->var_header.unsuback.properties) {
			property_dup(&mqtt->var_header.unsuback.properties,
			    s->var_header.unsuback.properties);
		}
		break;

	case NNG_MQTT_DISCONNECT:
		if (mqtt->var_header.disconnect.properties) {
			property_dup(&mqtt->var_header.disconnect.properties,
			    s->var_header.disconnect.properties);
		}
		break;

	default:
		break;
	}

	*dest = mqtt;

	return (0);
}

static void
dup_connect(nni_mqtt_proto_data *dest, nni_mqtt_proto_data *src)
{
	mqtt_buf_dup(&dest->var_header.connect.protocol_name,
	    &src->var_header.connect.protocol_name);
	mqtt_buf_dup(
	    &dest->payload.connect.client_id, &src->payload.connect.client_id);
	mqtt_buf_dup(
	    &dest->payload.connect.user_name, &src->payload.connect.user_name);
	mqtt_buf_dup(
	    &dest->payload.connect.password, &src->payload.connect.password);
	mqtt_buf_dup(&dest->payload.connect.will_topic,
	    &src->payload.connect.will_topic);
	mqtt_buf_dup(
	    &dest->payload.connect.will_msg, &src->payload.connect.will_msg);
}

static void
dup_publish(nni_mqtt_proto_data *dest, nni_mqtt_proto_data *src)
{
	mqtt_buf_dup(&dest->var_header.publish.topic_name,
	    &src->var_header.publish.topic_name);
	mqtt_buf_dup(
	    &dest->payload.publish.payload, &src->payload.publish.payload);
}

static void
dup_subscribe(nni_mqtt_proto_data *dest, nni_mqtt_proto_data *src)
{
	dest->payload.subscribe.topic_arr = nni_mqtt_topic_qos_array_create(
	    src->payload.subscribe.topic_count);
	dest->payload.subscribe.topic_count =
	    src->payload.subscribe.topic_count;

	for (size_t i = 0; i < src->payload.subscribe.topic_count; i++) {
		nni_mqtt_topic_qos_array_set(dest->payload.subscribe.topic_arr,
		    i, (const char *) src->payload.subscribe.topic_arr[i].topic.buf,
		    src->payload.subscribe.topic_arr[i].topic.length,
		    src->payload.subscribe.topic_arr[i].qos,
			src->payload.subscribe.topic_arr[i].nolocal,
			src->payload.subscribe.topic_arr[i].rap,
			src->payload.subscribe.topic_arr[i].retain_handling
			);
	}
}

static void
dup_suback(nni_mqtt_proto_data *dest, nni_mqtt_proto_data *src)
{
	dest->payload.suback.ret_code_arr =
	    nni_alloc(src->payload.suback.ret_code_count);
	dest->payload.suback.ret_code_count =
	    src->payload.suback.ret_code_count;
	memcpy(dest->payload.suback.ret_code_arr,
	    src->payload.suback.ret_code_arr,
	    src->payload.suback.ret_code_count);
}

static void
dup_unsubscribe(nni_mqtt_proto_data *dest, nni_mqtt_proto_data *src)
{
	dest->payload.unsubscribe.topic_arr =
	    nni_mqtt_topic_array_create(src->payload.unsubscribe.topic_count);
	dest->payload.unsubscribe.topic_count =
	    src->payload.unsubscribe.topic_count;

	for (size_t i = 0; i < src->payload.unsubscribe.topic_count; i++) {
		nni_mqtt_topic_array_set(dest->payload.unsubscribe.topic_arr,
		    i,
		    (const char *) src->payload.unsubscribe.topic_arr[i].buf);
	}
}

static void
destory_connect(nni_mqtt_proto_data *mqtt)
{
	mqtt_buf_free(&mqtt->var_header.connect.protocol_name);
	mqtt_buf_free(&mqtt->payload.connect.client_id);
	mqtt_buf_free(&mqtt->payload.connect.user_name);
	mqtt_buf_free(&mqtt->payload.connect.password);
	mqtt_buf_free(&mqtt->payload.connect.will_topic);
	mqtt_buf_free(&mqtt->payload.connect.will_msg);
}

static void
destory_publish(nni_mqtt_proto_data *mqtt)
{
	mqtt_buf_free(&mqtt->var_header.publish.topic_name);
	mqtt_buf_free(&mqtt->payload.publish.payload);
}

static void
destory_subscribe(nni_mqtt_proto_data *mqtt)
{
	nni_mqtt_topic_qos_array_free(mqtt->payload.subscribe.topic_arr,
	    mqtt->payload.subscribe.topic_count);
	mqtt->payload.subscribe.topic_count = 0;
}

static void
destory_suback(nni_mqtt_proto_data *mqtt)
{
	if (mqtt->payload.suback.ret_code_count > 0) {
		nni_free(mqtt->payload.suback.ret_code_arr,
		    mqtt->payload.suback.ret_code_count);
		mqtt->payload.suback.ret_code_arr   = NULL;
		mqtt->payload.suback.ret_code_count = 0;
	}
}

static void
destory_unsuback(nni_mqtt_proto_data *mqtt)
{
	if (mqtt->payload.unsuback.ret_code_count > 0) {
		nni_free(mqtt->payload.unsuback.ret_code_arr,
		    mqtt->payload.unsuback.ret_code_count);
		mqtt->payload.unsuback.ret_code_arr   = NULL;
		mqtt->payload.unsuback.ret_code_count = 0;
	}
}

static void
destory_unsubscribe(nni_mqtt_proto_data *mqtt)
{
	nni_mqtt_topic_array_free(mqtt->payload.unsubscribe.topic_arr,
	    mqtt->payload.unsubscribe.topic_count);
	mqtt->payload.unsubscribe.topic_count = 0;
}

static void
nni_mqtt_msg_append_u8(nni_msg *msg, uint8_t val)
{
	nni_msg_append(msg, &val, 1);
}

static void
nni_mqtt_msg_append_u16(nni_msg *msg, uint16_t val)
{
	uint8_t buf[2] = { 0 };
	NNI_PUT16(buf, val);
	nni_msg_append(msg, buf, 2);
}

static void
nni_mqtt_msg_append_u32(nni_msg *msg, uint32_t val)
{
	uint8_t buf[4] = { 0 };
	NNI_PUT32(buf, val);
	nni_msg_append(msg, buf, 4);
}

static void
nni_mqtt_msg_append_varint(nni_msg *msg, uint32_t val)
{
	uint8_t        buf[4] = { 0 };
	struct pos_buf pbuf   = { .curpos = &buf[0], .endpos = &buf[4] };
	int            bytes  = write_variable_length_value(val, &pbuf);
	nni_msg_append(msg, buf, bytes);
}

static void
nni_mqtt_msg_append_byte_str(nni_msg *msg, nni_mqtt_buffer *str)
{
	nni_mqtt_msg_append_u16(msg, (uint16_t) str->length);
	nni_msg_append(msg, str->buf, str->length);
}

static void
nni_mqtt_msg_encode_fixed_header(nni_msg *msg, nni_mqtt_proto_data *data)
{
	uint8_t        rlen[4] = { 0 };
	struct pos_buf buf     = { .curpos = &rlen[0],
                .endpos                = &rlen[sizeof(rlen)] };

	nni_msg_header_clear(msg);
	uint8_t header = *(uint8_t *) &data->fixed_header.common;

	nni_msg_header_append(msg, &header, 1);

	int len = write_variable_length_value(
	    data->fixed_header.remaining_length, &buf);
	data->used_bytes = len;
	nni_msg_header_append(msg, rlen, len);
}

static int
nni_mqtt_msg_encode_connect(nni_msg *msg)
{
	int                  poslength     = 6;
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);
	char                 client_id[20] = { 0 };

	nni_msg_clear(msg);

	mqtt_connect_vhdr *var_header = &mqtt->var_header.connect;

	if (var_header->protocol_name.length == 0) {
		mqtt_buf_create(&var_header->protocol_name,
		    (const uint8_t *) MQTT_PROTOCOL_NAME,
		    strlen(MQTT_PROTOCOL_NAME));
	}

	if (var_header->protocol_version == 0) {
		var_header->protocol_version = 4;
	}

	if (mqtt->payload.connect.client_id.length == 0) {
		snprintf(client_id, 20, "nanomq-%04x", nni_random());
		mqtt_buf_create(&mqtt->payload.connect.client_id,
		    (const uint8_t *) client_id, (uint32_t) strlen(client_id));
	}

	poslength += var_header->protocol_name.length;
	/* add the length of payload part */
	mqtt_connect_payload *payload = &mqtt->payload.connect;
	/* Clientid length */
	poslength += payload->client_id.length + 2;

	/* Will Topic */
	if (payload->will_topic.length > 0) {
		poslength += 2 + payload->will_topic.length;
		var_header->conn_flags.will_flag = 1;
	}
	/* Will Message */
	if (payload->will_msg.length > 0) {
		poslength += 2 + payload->will_msg.length;
		var_header->conn_flags.will_flag = 1;
	}
	/* User Name */
	if (payload->user_name.length > 0) {
		poslength += 2 + payload->user_name.length;
		var_header->conn_flags.username_flag = 1;
	}
	/* Password */
	if (payload->password.length > 0) {
		poslength += 2 + payload->password.length;
		var_header->conn_flags.password_flag = 1;
	}

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	if (mqtt->fixed_header.remaining_length > MQTT_MAX_MSG_LEN) {
		return MQTT_ERR_PAYLOAD_SIZE;
	}
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	nni_mqtt_msg_append_byte_str(msg, &var_header->protocol_name);

	nni_mqtt_msg_append_u8(msg, var_header->protocol_version);

	/* Connect Flags */
	nni_mqtt_msg_append_u8(msg, *(uint8_t *) &var_header->conn_flags);

	/* Keep Alive */
	if (var_header->keep_alive == 0) {
		var_header->keep_alive = 60;
	}
	nni_mqtt_msg_append_u16(msg, var_header->keep_alive);

	/* Now we are in payload part */

	/* Client Identifier */
	/* Client Identifier is mandatory */
	nni_mqtt_msg_append_byte_str(msg, &payload->client_id);

	/* Will Topic */
	if (payload->will_topic.length) {
		if (!(var_header->conn_flags.will_flag)) {
			return MQTT_ERR_PROTOCOL;
		}
		nni_mqtt_msg_append_byte_str(msg, &payload->will_topic);
	} else {
		if (var_header->conn_flags.will_flag) {
			return MQTT_ERR_PROTOCOL;
		}
	}

	/* Will Message */
	if (payload->will_msg.length) {
		if (!(var_header->conn_flags.will_flag)) {
			return MQTT_ERR_PROTOCOL;
		}
		nni_mqtt_msg_append_byte_str(msg, &payload->will_msg);
	} else {
		if (var_header->conn_flags.will_flag) {
			return MQTT_ERR_PROTOCOL;
		}
	}

	/* User-Name */
	if (payload->user_name.length) {
		if (!(var_header->conn_flags.username_flag)) {
			return MQTT_ERR_PROTOCOL;
		}
		nni_mqtt_msg_append_byte_str(msg, &payload->user_name);
	} else {
		if (var_header->conn_flags.username_flag) {
			return MQTT_ERR_PROTOCOL;
		}
	}

	/* Password */
	if (payload->password.length) {
		if (!(var_header->conn_flags.password_flag)) {
			return MQTT_ERR_PROTOCOL;
		}
		nni_mqtt_msg_append_byte_str(msg, &payload->password);
	} else {
		if (var_header->conn_flags.password_flag) {
			return MQTT_ERR_PROTOCOL;
		}
	}

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_connect(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);
	char                 client_id[20] = { 0 };

	nni_msg_clear(msg);

	int poslength = 6;
	int rv = 0;

	mqtt_connect_vhdr *var_header = &mqtt->var_header.connect;

	if (var_header->protocol_name.length == 0) {
		mqtt_buf_create(&var_header->protocol_name,
		    (const uint8_t *) MQTT_PROTOCOL_NAME,
		    strlen(MQTT_PROTOCOL_NAME));
	}

	if (var_header->protocol_version == 0) {
		var_header->protocol_version = 5;
	}

	if (mqtt->payload.connect.client_id.length == 0) {
		snprintf(client_id, 20, "nanomq-%04x", nni_random());
		mqtt_buf_create(&mqtt->payload.connect.client_id,
		    (const uint8_t *) client_id, (uint32_t) strlen(client_id));
	}

	poslength += var_header->protocol_name.length;
	/* add the length of payload part */
	mqtt_connect_payload *payload = &mqtt->payload.connect;
	/* Clientid length */
	poslength += payload->client_id.length + 2;

	/* Will Topic */
	if (payload->will_topic.length > 0) {
		poslength += 2 + payload->will_topic.length;
		var_header->conn_flags.will_flag = 1;
	}
	/* Will Message */
	if (payload->will_msg.length > 0) {
		poslength += 2 + payload->will_msg.length;
		var_header->conn_flags.will_flag = 1;
	}
	/* User Name */
	if (payload->user_name.length > 0) {
		poslength += 2 + payload->user_name.length;
		var_header->conn_flags.username_flag = 1;
	}
	/* Password */
	if (payload->password.length > 0) {
		poslength += 2 + payload->password.length;
		var_header->conn_flags.password_flag = 1;
	}

	nni_mqtt_msg_append_byte_str(msg, &var_header->protocol_name);

	nni_mqtt_msg_append_u8(msg, var_header->protocol_version);

	/* Connect Flags */
	nni_mqtt_msg_append_u8(msg, *(uint8_t *) &var_header->conn_flags);

	/* Keep Alive */
	if (var_header->keep_alive == 0) {
		var_header->keep_alive = 60;
	}
	nni_mqtt_msg_append_u16(msg, var_header->keep_alive);

	/* Encode properties */
	rv = encode_properties(msg, var_header->properties, CMD_CONNECT);
	if (rv != 0)
		return rv;

	/* Now we are in payload part */

	/* Client Identifier */
	/* Client Identifier is mandatory */
	nni_mqtt_msg_append_byte_str(msg, &payload->client_id);

	if (var_header->conn_flags.will_flag) {
		/* Will Properties */
		rv = encode_properties(
		    msg, payload->will_properties, CMD_CONNECT);
		if (rv != 0)
			return rv;

		/* Will Topic */
		if (payload->will_topic.length) {
			nni_mqtt_msg_append_byte_str(
			    msg, &payload->will_topic);
		} else {
			return MQTT_ERR_PROTOCOL;
		}

		/* Will Message */
		if (payload->will_msg.length) {
			nni_mqtt_msg_append_byte_str(msg, &payload->will_msg);
		} else {
			return MQTT_ERR_PROTOCOL;
		}
	}

	/* User-Name */
	if (payload->user_name.length) {
		if (!(var_header->conn_flags.username_flag)) {
			return MQTT_ERR_PROTOCOL;
		}
		nni_mqtt_msg_append_byte_str(msg, &payload->user_name);
	} else {
		if (var_header->conn_flags.username_flag) {
			return MQTT_ERR_PROTOCOL;
		}
	}

	/* Password */
	if (payload->password.length) {
		if (!(var_header->conn_flags.password_flag)) {
			return MQTT_ERR_PROTOCOL;
		}
		nni_mqtt_msg_append_byte_str(msg, &payload->password);
	} else {
		if (var_header->conn_flags.password_flag) {
			return MQTT_ERR_PROTOCOL;
		}
	}

	/* Encode fix header finally */
	mqtt->fixed_header.remaining_length = (uint32_t) nni_msg_len(msg);
	if (mqtt->fixed_header.remaining_length > MQTT_MAX_MSG_LEN) {
		return MQTT_ERR_PAYLOAD_SIZE;
	}
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_connack(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* ConnAck Flags(1) + Connect Return Code(1) */

	mqtt_connack_vhdr *var_header = &mqtt->var_header.connack;

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	/* Connect Acknowledge Flags */
	nni_mqtt_msg_append_u8(msg, *(uint8_t *) &var_header->connack_flags);

	/* Connect Return Code */
	nni_mqtt_msg_append_u8(
	    msg, *(uint8_t *) &var_header->conn_return_code);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_connack(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	mqtt_connack_vhdr *var_header = &mqtt->var_header.connack;

	/* Connect Acknowledge Flags */
	nni_mqtt_msg_append_u8(msg, *(uint8_t *) &var_header->connack_flags);

	/* Connect Return Code */
	nni_mqtt_msg_append_u8(
	    msg, *(uint8_t *) &var_header->conn_return_code);

	encode_properties(msg, var_header->properties, CMD_CONNACK);

	/* Encode fix header finally */
	mqtt->fixed_header.remaining_length = (uint32_t) nni_msg_len(msg);
	if (mqtt->fixed_header.remaining_length > MQTT_MAX_MSG_LEN) {
		return MQTT_ERR_PAYLOAD_SIZE;
	}
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_subscribe(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 0;

	poslength += 2; /* for Packet Identifier */

	mqtt_subscribe_payload *spld = &mqtt->payload.subscribe;

	/* Go through topic filters to calculate length information */
	for (size_t i = 0; i < spld->topic_count; i++) {
		mqtt_topic_qos *topic = &spld->topic_arr[i];
		poslength += topic->topic.length;
		poslength += 1; // for 'options' byte
		poslength += 2; // for 'length' field of Topic Filter, which is
		                // encoded as UTF-8 encoded strings */
	}

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	mqtt->fixed_header.common.bit_1     = 1;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	mqtt_subscribe_vhdr *var_header = &mqtt->var_header.subscribe;
	/* Packet Id */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Subscribe topic_arr */
	for (size_t i = 0; i < spld->topic_count; i++) {
		mqtt_topic_qos *topic = &spld->topic_arr[i];
		nni_mqtt_msg_append_byte_str(msg, &topic->topic);
		nni_mqtt_msg_append_u8(msg, topic->qos);
	}

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_subscribe(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 0;

	poslength += 2; /* for Packet Identifier */

	mqtt_subscribe_payload *spld = &mqtt->payload.subscribe;

	/* Go through topic filters to calculate length information */
	for (size_t i = 0; i < spld->topic_count; i++) {
		mqtt_topic_qos *topic = &spld->topic_arr[i];
		poslength += topic->topic.length;
		poslength += 1; // for 'options' byte
		poslength += 2; // for 'length' field of Topic Filter, which is
		                // encoded as UTF-8 encoded strings */
	}

	mqtt_subscribe_vhdr *var_header = &mqtt->var_header.subscribe;
	/* Packet Id */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Properties */
	encode_properties(msg, var_header->properties, CMD_SUBSCRIBE);

	/* Subscribe topic_arr */
	for (size_t i = 0; i < spld->topic_count; i++) {
		mqtt_topic_qos *topic = &spld->topic_arr[i];
		nni_mqtt_msg_append_byte_str(msg, &topic->topic);
		uint8_t buf = topic->qos
			| (topic->nolocal << 2)
			| (topic->rap << 3)
			| (topic->retain_handling << 4);
		nni_mqtt_msg_append_u8(msg, buf);
	}

	/* Fixed header */
	mqtt->fixed_header.remaining_length = (uint32_t) nni_msg_len(msg);
	mqtt->fixed_header.common.bit_1     = 1;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_suback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_suback_vhdr *   var_header = &mqtt->var_header.suback;
	mqtt_suback_payload *spld       = &mqtt->payload.suback;

	poslength += spld->ret_code_count;

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Return Codes */
	nni_msg_append(msg, spld->ret_code_arr, spld->ret_code_count);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_suback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_suback_vhdr *   var_header = &mqtt->var_header.suback;
	mqtt_suback_payload *spld       = &mqtt->payload.suback;

	poslength += spld->ret_code_count;

	/* Properties */
	encode_properties(msg, var_header->properties, CMD_SUBACK);

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Return Codes */
	nni_msg_append(msg, spld->ret_code_arr, spld->ret_code_count);

	/* Fixed header */
	mqtt->fixed_header.remaining_length = (uint32_t) nni_msg_len(msg);
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_publish(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 0;

	poslength += 2; /* for Topic Name length field */
	poslength += mqtt->var_header.publish.topic_name.length;
	/* Packet Identifier is requested if QoS>0 */
	if (mqtt->fixed_header.publish.qos > 0) {
		poslength += 2; /* for Packet Identifier */
	}
	poslength += mqtt->payload.publish.payload.length;
	mqtt->fixed_header.remaining_length = (uint32_t) poslength;

	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	mqtt_publish_vhdr *var_header = &mqtt->var_header.publish;

	/* Topic Name */
	nni_mqtt_msg_append_byte_str(msg, &var_header->topic_name);

	if (mqtt->fixed_header.publish.qos > 0) {
		/* Packet Id */
		nni_mqtt_msg_append_u16(msg, var_header->packet_id);
	}

	/* Payload */
	if (mqtt->payload.publish.payload.length > 0) {
		nni_msg_append(msg, mqtt->payload.publish.payload.buf,
		    mqtt->payload.publish.payload.length);
	}

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_publish(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 0;

	poslength += 2; /* for Topic Name length field */
	poslength += mqtt->var_header.publish.topic_name.length;
	/* Packet Identifier is requested if QoS>0 */
	if (mqtt->fixed_header.publish.qos > 0) {
		poslength += 2; /* for Packet Identifier */
	}
	poslength += mqtt->payload.publish.payload.length;
	mqtt->fixed_header.remaining_length = (uint32_t) poslength;


	mqtt_publish_vhdr *var_header = &mqtt->var_header.publish;

	/* Topic Name */
	nni_mqtt_msg_append_byte_str(msg, &var_header->topic_name);

	if (mqtt->fixed_header.publish.qos > 0) {
		/* Packet Id */
		nni_mqtt_msg_append_u16(msg, var_header->packet_id);
	}

	encode_properties(msg, mqtt->var_header.publish.properties, CMD_PUBLISH);

	/* Payload */
	if (mqtt->payload.publish.payload.length > 0) {
		nni_msg_append(msg, mqtt->payload.publish.payload.buf,
		    mqtt->payload.publish.payload.length);
	}

	mqtt->fixed_header.remaining_length = nng_msg_len(msg);
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_puback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_puback_vhdr *var_header = &mqtt->var_header.puback;

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_puback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_puback_vhdr *var_header = &mqtt->var_header.puback;

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);
	encode_properties(msg, mqtt->var_header.puback.properties, CMD_PUBACK);

	mqtt->fixed_header.remaining_length = nng_msg_len(msg);
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_pubrec(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength                       = 2; /* for Packet Identifier */
	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	mqtt_pubrec_vhdr *var_header = &mqtt->var_header.pubrec;

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_pubrec(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength                       = 2; /* for Packet Identifier */
	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	mqtt_pubrec_vhdr *var_header = &mqtt->var_header.pubrec;

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);
	encode_properties(msg, mqtt->var_header.pubrec.properties, CMD_PUBREC);

	mqtt->fixed_header.remaining_length = nng_msg_len(msg);
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);


	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_pubrel(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_pubrel_vhdr *var_header = &mqtt->var_header.pubrel;

	mqtt->fixed_header.common.bit_1     = 1;
	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_pubrel(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_pubrel_vhdr *var_header = &mqtt->var_header.pubrel;

	mqtt->fixed_header.common.bit_1     = 1;
	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	encode_properties(msg, mqtt->var_header.pubrel.properties, CMD_PUBREL);

	mqtt->fixed_header.remaining_length = nng_msg_len(msg);
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_pubcomp(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_pubcomp_vhdr *var_header = &mqtt->var_header.pubcomp;

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_pubcomp(nni_msg *msg)
{
	NNI_ARG_UNUSED(msg);
	return 0;
}

static int
nni_mqtt_msg_encode_unsubscribe(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 0;

	poslength += 2; /* for Packet Identifier */

	mqtt_unsubscribe_payload *uspld = &mqtt->payload.unsubscribe;

	/* Go through topic filters to calculate length information */
	for (size_t i = 0; i < uspld->topic_count; i++) {
		mqtt_buf *topic = &uspld->topic_arr[i];
		poslength += topic->length;
		poslength += 2; // for 'length' field of Topic Filter, which is
		                // encoded as UTF-8 encoded strings */
	}

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	mqtt->fixed_header.common.bit_1     = 1;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	mqtt_unsubscribe_vhdr *var_header = &mqtt->var_header.unsubscribe;
	/* Packet Id */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Unsubscribe topic_arr */
	for (size_t i = 0; i < uspld->topic_count; i++) {
		mqtt_buf *topic = &uspld->topic_arr[i];
		nni_mqtt_msg_append_byte_str(msg, topic);
	}

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_unsubscribe(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 0;

	poslength += 2; /* for Packet Identifier */

	mqtt_unsubscribe_payload *uspld = &mqtt->payload.unsubscribe;

	/* Go through topic filters to calculate length information */
	for (size_t i = 0; i < uspld->topic_count; i++) {
		mqtt_buf *topic = &uspld->topic_arr[i];
		poslength += topic->length;
		poslength += 2; // for 'length' field of Topic Filter, which is
		                // encoded as UTF-8 encoded strings */
	}

	mqtt_unsubscribe_vhdr *var_header = &mqtt->var_header.unsubscribe;
	/* Packet Id */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Properties */
	encode_properties(msg, var_header->properties, CMD_UNSUBSCRIBE);

	/* Unsubscribe topic_arr */
	for (size_t i = 0; i < uspld->topic_count; i++) {
		mqtt_buf *topic = &uspld->topic_arr[i];
		nni_mqtt_msg_append_byte_str(msg, topic);
	}

	/* Fixed header */
	mqtt->fixed_header.remaining_length = (uint32_t) nni_msg_len(msg);
	mqtt->fixed_header.common.bit_1     = 1;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_unsuback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	int poslength = 2; /* for Packet Identifier */

	mqtt_unsuback_vhdr *var_header = &mqtt->var_header.unsuback;

	mqtt->fixed_header.remaining_length = (uint32_t) poslength;
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_unsuback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	mqtt_unsuback_vhdr *var_header = &mqtt->var_header.unsuback;
	mqtt_unsuback_payload *spld    = &mqtt->payload.unsuback;

	/* Packet Identifier */
	nni_mqtt_msg_append_u16(msg, var_header->packet_id);

	/* Properties */
	encode_properties(msg, var_header->properties, CMD_UNSUBACK);

	/* Return Codes */
	nni_msg_append(msg, spld->ret_code_arr, spld->ret_code_count);

	/* Fixed header */
	mqtt->fixed_header.remaining_length = (uint32_t) nni_msg_len(msg);
	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_encode_base(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	mqtt->fixed_header.remaining_length = 0;

	nni_mqtt_msg_encode_fixed_header(msg, mqtt);

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_encode_base(nni_msg *msg)
{
	NNI_ARG_UNUSED(msg);
	return 0;
}

static int
nni_mqttv5_msg_encode_disconnect(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);
	nni_msg_clear(msg);

	mqtt_disconnect_vhdr *var_header = &mqtt->var_header.disconnect;
	if (0 == var_header->reason_code && NULL == var_header->properties) {
		mqtt->fixed_header.remaining_length = 0;
	} else {
		nni_mqtt_msg_append_u8(msg, var_header->reason_code);
		encode_properties(msg, mqtt->var_header.disconnect.properties, CMD_DISCONNECT);
		mqtt->fixed_header.remaining_length = nng_msg_len(msg);
	}

	nni_mqtt_msg_encode_fixed_header(msg, mqtt);
	return MQTT_SUCCESS;
}


static int
nni_mqtt_msg_decode_fixed_header(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	size_t   len    = nni_msg_header_len(msg);
	uint8_t *header = nni_msg_header(msg);

	if (len < 2) {
		return MQTT_ERR_PROTOCOL;
	}

	memcpy(&mqtt->fixed_header.common, header, 1);

	uint8_t  used_bytes;
	uint32_t remain_len = 0;

	int ret;
	if ((ret = mqtt_get_remaining_length(header, (uint32_t) len,
	         &remain_len, &used_bytes)) != MQTT_SUCCESS) {
		return ret;
	}

	mqtt->fixed_header.remaining_length = remain_len;
	mqtt->used_bytes                    = used_bytes;

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_connect(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	/* Protocol Name */
	ret = read_str_data(&buf, &mqtt->var_header.connect.protocol_name);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}
	/* Protocol Level */
	ret = read_byte(&buf, &mqtt->var_header.connect.protocol_version);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}
	/* Protocol Level */
	ret =
	    read_byte(&buf, (uint8_t *) &mqtt->var_header.connect.conn_flags);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Keep Alive */
	ret = read_uint16(&buf, &mqtt->var_header.connect.keep_alive);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}
	/* Client Identifier */
	ret = read_utf8_str(&buf, &mqtt->payload.connect.client_id);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}
	if (mqtt->var_header.connect.conn_flags.will_flag) {
		/* Will Topic */
		ret = read_utf8_str(&buf, &mqtt->payload.connect.will_topic);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
		/* Will Message */
		ret = read_str_data(&buf, &mqtt->payload.connect.will_msg);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
	}
	if (mqtt->var_header.connect.conn_flags.username_flag) {
		/* Username */
		ret = read_utf8_str(&buf, &mqtt->payload.connect.user_name);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
	}
	if (mqtt->var_header.connect.conn_flags.password_flag) {
		/* Password */
		ret = read_str_data(&buf, &mqtt->payload.connect.password);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
	}
	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_decode_connect(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	/* Protocol Name */
	ret = read_str_data(&buf, &mqtt->var_header.connect.protocol_name);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}
	/* Protocol Level */
	ret = read_byte(&buf, &mqtt->var_header.connect.protocol_version);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}
	/* Protocol Level */
	ret =
	    read_byte(&buf, (uint8_t *) &mqtt->var_header.connect.conn_flags);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Keep Alive */
	ret = read_uint16(&buf, &mqtt->var_header.connect.keep_alive);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.connect.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;

	// Check connect properties
	property *prop = mqtt->var_header.connect.properties;
	if (prop != NULL) {
		if ((ret = check_properties(prop)) != SUCCESS)
			return ret;
		// Check Invalid properties
		for (property *p = prop->next; p != NULL; p = p->next) {
			switch (p->id) {
			case REQUEST_RESPONSE_INFORMATION:
			case REQUEST_PROBLEM_INFORMATION:
				if (p->data.p_value.u8 > 1)
					return PROTOCOL_ERROR;
				break;
			case SESSION_EXPIRY_INTERVAL:
			case RECEIVE_MAXIMUM:
			case MAXIMUM_PACKET_SIZE:
			case TOPIC_ALIAS_MAXIMUM:
			case USER_PROPERTY:
			case AUTHENTICATION_METHOD:
			case AUTHENTICATION_DATA:
				break;
			default:
				return PROTOCOL_ERROR;
			}
		}
	}

	/* Client Identifier */
	ret = read_utf8_str(&buf, &mqtt->payload.connect.client_id);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	if (mqtt->var_header.connect.conn_flags.will_flag) {
		/* Will Properties */
		pos = buf.curpos - &body[0];
		prop_len = 0;
		mqtt->payload.connect.will_properties =
		    decode_buf_properties(body, length, &pos, &prop_len, true);
		buf.curpos = &body[0] + pos;

		property *will_prop = mqtt->payload.connect.will_properties;
		if (will_prop != NULL) {
			if ((ret = check_properties(will_prop)) != SUCCESS)
				return ret;
			// Check Invalid properties
			for (property *p = prop->next; p != NULL;
			     p           = p->next) {
				switch (p->id) {
				case PAYLOAD_FORMAT_INDICATOR:
					if (p->data.p_value.u8 > 1)
						return PAYLOAD_FORMAT_INVALID;
					break;
				case WILL_DELAY_INTERVAL:
				case MESSAGE_EXPIRY_INTERVAL:
				case CONTENT_TYPE:
				case RESPONSE_TOPIC:
				case CORRELATION_DATA:
				case USER_PROPERTY:
					break;
				default:
					return PROTOCOL_ERROR;
				}
			}
		}

		/* Will Topic */
		ret = read_utf8_str(&buf, &mqtt->payload.connect.will_topic);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
		/* Will Message */
		ret = read_str_data(&buf, &mqtt->payload.connect.will_msg);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
	}
	if (mqtt->var_header.connect.conn_flags.username_flag) {
		/* Username */
		ret = read_utf8_str(&buf, &mqtt->payload.connect.user_name);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
	}
	if (mqtt->var_header.connect.conn_flags.password_flag) {
		/* Password */
		ret = read_str_data(&buf, &mqtt->payload.connect.password);
		if (ret != 0) {
			return MQTT_ERR_PROTOCOL;
		}
	}
	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_decode_disconnect(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	ret = read_byte(&buf, &mqtt->var_header.disconnect.reason_code);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.disconnect.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_connack(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	int result = read_byte(&buf, &mqtt->var_header.connack.connack_flags);
	if (result != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Connect Return Code */
	result = read_byte(&buf, &mqtt->var_header.connack.connack_flags);
	if (result != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_decode_connack(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	int result = read_byte(&buf, &mqtt->var_header.connack.connack_flags);
	if (result != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Connect Return Code */
	result = read_byte(&buf, &mqtt->var_header.connack.connack_flags);
	if (result != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.connack.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;
	// Check properties
	property *prop = mqtt->var_header.connack.properties;
	if (prop != NULL) {
		if ((result = check_properties(prop)) != SUCCESS)
			return result;
		// Check Invalid properties
		for (property *p = prop->next; p != NULL; p = p->next) {
			switch (p->id) {
			case PUBLISH_MAXIMUM_QOS:
			case WILDCARD_SUBSCRIPTION_AVAILABLE:
			case SUBSCRIPTION_IDENTIFIER_AVAILABLE:
			case SHARED_SUBSCRIPTION_AVAILABLE:
				if (p->data.p_value.u8 > 1)
					return PROTOCOL_ERROR;
				break;
			case RECEIVE_MAXIMUM:
			case RETAIN_AVAILABLE:
			case MAXIMUM_PACKET_SIZE:
			case ASSIGNED_CLIENT_IDENTIFIER:
			case TOPIC_ALIAS_MAXIMUM:
			case REASON_STRING:
			case USER_PROPERTY:
			case SERVER_KEEP_ALIVE:
			case RESPONSE_INFORMATION:
			case SERVER_REFERENCE:
			case AUTHENTICATION_METHOD:
			case AUTHENTICATION_DATA:
				break;
			default:
				return PROTOCOL_ERROR;
			}
		}
	}

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_subscribe(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	mqtt_subscribe_payload *spld = &mqtt->payload.subscribe;

	/* Packet Identifier */
	ret = read_uint16(&buf, &mqtt->var_header.subscribe.packet_id);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	uint8_t *saved_current_pos = NULL;
	uint16_t temp_length       = 0;
	uint32_t topic_count       = 0;

	/* The loop to determine the number of topic_arr.
	 * TODO: Some other way may be used such as std::vector to collect
	 * topic_arr but there is a question that which is faster
	 */
	/* Save the current position to back */
	saved_current_pos = buf.curpos;
	while (buf.curpos < buf.endpos) {
		ret = read_uint16(&buf, &temp_length);
		/* jump to the end of topic-name */
		buf.curpos += temp_length;
		/* skip QoS field */
		buf.curpos++;
		topic_count++;
	}
	/* Allocate topic_qos array */
	spld->topic_arr =
	    (mqtt_topic_qos *) nni_alloc(sizeof(mqtt_topic_qos) * topic_count);

	/* Set back current position */
	buf.curpos = saved_current_pos;
	while (buf.curpos < buf.endpos) {
		/* Topic Name */
		ret = read_utf8_str(
		    &buf, &spld->topic_arr[spld->topic_count].topic);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		/* QoS */
		ret = read_byte(&buf, &spld->topic_arr[spld->topic_count].qos);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		spld->topic_count++;
	}
	return MQTT_SUCCESS;

err:
	nni_free(spld->topic_arr, sizeof(mqtt_topic_qos) * topic_count);
	return ret;
}

static int
nni_mqttv5_msg_decode_subscribe(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	mqtt_subscribe_payload *spld = &mqtt->payload.subscribe;

	/* Packet Identifier */
	ret = read_uint16(&buf, &mqtt->var_header.subscribe.packet_id);
	if (ret != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.subscribe.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;

	uint8_t *saved_current_pos = NULL;
	uint16_t temp_length       = 0;
	uint32_t topic_count       = 0;

	uint8_t to; // topic option

	/* The loop to determine the number of topic_arr.
	 * TODO: Some other way may be used such as std::vector to collect
	 * topic_arr but there is a question that which is faster
	 */
	/* Save the current position to back */
	saved_current_pos = buf.curpos;
	while (buf.curpos < buf.endpos) {
		ret = read_uint16(&buf, &temp_length);
		/* jump to the end of topic-name */
		buf.curpos += temp_length;
		/* skip QoS field */
		buf.curpos++;
		topic_count++;
	}
	/* Allocate topic_qos array */
	spld->topic_arr =
	    (mqtt_topic_qos *) nni_alloc(sizeof(mqtt_topic_qos) * topic_count);

	/* Set back current position */
	buf.curpos = saved_current_pos;
	while (buf.curpos < buf.endpos) {
		/* Topic Name */
		ret = read_utf8_str(
		    &buf, &spld->topic_arr[spld->topic_count].topic);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		/* QoS */
		ret = read_byte(&buf, &to);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		spld->topic_arr[spld->topic_count].qos             = to & 0x03;
		spld->topic_arr[spld->topic_count].nolocal         = (to >> 2) & 0x01;
		spld->topic_arr[spld->topic_count].rap             = (to >> 3) & 0x01;
		spld->topic_arr[spld->topic_count].retain_handling = (to >> 4) & 0x03;
		spld->topic_count++;
	}
	return MQTT_SUCCESS;

err:
	nni_free(spld->topic_arr, sizeof(mqtt_topic_qos) * topic_count);
	return ret;
}

static int
nni_mqtt_msg_decode_suback(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	ret = read_uint16(&buf, &mqtt->var_header.suback.packet_id);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Suback Return Codes */
	mqtt->payload.suback.ret_code_count = buf.endpos - buf.curpos;

	mqtt->payload.suback.ret_code_arr =
	    (uint8_t *) nni_alloc(mqtt->payload.suback.ret_code_count);
	uint8_t *ptr = mqtt->payload.suback.ret_code_arr;

	for (uint32_t i = 0; i < mqtt->payload.suback.ret_code_count; i++) {
		ret = read_byte(&buf, ptr);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		ptr++;
	}

	return MQTT_SUCCESS;

err:
	nni_free(mqtt->payload.suback.ret_code_arr,
	    mqtt->payload.suback.ret_code_count);
	return ret;
}

static int
nni_mqttv5_msg_decode_suback(nni_msg *msg)
{
	int                  ret;
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	ret = read_uint16(&buf, &mqtt->var_header.suback.packet_id);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.suback.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;

	/* Suback Return Codes */
	mqtt->payload.suback.ret_code_count = buf.endpos - buf.curpos;

	mqtt->payload.suback.ret_code_arr =
	    (uint8_t *) nni_alloc(mqtt->payload.suback.ret_code_count);
	uint8_t *ptr = mqtt->payload.suback.ret_code_arr;

	for (uint32_t i = 0; i < mqtt->payload.suback.ret_code_count; i++) {
		ret = read_byte(&buf, ptr);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		ptr++;
	}

	return MQTT_SUCCESS;

err:
	nni_free(mqtt->payload.suback.ret_code_arr,
	    mqtt->payload.suback.ret_code_count);
	return ret;
}

static int
nni_mqtt_msg_decode_publish(nni_msg *msg)
{
	int                  ret;
	int                  packid_length = 0;
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	/* Topic Name */
	ret = read_utf8_str(&buf, &mqtt->var_header.publish.topic_name);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	if (mqtt->fixed_header.publish.qos > MQTT_QOS_0_AT_MOST_ONCE) {
		/* Packet Identifier */
		ret = read_uint16(&buf, &mqtt->var_header.publish.packet_id);
		if (ret != MQTT_SUCCESS) {
			return MQTT_ERR_PROTOCOL;
		}
		packid_length = 2;
	}

	/* Payload */
	/* No length information for payload. The length of the payload can be
	   calculated by subtracting the length of the variable header from the
	   Remaining Length field that is in the Fixed Header. It is valid for
	   a PUBLISH Packet to contain a zero length payload.*/
	mqtt->payload.publish.payload.length =
	    mqtt->fixed_header.remaining_length -
	    (2 /* Length bytes of Topic Name */ +
	        mqtt->var_header.publish.topic_name.length + packid_length);
	mqtt->payload.publish.payload.buf =
	    (mqtt->payload.publish.payload.length > 0) ? buf.curpos : NULL;

	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_decode_publish(nni_msg *msg)
{
	int                  ret;
	int                  packid_length = 0;
	nni_mqtt_proto_data *mqtt          = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	/* Topic Name */
	ret = read_utf8_str(&buf, &mqtt->var_header.publish.topic_name);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	if (mqtt->fixed_header.publish.qos > MQTT_QOS_0_AT_MOST_ONCE) {
		/* Packet Identifier */
		ret = read_uint16(&buf, &mqtt->var_header.publish.packet_id);
		if (ret != MQTT_SUCCESS) {
			return MQTT_ERR_PROTOCOL;
		}
		packid_length = 2;
	}

	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0, prop_sz;
	uint32_t pos1 = pos;
	mqtt->var_header.publish.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;
	prop_sz = pos - pos1;


	/* Payload */
	/* No length information for payload. The length of the payload can be
	   calculated by subtracting the length of the variable header from the
	   Remaining Length field that is in the Fixed Header. It is valid for
	   a PUBLISH Packet to contain a zero length payload.*/
	mqtt->payload.publish.payload.length =
	    mqtt->fixed_header.remaining_length -
	    (2 /* Length bytes of Topic Name */ + prop_sz +
	        mqtt->var_header.publish.topic_name.length + packid_length);
	mqtt->payload.publish.payload.buf =
	    (mqtt->payload.publish.payload.length > 0) ? buf.curpos : NULL;

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_base_with_packet_id(nni_msg *msg, uint16_t *packet_id)
{
	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	int result = read_uint16(&buf, packet_id);
	if (result != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	return MQTT_SUCCESS;
}

/*
static int
nni_mqttv5_msg_decode_base_with_packet_id(nni_msg *msg, uint16_t *packet_id)
{
	NNI_ARG_UNUSED(msg);
	NNI_ARG_UNUSED(packet_id);
	return 0;
}
*/

static int
nni_mqtt_msg_decode_puback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	return nni_mqtt_msg_decode_base_with_packet_id(
	    msg, &mqtt->var_header.puback.packet_id);
}

static int
nni_mqttv5_msg_decode_puback(nni_msg *msg)
{
	int                  rv;
	nni_mqtt_proto_data *mqtt   = nni_msg_get_proto_data(msg);
	uint8_t             *body   = nni_msg_body(msg);
	size_t               length = nni_msg_len(msg);

	struct pos_buf buf = { .curpos = &body[0], .endpos = &body[length] };
	if ((rv = read_uint16(&buf, &mqtt->var_header.puback.packet_id)) !=
	    MQTT_SUCCESS) {
		return rv;
	}
	if (length <= 2) {
		mqtt->var_header.puback.code       = SUCCESS;
		mqtt->var_header.puback.properties = NULL;
		return MQTT_SUCCESS;
	}
	if ((rv = read_byte(&buf, (uint8_t *)&mqtt->var_header.puback.code)) !=
	    MQTT_SUCCESS) {
		return rv;
	}

	if ((buf.endpos - buf.curpos) <= 0) {
		mqtt->var_header.puback.properties = NULL;
		return MQTT_SUCCESS;
	}

	uint32_t pos      = (uint32_t) (buf.curpos - body);
	uint32_t prop_len = 0;

	mqtt->var_header.puback.properties =
	    decode_properties(msg, &pos, &prop_len, false);
	if (check_properties(mqtt->var_header.puback.properties) != SUCCESS) {
		property_free(mqtt->var_header.puback.properties);
		return PROTOCOL_ERROR;
	}

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_pubrec(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	return nni_mqtt_msg_decode_base_with_packet_id(
	    msg, &mqtt->var_header.pubrec.packet_id);
}

static int
nni_mqttv5_msg_decode_pubrec(nni_msg *msg)
{
	int                  rv;
	nni_mqtt_proto_data *mqtt   = nni_msg_get_proto_data(msg);
	uint8_t             *body   = nni_msg_body(msg);
	size_t               length = nni_msg_len(msg);

	struct pos_buf buf = { .curpos = &body[0], .endpos = &body[length] };
	if ((rv = read_uint16(&buf, &mqtt->var_header.pubrec.packet_id)) !=
	    MQTT_SUCCESS) {
		return rv;
	}
	if (length <= 2) {
		mqtt->var_header.pubrec.code       = SUCCESS;
		mqtt->var_header.pubrec.properties = NULL;
		return MQTT_SUCCESS;
	}
	if ((rv = read_byte(&buf, (uint8_t *)&mqtt->var_header.pubrec.code)) !=
	    MQTT_SUCCESS) {
		return rv;
	}

	if ((buf.endpos - buf.curpos) <= 0) {
		mqtt->var_header.pubrec.properties = NULL;
		return MQTT_SUCCESS;
	}

	uint32_t pos      = (uint32_t) (buf.curpos - body);
	uint32_t prop_len = 0;

	mqtt->var_header.pubrec.properties =
	    decode_properties(msg, &pos, &prop_len, false);
	if (check_properties(mqtt->var_header.pubrec.properties) != SUCCESS) {
		property_free(mqtt->var_header.pubrec.properties);
		return PROTOCOL_ERROR;
	}

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_pubrel(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	if (mqtt->fixed_header.common.bit_0 != 0 ||
	    mqtt->fixed_header.common.bit_1 != 1 ||
	    mqtt->fixed_header.common.bit_2 != 0 ||
	    mqtt->fixed_header.common.bit_3 != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	return nni_mqtt_msg_decode_base_with_packet_id(
	    msg, &mqtt->var_header.pubrel.packet_id);
}

static int
nni_mqttv5_msg_decode_pubrel(nni_msg *msg)
{
	int                  rv;
	nni_mqtt_proto_data *mqtt   = nni_msg_get_proto_data(msg);
	uint8_t             *body   = nni_msg_body(msg);
	size_t               length = nni_msg_len(msg);

	struct pos_buf buf = { .curpos = &body[0], .endpos = &body[length] };
	if ((rv = read_uint16(&buf, &mqtt->var_header.pubrel.packet_id)) !=
	    MQTT_SUCCESS) {
		return rv;
	}
	if (length <= 2) {
		mqtt->var_header.pubrel.code       = SUCCESS;
		mqtt->var_header.pubrel.properties = NULL;
		return MQTT_SUCCESS;
	}
	if ((rv = read_byte(&buf, (uint8_t *)&mqtt->var_header.pubrel.code)) !=
	    MQTT_SUCCESS) {
		return rv;
	}

	if ((buf.endpos - buf.curpos) <= 0) {
		mqtt->var_header.pubrel.properties = NULL;
		return MQTT_SUCCESS;
	}

	uint32_t pos      = (uint32_t) (buf.curpos - body);
	uint32_t prop_len = 0;

	mqtt->var_header.pubrel.properties =
	    decode_properties(msg, &pos, &prop_len, false);
	if (check_properties(mqtt->var_header.pubrel.properties) != SUCCESS) {
		property_free(mqtt->var_header.pubrel.properties);
		return PROTOCOL_ERROR;
	}

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_pubcomp(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	return nni_mqtt_msg_decode_base_with_packet_id(
	    msg, &mqtt->var_header.pubcomp.packet_id);
}

static int
nni_mqttv5_msg_decode_pubcomp(nni_msg *msg)
{
	int                  rv;
	nni_mqtt_proto_data *mqtt   = nni_msg_get_proto_data(msg);
	uint8_t             *body   = nni_msg_body(msg);
	size_t               length = nni_msg_len(msg);

	struct pos_buf buf = { .curpos = &body[0], .endpos = &body[length] };
	if ((rv = read_uint16(&buf, &mqtt->var_header.pubcomp.packet_id)) !=
	    MQTT_SUCCESS) {
		return rv;
	}
	if (length <= 2) {
		mqtt->var_header.pubcomp.code       = SUCCESS;
		mqtt->var_header.pubcomp.properties = NULL;
		return MQTT_SUCCESS;
	}
	if ((rv = read_byte(&buf, (uint8_t *)&mqtt->var_header.pubcomp.code)) !=
	    MQTT_SUCCESS) {
		return rv;
	}

	if ((buf.endpos - buf.curpos) <= 0) {
		mqtt->var_header.pubcomp.properties = NULL;
		return MQTT_SUCCESS;
	}

	uint32_t pos      = (uint32_t) (buf.curpos - body);
	uint32_t prop_len = 0;

	mqtt->var_header.pubcomp.properties =
	    decode_properties(msg, &pos, &prop_len, false);
	if (check_properties(mqtt->var_header.pubcomp.properties) != SUCCESS) {
		property_free(mqtt->var_header.pubcomp.properties);
		return PROTOCOL_ERROR;
	}

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_unsubscribe(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	if (mqtt->fixed_header.common.bit_0 != 0 ||
	    mqtt->fixed_header.common.bit_1 != 1 ||
	    mqtt->fixed_header.common.bit_2 != 0 ||
	    mqtt->fixed_header.common.bit_3 != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos                      = &body[0];
	buf.endpos                      = &body[length];
	mqtt_unsubscribe_payload *uspld = &mqtt->payload.unsubscribe;

	int ret = read_uint16(&buf, &mqtt->var_header.unsubscribe.packet_id);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	uint8_t *saved_current_pos = NULL;
	uint16_t temp_length       = 0;
	uint32_t topic_count       = 0;

	saved_current_pos = buf.curpos;
	while (buf.curpos < buf.endpos) {
		ret = read_uint16(&buf, &temp_length);
		/* jump to the end of topic-name */
		buf.curpos += temp_length;
		/* skip QoS field */
		topic_count++;
	}

	/* Allocate topic array */
	uspld->topic_arr =
	    (mqtt_buf *) nni_alloc(topic_count * sizeof(mqtt_buf));

	/* Set back current position */
	buf.curpos = saved_current_pos;
	while (buf.curpos < buf.endpos) {
		/* Topic Name */
		ret =
		    read_utf8_str(&buf, &uspld->topic_arr[uspld->topic_count]);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		uspld->topic_count++;
	}
	return MQTT_SUCCESS;

err:
	nni_free(uspld->topic_arr, topic_count * sizeof(mqtt_buf));

	return ret;
}

static int
nni_mqttv5_msg_decode_unsubscribe(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	if (mqtt->fixed_header.common.bit_0 != 0 ||
	    mqtt->fixed_header.common.bit_1 != 1 ||
	    mqtt->fixed_header.common.bit_2 != 0 ||
	    mqtt->fixed_header.common.bit_3 != 0) {
		return MQTT_ERR_PROTOCOL;
	}

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos                      = &body[0];
	buf.endpos                      = &body[length];
	mqtt_unsubscribe_payload *uspld = &mqtt->payload.unsubscribe;

	int ret = read_uint16(&buf, &mqtt->var_header.unsubscribe.packet_id);
	if (ret != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.unsubscribe.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;

	uint8_t *saved_current_pos = NULL;
	uint16_t temp_length       = 0;
	uint32_t topic_count       = 0;

	saved_current_pos = buf.curpos;
	while (buf.curpos < buf.endpos) {
		ret = read_uint16(&buf, &temp_length);
		/* jump to the end of topic-name */
		buf.curpos += temp_length;
		/* skip QoS field */
		topic_count++;
	}

	/* Allocate topic array */
	uspld->topic_arr =
	    (mqtt_buf *) nni_alloc(topic_count * sizeof(mqtt_buf));

	/* Set back current position */
	buf.curpos = saved_current_pos;
	while (buf.curpos < buf.endpos) {
		/* Topic Name */
		ret =
		    read_utf8_str(&buf, &uspld->topic_arr[uspld->topic_count]);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			goto err;
		}
		uspld->topic_count++;
	}
	return MQTT_SUCCESS;

err:
	nni_free(uspld->topic_arr, topic_count * sizeof(mqtt_buf));

	return ret;
}

static int
nni_mqtt_msg_decode_unsuback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	return nni_mqtt_msg_decode_base_with_packet_id(
	    msg, &mqtt->var_header.unsuback.packet_id);
}

static int
nni_mqttv5_msg_decode_unsuback(nni_msg *msg)
{
	nni_mqtt_proto_data *mqtt = nni_msg_get_proto_data(msg);

	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf;
	buf.curpos = &body[0];
	buf.endpos = &body[length];

	int result = read_uint16(&buf, &mqtt->var_header.unsubscribe.packet_id);
	if (result != MQTT_SUCCESS) {
		return MQTT_ERR_PROTOCOL;
	}

	/* Properties */
	uint32_t pos = buf.curpos - &body[0];
	uint32_t prop_len = 0;
	mqtt->var_header.unsuback.properties =
	    decode_buf_properties(body, length, &pos, &prop_len, true);
	buf.curpos = &body[0] + pos;

	mqtt->payload.unsuback.ret_code_count = length - pos;
	mqtt->payload.unsuback.ret_code_arr =
	    (uint8_t *) nni_alloc(mqtt->payload.unsuback.ret_code_count);
	uint8_t *ptr = mqtt->payload.unsuback.ret_code_arr;

	for (uint32_t i = 0; i < mqtt->payload.unsuback.ret_code_count; i++) {
		int ret = read_byte(&buf, ptr);
		if (ret != MQTT_SUCCESS) {
			ret = MQTT_ERR_PROTOCOL;
			return ret;
		}
		ptr++;
	}

	return MQTT_SUCCESS;
}

static int
nni_mqtt_msg_decode_base(nni_msg *msg)
{
	NNI_ARG_UNUSED(msg);
	return MQTT_SUCCESS;
}

static int
nni_mqttv5_msg_decode_base(nni_msg *msg)
{
	NNI_ARG_UNUSED(msg);
	return 0;
}

int
byte_number_for_variable_length(uint32_t variable)
{
	if (variable < 128) {
		return 1;
	} else if (variable < 16384) {
		return 2;
	} else if (variable < 2097152) {
		return 3;
	} else if (variable < 268435456) {
		return 4;
	}
	return 5;
}

int
write_variable_length_value(uint32_t value, struct pos_buf *buf)
{
	uint8_t byte;
	int     count = 0;

	do {
		byte  = value % 128;
		value = value / 128;
		/* If there are more digits to encode, set the top bit of this
		 * digit */
		if (value > 0) {
			byte = byte | 0x80;
		}
		*(buf->curpos++) = byte;
		count++;
	} while (value > 0 && count < 5);

	if (count == 5) {
		return -1;
	}
	return count;
}

int
write_byte(uint8_t val, struct pos_buf *buf)
{
	if ((buf->endpos - buf->curpos) < 1) {
		return MQTT_ERR_NOMEM;
	}

	*(buf->curpos++) = val;

	return 0;
}

int
write_uint16(uint16_t value, struct pos_buf *buf)
{
	if ((buf->endpos - buf->curpos) < (long) sizeof(uint16_t)) {
		return MQTT_ERR_NOMEM;
	}

	NNI_PUT16(buf->curpos, value);
	buf->curpos += sizeof(uint16_t);

	return 0;
}

int
write_uint32(uint32_t value, struct pos_buf *buf)
{
	if ((buf->endpos - buf->curpos) < (long) sizeof(uint32_t)) {
		return MQTT_ERR_NOMEM;
	}

	NNI_PUT32(buf->curpos, value);
	buf->curpos += sizeof(uint32_t);

	return 0;
}

int
write_uint64(uint64_t value, struct pos_buf *buf)
{
	if ((buf->endpos - buf->curpos) < (long) sizeof(uint64_t)) {
		return MQTT_ERR_NOMEM;
	}

	NNI_PUT64(buf->curpos, value);
	buf->curpos += sizeof(uint64_t);

	return 0;
}

int
write_bytes(uint8_t *bytes, size_t len, struct pos_buf *buf)
{
	if ((buf->endpos - buf->curpos) < (long) len) {
		return MQTT_ERR_NOMEM;
	}

	memcpy(buf->curpos, bytes, len);
	buf->curpos += len;

	return 0;
}

int
write_byte_string(mqtt_buf *str, struct pos_buf *buf)
{
	if ((buf->endpos - buf->curpos) < (str->length + 2)) {
		return MQTT_ERR_NOMEM;
	}
	write_uint16(str->length, buf);

	memcpy(buf->curpos, str->buf, str->length);
	str->buf = buf->curpos; /* reset data position to indicate data in raw
	                           data block */
	buf->curpos += str->length;

	return 0;
}

int
read_byte(struct pos_buf *buf, uint8_t *val)
{
	if ((buf->endpos - buf->curpos) < 1) {
		return MQTT_ERR_NOMEM;
	}

	*val = *(buf->curpos++);

	return 0;
}


int
read_uint16(struct pos_buf *buf, uint16_t *val)
{
	if ((buf->endpos - buf->curpos) < (long) sizeof(uint16_t)) {
		return MQTT_ERR_INVAL;
	}

	NNI_GET16(buf->curpos, *val);
	buf->curpos += 2;

	return 0;
}

int
read_uint32(struct pos_buf *buf, uint32_t *val)
{
	if ((buf->endpos - buf->curpos) < (long) sizeof(uint32_t)) {
		return MQTT_ERR_INVAL;
	}

	NNI_GET32(buf->curpos, *val);
	buf->curpos += sizeof(uint32_t);

	return 0;
}

int
read_uint64(struct pos_buf *buf, uint64_t *val)
{
	if ((buf->endpos - buf->curpos) < (long) sizeof(uint64_t)) {
		return MQTT_ERR_INVAL;
	}

	NNI_GET64(buf->curpos, *val);
	buf->curpos += sizeof(uint64_t);

	return 0;
}

int
read_bytes(struct pos_buf *buf, uint8_t **bytes, size_t len)
{
	if ((size_t) (buf->endpos - buf->curpos) < len) {
		return MQTT_ERR_INVAL;
	}

	*bytes = buf->curpos;
	buf->curpos += len;
	return 0;
}

int
read_utf8_str(struct pos_buf *buf, mqtt_buf *val)
{
	uint16_t length = 0;
	int      ret    = read_uint16(buf, &length);
	if (ret != 0) {
		return ret;
	}
	if ((buf->endpos - buf->curpos) < length) {
		return MQTT_ERR_INVAL;
	}

	val->length = length;
	/* Zero length UTF8 strings are permitted. */
	if (length > 0) {
		val->buf = buf->curpos;
		buf->curpos += length;
	} else {
		val->buf = NULL;
	}
	return 0;
}

int
read_str_data(struct pos_buf *buf, mqtt_buf *val)
{
	uint16_t length = 0;
	int      ret    = read_uint16(buf, &length);
	if (ret != 0) {
		return ret;
	}
	if ((buf->endpos - buf->curpos) < length) {
		return MQTT_ERR_INVAL;
	}

	val->length = length;
	if (length > 0) {
		val->buf = buf->curpos;
		buf->curpos += length;
	} else {
		val->buf = NULL;
	}
	return 0;
}
// TODO unite with get_var_integer
static int
read_variable_int(
    uint8_t *ptr, uint32_t length, uint32_t *value, uint8_t *used_bytes)
{
	int      i;
	uint8_t  byte;
	int      multiplier = 1;
	int32_t  lword      = 0;
	uint8_t  lbytes     = 0;
	uint8_t *start      = ptr;

	if (!ptr) {
		return 0;
	}
	for (i = 0; i < 4; i++) {
		if ((ptr - start + 1) > length) {
			return MQTT_ERR_PAYLOAD_SIZE;
		}
		lbytes++;
		byte = ptr[0];
		lword += (byte & 127) * multiplier;
		multiplier *= 128;
		ptr++;
		if ((byte & 128) == 0) {
			if (lbytes > 1 && byte == 0) {
				/* Catch overlong encodings */
				return MQTT_ERR_INVAL;
			} else {
				*value = lword;
				if (used_bytes) {
					*used_bytes = lbytes;
				}
				return MQTT_SUCCESS;
			}
		}
	}
	return MQTT_ERR_INVAL;
}

int
read_variable_integer(struct pos_buf *buf, uint32_t *integer)
{
	int     rv;
	uint8_t bytes = 0;
	if ((rv = read_variable_int(buf->curpos, buf->endpos - buf->curpos,
	              integer, &bytes) != MQTT_SUCCESS)) {
		return rv;
	}
	buf->curpos += bytes;
	return MQTT_SUCCESS;
}

int
read_packet_length(struct pos_buf *buf, uint32_t *length)
{
	uint8_t  shift = 0;
	uint32_t bytes = 0;

	*length = 0;
	do {
		if (bytes >= MQTT_MAX_MSG_LEN) {
			return MQTT_ERR_INVAL;
		}

		if (buf->curpos >= buf->endpos) {
			return MQTT_ERR_MALFORMED;
		}

		*length +=
		    ((uint32_t) * (buf->curpos) & MQTT_LENGTH_VALUE_MASK)
		    << shift;
		shift += MQTT_LENGTH_SHIFT;
		bytes++;
	} while ((*(buf->curpos++) & MQTT_LENGTH_CONTINUATION_BIT) != 0U);

	if (*length > MQTT_MAX_MSG_LEN) {
		return MQTT_ERR_INVAL;
	}

	return 0;
}

int
mqtt_get_remaining_length(uint8_t *packet, uint32_t len,
    uint32_t *remainning_length, uint8_t *used_bytes)
{
	int      multiplier = 1;
	int32_t  lword      = 0;
	uint8_t  lbytes     = 0;
	uint8_t *ptr        = packet + 1;
	uint8_t *start      = ptr;

	for (size_t i = 0; i < 4; i++) {
		if ((size_t) (ptr - start + 1) > len) {
			return MQTT_ERR_PAYLOAD_SIZE;
		}
		lbytes++;
		uint8_t byte = ptr[0];
		lword += (byte & 127) * multiplier;
		multiplier *= 128;
		ptr++;
		if ((byte & 128) == 0) {
			if (lbytes > 1 && byte == 0) {
				return MQTT_ERR_INVAL;
			} else {
				*remainning_length = lword;
				if (used_bytes) {
					*used_bytes = lbytes;
				}
				return MQTT_SUCCESS;
			}
		}
	}

	return MQTT_ERR_INVAL;
}

int
mqtt_buf_create(mqtt_buf *mbuf, const uint8_t *buf, uint32_t length)
{
	void *new = NULL;
	if ((new = nni_alloc(length)) != NULL) {
		free(mbuf->buf);
		memcpy(new, buf, length);
		mbuf->length = length;
		mbuf->buf = new;
		return (0);
	}
	return NNG_ENOMEM;
}

int
mqtt_buf_dup(mqtt_buf *dest, const mqtt_buf *src)
{
	if (src->length <= 0) {
		return 0;
	}
	if ((dest->buf = nni_alloc(src->length)) != NULL) {
		dest->length = src->length;
		memcpy(dest->buf, src->buf, src->length);
		return (0);
	}
	return NNG_ENOMEM;
}

void
mqtt_buf_free(mqtt_buf *buf)
{
	if (buf->length > 0) {
		nni_free(buf->buf, buf->length);
		buf->length = 0;
		buf->buf    = NULL;
	}
}

int
mqtt_kv_create(mqtt_kv *kv, const char *key, size_t key_len, const char *value,
    size_t value_len)
{
	int rv;
	if (((rv = mqtt_buf_create(&kv->key, (uint8_t *) key, key_len)) !=
	        0) ||
	    ((rv = mqtt_buf_create(
	          &kv->value, (uint8_t *) value, value_len)) != 0)) {
		return rv;
	}
	return 0;
}

int
mqtt_kv_dup(mqtt_kv *dest, const mqtt_kv *src)
{
	int rv;
	if (((rv = mqtt_buf_dup(&dest->key, &src->key)) != 0) ||
	    ((rv = mqtt_buf_dup(&dest->value, &src->value)) != 0)) {
		return rv;
	}
	return 0;
}

void
mqtt_kv_free(mqtt_kv *kv)
{
	mqtt_buf_free(&kv->key);
	mqtt_buf_free(&kv->value);
}

static mqtt_msg *
mqtt_msg_create_empty(void)
{
	mqtt_msg *msg = (mqtt_msg *) malloc(sizeof(mqtt_msg));
	memset((char *) msg, 0, sizeof(mqtt_msg));

	return msg;
}

mqtt_msg *
mqtt_msg_create(nni_mqtt_packet_type packet_type)
{
	mqtt_msg *msg                        = mqtt_msg_create_empty();
	msg->fixed_header.common.packet_type = packet_type;

	return msg;
}

int
mqtt_msg_destroy(mqtt_msg *self)
{
	free(self);

	return 0;
}

const char *
get_packet_type_str(nni_mqtt_packet_type packtype)
{
	static const char *packTypeNames[16] = { "Forbidden-0", "CONNECT",
		"CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL", "PUBCOMP",
		"SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK", "PINGREQ",
		"PINGRESP", "DISCONNECT", "Forbidden-15" };
	if (packtype > 15) {
		packtype = 0;
	}
	return packTypeNames[packtype];
}

int
mqtt_msg_dump(mqtt_msg *msg, mqtt_buf *buf, mqtt_buf *packet, bool print_bytes)
{
	uint32_t pos = 0;
	int      ret = 0;

	size_t i = 0;

	ret = sprintf((char *) &buf->buf[pos],
	    "\n----- mqtt message dump  -----\n"
	    "packet type        :   %d (%s)\n"
	    "packet flags       :   |%d|%d|%d|%d|\n"
	    "remaining length   :   %d (%d bytes)\n",
	    msg->fixed_header.common.packet_type,
	    get_packet_type_str(msg->fixed_header.common.packet_type),
	    msg->fixed_header.common.bit_3, msg->fixed_header.common.bit_2,
	    msg->fixed_header.common.bit_1, msg->fixed_header.common.bit_0,
	    (int) msg->fixed_header.remaining_length, msg->used_bytes);
	if ((ret < 0) || ((pos + ret) > buf->length)) {
		return 1;
	}
	pos += ret;

	/* Print variable header part */
	switch (msg->fixed_header.common.packet_type) {
	case NNG_MQTT_CONNECT: {
		ret = sprintf((char *) &buf->buf[pos],
		    "protocol name      :   %.*s\n"
		    "protocol version   :   %d\n"
		    "keep alive         :   %d\n",
		    msg->var_header.connect.protocol_name.length,
		    msg->var_header.connect.protocol_name.buf,
		    (int) msg->var_header.connect.protocol_version,
		    (int) msg->var_header.connect.keep_alive);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		conn_flags flags_set = msg->var_header.connect.conn_flags;

		ret = sprintf((char *) &buf->buf[pos],
		    "connect flags:\n"
		    "   clean session flag : %s\n"
		    "   will flag          : %s\n"
		    "   will retain flag   : %s\n"
		    "   will qos flag      : %d\n"
		    "   user name flag     : %s\n"
		    "   password flag      : %s\n",
		    ((flags_set.clean_session) ? "true" : "false"),
		    ((flags_set.will_flag) ? "true" : "false"),
		    ((flags_set.will_retain) ? "true" : "false"),
		    flags_set.will_qos,
		    ((flags_set.username_flag) ? "true" : "false"),
		    ((flags_set.password_flag) ? "true" : "false"));
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		ret = sprintf((char *) &buf->buf[pos],
		    "client id             : %.*s\n",
		    msg->payload.connect.client_id.length,
		    msg->payload.connect.client_id.buf);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		ret = sprintf((char *) &buf->buf[pos],
		    "will topic            : %.*s\n",
		    msg->payload.connect.will_topic.length,
		    msg->payload.connect.will_topic.buf);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		ret = sprintf((char *) &buf->buf[pos],
		    "will message          : %.*s\n",
		    msg->payload.connect.will_msg.length,
		    msg->payload.connect.will_msg.buf);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		ret = sprintf((char *) &buf->buf[pos],
		    "user name             : %.*s\n",
		    msg->payload.connect.user_name.length,
		    msg->payload.connect.user_name.buf);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		ret = sprintf((char *) &buf->buf[pos],
		    "password              : %.*s\n",
		    msg->payload.connect.password.length,
		    msg->payload.connect.password.buf);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
	} break;

	case NNG_MQTT_CONNACK:
		ret = sprintf((char *) &buf->buf[pos],
		    "connack flags      : %d\n"
		    "connack return-code: %d\n",
		    (int) msg->var_header.connack.connack_flags,
		    (int) msg->var_header.connack.conn_return_code);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		break;

	case NNG_MQTT_PUBLISH: {

		ret = sprintf((char *) &buf->buf[pos],
		    "publis flags:\n"
		    "   retain   : %s\n"
		    "   qos      : %d\n"
		    "   dup      : %s\n",
		    ((msg->fixed_header.publish.retain) ? "true" : "false"),
		    msg->fixed_header.publish.qos,
		    ((msg->fixed_header.publish.dup) ? "true" : "false"));
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		ret = sprintf((char *) &buf->buf[pos],
		    "topic       : %.*s\n"
		    "packet id   : %d\n"
		    "payload     : %.*s\n",
		    msg->var_header.publish.topic_name.length,
		    msg->var_header.publish.topic_name.buf,
		    (int) msg->var_header.publish.packet_id,
		    msg->payload.publish.payload.length,
		    msg->payload.publish.payload.buf);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
	} break;

	case NNG_MQTT_PUBACK:
		ret = sprintf((char *) &buf->buf[pos], "packet-id: %d\n",
		    msg->var_header.puback.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		break;

	case NNG_MQTT_PUBREC:
		ret = sprintf((char *) &buf->buf[pos], "packet-id: %d\n",
		    msg->var_header.pubrec.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		break;

	case NNG_MQTT_PUBREL:
		ret = sprintf((char *) &buf->buf[pos], "packet-id: %d\n",
		    msg->var_header.pubrel.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		break;

	case NNG_MQTT_PUBCOMP:
		ret = sprintf((char *) &buf->buf[pos], "packet-id: %d\n",
		    msg->var_header.pubcomp.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		break;

	case NNG_MQTT_SUBSCRIBE: {
		ret = sprintf((char *) &buf->buf[pos],
		    "packet-id          :   %d\n",
		    msg->var_header.subscribe.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		for (uint32_t i = 0; i < msg->payload.subscribe.topic_count;
		     i++) {
			ret = sprintf((char *) &buf->buf[pos],
			    "topic       [%u]    :   %.*s\n"
			    "requested qos[%u]   :   %d\n",
			    i,
			    msg->payload.subscribe.topic_arr[i].topic.length,
			    msg->payload.subscribe.topic_arr[i].topic.buf, i,
			    (int) msg->payload.subscribe.topic_arr[i].qos);
			if ((ret < 0) || ((pos + ret) > buf->length)) {
				return 1;
			}
			pos += ret;
		}
	} break;

	case NNG_MQTT_SUBACK: {
		ret = sprintf((char *) &buf->buf[pos],
		    "packet-id          :   %d\n",
		    msg->var_header.suback.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		for (uint32_t i = 0; i < msg->payload.suback.ret_code_count;
		     i++) {
			ret = sprintf((char *) &buf->buf[pos],
			    "return code[%u]: %d\n", i,
			    (int) msg->payload.suback.ret_code_arr[i]);
			if ((ret < 0) || ((pos + ret) > buf->length)) {
				return 1;
			}
			pos += ret;
		}
	} break;

	case NNG_MQTT_UNSUBSCRIBE: {
		ret = sprintf((char *) &buf->buf[pos],
		    "packet-id          : %d\n",
		    msg->var_header.unsubscribe.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		for (i = 0; i < msg->payload.unsubscribe.topic_count; i++) {
			ret = sprintf((char *) &buf->buf[pos],
			    "topic       [%lu] :  %.*s\n", i,
			    msg->payload.unsubscribe.topic_arr[i].length,
			    (char *) msg->payload.unsubscribe.topic_arr[i]
			        .buf);
			if ((ret < 0) || ((pos + ret) > buf->length)) {
				return 1;
			}
			pos += ret;
		}
	} break;

	case NNG_MQTT_UNSUBACK:
		ret = sprintf((char *) &buf->buf[pos],
		    "packet-id          : %d\n",
		    msg->var_header.unsuback.packet_id);
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		break;

	case NNG_MQTT_PINGREQ:
	case NNG_MQTT_PINGRESP:
		break;

	case NNG_MQTT_DISCONNECT:
		break;

	case NNG_MQTT_AUTH:
		break;
	}

	if (print_bytes) {
		ret = sprintf((char *) &buf->buf[pos], "raw message: ");
		if ((ret < 0) || ((pos + ret) > buf->length)) {
			return 1;
		}
		pos += ret;
		for (i = 0; i < packet->length; i++) {
			ret = sprintf((char *) &buf->buf[pos], "%02x ",
			    ((uint8_t) (packet->buf[i] & 0xff)));
			if ((ret < 0) || ((pos + ret) > buf->length)) {
				return 1;
			}
			pos += ret;
		}
		buf->buf[pos++] = '\n';
		if (pos > packet->length) {
			return 1;
		}
		sprintf((char *) &buf->buf[pos], "------------------------\n");
	}
	return 0;
}


property *
property_alloc(void)
{
	property *p = nni_zalloc(sizeof(property));
	p->next     = NULL;
	return p;
}

property_type_enum
property_get_value_type(uint8_t prop_id)
{
	property_type_enum value_type = UNKNOWN;
	switch (prop_id) {
	case PAYLOAD_FORMAT_INDICATOR:
	case REQUEST_PROBLEM_INFORMATION:
	case REQUEST_RESPONSE_INFORMATION:
	case PUBLISH_MAXIMUM_QOS:
	case RETAIN_AVAILABLE:
	case WILDCARD_SUBSCRIPTION_AVAILABLE:
	case SUBSCRIPTION_IDENTIFIER_AVAILABLE:
	case SHARED_SUBSCRIPTION_AVAILABLE:
		value_type = U8;
		break;

	case SERVER_KEEP_ALIVE:
	case RECEIVE_MAXIMUM:
	case TOPIC_ALIAS_MAXIMUM:
	case TOPIC_ALIAS:
		value_type = U16;
		break;
	case MESSAGE_EXPIRY_INTERVAL:
	case SESSION_EXPIRY_INTERVAL:
	case WILL_DELAY_INTERVAL:
	case MAXIMUM_PACKET_SIZE:
		value_type = U32;
		break;
	case SUBSCRIPTION_IDENTIFIER:
		value_type = VARINT;
		break;
	case CONTENT_TYPE:
	case RESPONSE_TOPIC:
	case ASSIGNED_CLIENT_IDENTIFIER:
	case AUTHENTICATION_METHOD:
	case RESPONSE_INFORMATION:
	case SERVER_REFERENCE:
	case REASON_STRING:
		value_type = STR;
		break;
	case CORRELATION_DATA:
	case AUTHENTICATION_DATA:
		value_type = BINARY;
		break;
	case USER_PROPERTY:
		value_type = STR_PAIR;
		break;
	}
	return value_type;
}

property *
property_set_value_u8(uint8_t prop_id, uint8_t value)
{
	property *prop        = property_alloc();
	prop->id              = prop_id;
	prop->data.p_value.u8 = value;
	prop->data.p_type     = U8;
	return prop;
}

property *
property_set_value_u16(uint8_t prop_id, uint16_t value)
{
	property *prop         = property_alloc();
	prop->id               = prop_id;
	prop->data.p_value.u16 = value;
	prop->data.p_type      = U16;
	return prop;
}

property *
property_set_value_u32(uint8_t prop_id, uint32_t value)
{
	property *prop         = property_alloc();
	prop->id               = prop_id;
	prop->data.p_value.u32 = value;
	prop->data.p_type      = U32;
	return prop;
}

property *
property_set_value_varint(uint8_t prop_id, uint32_t value)
{
	property *prop            = property_alloc();
	prop->id                  = prop_id;
	prop->data.p_value.varint = value;
	prop->data.p_type         = VARINT;
	return prop;
}

property *
property_set_value_binary(
    uint8_t prop_id, uint8_t *value, uint32_t len, bool copy_value)
{
	property *prop     = property_alloc();
	prop->id           = prop_id;
	prop->data.is_copy = copy_value;
	if (copy_value) {
		mqtt_buf_create(&prop->data.p_value.binary, value, len);
	} else {
		prop->data.p_value.binary.buf    = (uint8_t *) value;
		prop->data.p_value.binary.length = len;
	}
	prop->data.p_type = BINARY;
	return prop;
}

property *
property_set_value_str(
    uint8_t prop_id, const char *value, uint32_t len, bool copy_value)
{
	property *prop     = property_alloc();
	prop->id           = prop_id;
	prop->data.is_copy = copy_value;
	if (copy_value) {
		mqtt_buf_create(
		    &prop->data.p_value.str, (uint8_t *) value, len);
	} else {
		prop->data.p_value.str.buf    = (uint8_t *) value;
		prop->data.p_value.str.length = len;
	}
	prop->data.p_type = STR;
	return prop;
}

property *
property_set_value_strpair(uint8_t prop_id, const char *key, uint32_t key_len,
    const char *value, uint32_t value_len, bool copy_value)
{
	property *prop     = property_alloc();
	prop->id           = prop_id;
	prop->data.is_copy = copy_value;
	if (copy_value) {
		mqtt_kv_create(&prop->data.p_value.strpair, key, key_len,
		    value, value_len);
	} else {
		prop->data.p_value.strpair.key.buf      = (uint8_t *) key;
		prop->data.p_value.strpair.key.length   = key_len;
		prop->data.p_value.strpair.value.buf    = (uint8_t *) value;
		prop->data.p_value.strpair.value.length = value_len;
	}
	prop->data.p_type = STR_PAIR;
	return prop;
}

property *
property_parse(struct pos_buf *buf, property *prop, uint8_t prop_id,
    property_type_enum type, bool copy_value)
{
	if (prop == NULL) {
		prop = property_alloc();
	}
	prop->next         = NULL;
	prop->data.p_type  = type;
	prop->data.is_copy = copy_value;
	prop->id           = prop_id;
	switch (type) {
	case U8:
		read_byte(buf, &prop->data.p_value.u8);
		break;
	case U16:
		read_uint16(buf, &prop->data.p_value.u16);
		break;
	case U32:
		read_uint32(buf, &prop->data.p_value.u32);
		break;
	case VARINT:
		read_variable_integer(buf, &prop->data.p_value.varint);
		break;
	case BINARY:
		if (copy_value) {
			mqtt_buf binary = { 0 };
			read_utf8_str(buf, &binary);
			mqtt_buf_dup(&prop->data.p_value.binary, &binary);
		} else {
			read_utf8_str(buf, &prop->data.p_value.binary);
		}
		break;
	case STR:
		if (copy_value) {
			mqtt_buf str = { 0 };
			read_utf8_str(buf, &str);
			mqtt_buf_dup(&prop->data.p_value.binary, &str);
		} else {
			read_utf8_str(buf, &prop->data.p_value.str);
		}
		break;
	case STR_PAIR:
		if (copy_value) {
			mqtt_kv kv = { 0 };
			read_utf8_str(buf, &kv.key);
			read_utf8_str(buf, &kv.value);
			mqtt_kv_dup(&prop->data.p_value.strpair, &kv);
		} else {
			read_utf8_str(buf, &prop->data.p_value.strpair.key);
			read_utf8_str(buf, &prop->data.p_value.strpair.value);
		}
		break;

	default:
		break;
	}

	return prop;
}

void
property_append(property *prop_list, property *last)
{
	property *p = prop_list;
	while (p) {
		if (p->next == NULL) {
			p->next    = last;
			last->next = NULL;
			break;
		}
		p = p->next;
	}
}

void
property_remove(property *prop_list, uint8_t prop_id)
{
	for (property *p = prop_list; p != NULL; p = p->next) {
		if (p->next != NULL) {
			property *p_temp = p->next;
			if (prop_id == p_temp->id) {
				switch (p_temp->data.p_type) {
				case STR:
					mqtt_buf_free(
					    &p_temp->data.p_value.str);
					break;
				case BINARY:
					mqtt_buf_free(
					    &p_temp->data.p_value.binary);
					break;
				case STR_PAIR:
					mqtt_kv_free(
					    &p_temp->data.p_value.strpair);
					break;
				default:
					break;
				}
				if (p_temp->next != NULL) {
					p->next = p_temp->next;
				} else {
					p->next = NULL;
				}
				free(p_temp);
				break;
			}
		}
	}
}

int
property_dup(property **dup, const property *src)
{
	property *item = NULL;
	if (src == NULL) {
		return -1;
	}
	property *list = property_alloc();

	for (property *p = src->next; p != NULL; p = p->next) {
		property_type_enum type = property_get_value_type(p->id);
		switch (type) {
		case U8:
			item =
			    property_set_value_u8(p->id, p->data.p_value.u8);
			break;
		case U16:
			item =
			    property_set_value_u16(p->id, p->data.p_value.u16);
			break;
		case U32:
			item =
			    property_set_value_u32(p->id, p->data.p_value.u32);
			break;
		case VARINT:
			item = property_set_value_varint(
			    p->id, p->data.p_value.varint);
			break;
		case BINARY:
			item = property_set_value_binary(p->id,
			    p->data.p_value.binary.buf,
			    p->data.p_value.binary.length, true);
			break;
		case STR:
			item = property_set_value_str(p->id,
			    (const char *) p->data.p_value.str.buf,
			    p->data.p_value.str.length, true);
			break;
		case STR_PAIR:
			item = property_set_value_strpair(p->id,
			    (const char *) p->data.p_value.strpair.key.buf,
			    p->data.p_value.strpair.key.length,
			    (const char *) p->data.p_value.strpair.value.buf,
			    p->data.p_value.strpair.value.length, true);
			break;
		default:
			break;
		}
		if (item) {
			property_append(list, item);
		}
		item = NULL;
	}
	*dup = list;
	return 0;
}

void
property_foreach(property *prop, void (*cb)(property *))
{
	for (property *p = prop->next; p != NULL; p = p->next) {
		cb(p);
	}
}

int
property_free(property *prop)
{
	property *head = prop;
	property *p;

	while (head) {
		p    = head;
		head = head->next;
		if (p->data.is_copy) {
			switch (p->data.p_type) {
			case STR:
				mqtt_buf_free(&p->data.p_value.str);
				break;
			case BINARY:
				mqtt_buf_free(&p->data.p_value.binary);
				break;
			case STR_PAIR:
				mqtt_kv_free(&p->data.p_value.strpair);
				break;
			default:
				break;
			}
		}
		free(p);
		p = NULL;
	}
	return 0;
}

// Check if repeated properties exist
reason_code
check_properties(property *prop)
{
	if (prop == NULL) {
		return SUCCESS;
	}
	for (property *p1 = prop->next; p1 != NULL; p1 = p1->next) {
		for (property *p2 = p1->next; p2 != NULL; p2 = p2->next) {
			if (p1->id == p2->id &&
			    p1->data.p_type != STR_PAIR) {
				return PROTOCOL_ERROR;
			}
		}
	}

	return SUCCESS;
}

/**
 * packet_len: remaining length
 * len: property length
 * */
property *
decode_buf_properties(uint8_t *packet, uint32_t packet_len, uint32_t *pos,
    uint32_t *len, bool copy_value)
{
	int       rv;
	uint8_t * msg_body    = packet;
	size_t    msg_len     = packet_len;
	uint32_t  prop_len    = 0;
	uint8_t   bytes       = 0;
	uint32_t  current_pos = *pos;
	property *list        = NULL;

	if (current_pos >= msg_len) {
		return NULL;
	}

	if ((rv = read_variable_int(msg_body + current_pos,
	         msg_len - current_pos, &prop_len, &bytes)) != 0) {
		*len = 0;
		return NULL;
	}
	current_pos += bytes;
	if (prop_len == 0) {
		goto out;
	}
	struct pos_buf buf = {
		.curpos = &msg_body[current_pos],
		.endpos = &msg_body[current_pos + prop_len],
	};

	uint8_t prop_id = 0;
	list            = property_alloc();
	/* Check properties appearance time */
	// TODO

	while (buf.curpos < buf.endpos) {
		if (0 != read_byte(&buf, &prop_id)) {
			property_free(list);
			break;
		}
		property *         cur_prop = NULL;
		property_type_enum type     = property_get_value_type(prop_id);
		cur_prop =
		    property_parse(&buf, cur_prop, prop_id, type, copy_value);
		property_append(list, cur_prop);
	}

out:
	current_pos += (prop_len);
	*pos = current_pos;
	*len = prop_len;
	return list;
}

property *
decode_properties(nng_msg *msg, uint32_t *pos, uint32_t *len, bool copy_value)
{
	uint8_t *msg_body = nng_msg_body(msg);
	size_t   msg_len  = nng_msg_len(msg);
	return decode_buf_properties(msg_body, msg_len, pos, len, copy_value);
}

uint32_t
get_properties_len(property *prop)
{
	uint32_t prop_len = 0;
	if (prop != NULL) {
		for (property *p = prop->next; p != NULL; p = p->next) {
			switch (p->data.p_type) {
			case U8:
				prop_len++;
				prop_len += 1;
				break;
			case U16:
				prop_len++;
				prop_len += 2;
				break;
			case U32:
				prop_len++;
				prop_len += 4;
				break;
			case VARINT:
				prop_len++;
				prop_len += (byte_number_for_variable_length(
				    p->data.p_value.varint));
				break;
			case BINARY:
				prop_len++;
				prop_len += p->data.p_value.binary.length + 2;
				break;
			case STR:
				prop_len++;
				prop_len += p->data.p_value.str.length + 2;
				break;
			case STR_PAIR:
				prop_len++;
				prop_len +=
				    p->data.p_value.strpair.key.length + 2 +
				    p->data.p_value.strpair.value.length + 2;
				break;

			default:
				break;
			}
		}
	}

	return prop_len;
}

property_data *
property_get_value(property *prop, uint8_t prop_id)
{
	if (prop) {
		for (property *p = prop->next; p != NULL; p = p->next) {
			if (p->id == prop_id) {
				return &p->data;
			}
		}
	}
	return NULL;
}

int
property_value_copy(property *dest,const property *src)
{
	if (dest == NULL || src == NULL) {
		return -1;
	}

	dest->data.is_copy = src->data.is_copy;
	dest->data.p_type  = src->data.p_type;
	switch (src->data.p_type) {
	case U8:
		dest->data.p_value.u8 = src->data.p_value.u8;
		break;
	case U16:
		dest->data.p_value.u16 = src->data.p_value.u16;
		break;
	case U32:
		dest->data.p_value.u32 = src->data.p_value.u32;
		break;
	case VARINT:
		dest->data.p_value.varint = src->data.p_value.varint;
		break;
	case BINARY:
		if (src->data.is_copy) {
			mqtt_buf_dup(&dest->data.p_value.binary,
			    &src->data.p_value.binary);
		} else {
			dest->data.p_value.binary = src->data.p_value.binary;
		}
		break;
	case STR:
		if (src->data.is_copy) {
			mqtt_buf_dup(
			    &dest->data.p_value.str, &src->data.p_value.str);
		} else {
			dest->data.p_value.str = src->data.p_value.str;
		}
		break;
	case STR_PAIR:
		if (src->data.is_copy) {
			mqtt_kv_dup(&dest->data.p_value.strpair,
			    &src->data.p_value.strpair);
		} else {
			dest->data.p_value.strpair = src->data.p_value.strpair;
		}
		break;

	default:
		break;
	}

	return 0;
}

property *
property_pub_by_will(property *will_prop)
{
	property *list = NULL;

	if (will_prop == NULL) {
		goto out;
	}

	property_dup(&list, will_prop);
	property_remove(list, WILL_DELAY_INTERVAL);

	return list;

out:
	property_free(list);
	return NULL;
}

int
encode_properties(nni_msg *msg, property *prop, uint8_t cmd)
{
	uint8_t        rlen[4] = { 0 };
	struct pos_buf buf     = { .curpos = &rlen[0],
                .endpos                = &rlen[sizeof(rlen)] };

	uint32_t prop_len = get_properties_len(prop);
	int      bytes    = write_variable_length_value(prop_len, &buf);
	nni_msg_append(msg, rlen, bytes);
	if (prop_len == 0) {
		return 0;
	}
	if (cmd == CMD_PUBLISH) {
		/* TODO
		nni_time       rtime = nni_msg_get_timestamp(msg);
		nni_time       ntime = nni_clock();
		property_data *data =
		    property_get_value(prop, MESSAGE_EXPIRY_INTERVAL);
		if (data && ntime > rtime + data->p_value.u32 * 1000) {
			return -1;
		} else if (data) {
			data->p_value.u32 =
			    data->p_value.u32 - (ntime - rtime) / 1000;
		}
		*/
	}

	for (property *p = prop->next; p != NULL; p = p->next) {
		nni_mqtt_msg_append_u8(msg, p->id);
		property_type_enum type = property_get_value_type(p->id);
		switch (type) {
		case U8:
			nni_mqtt_msg_append_u8(msg, p->data.p_value.u8);
			break;
		case U16:
			nni_mqtt_msg_append_u16(msg, p->data.p_value.u16);
			break;
		case U32:
			nni_mqtt_msg_append_u32(msg, p->data.p_value.u32);
			break;
		case VARINT:
			nni_mqtt_msg_append_varint(
			    msg, p->data.p_value.varint);
			break;
		case BINARY:
			nni_mqtt_msg_append_byte_str(
			    msg, &p->data.p_value.binary);
			break;
		case STR:
			nni_mqtt_msg_append_byte_str(
			    msg, &p->data.p_value.str);
			break;
		case STR_PAIR:
			nni_mqtt_msg_append_byte_str(
			    msg, &p->data.p_value.strpair.key);
			nni_mqtt_msg_append_byte_str(
			    msg, &p->data.p_value.strpair.value);
			break;

		default:
			break;
		}
	}

	return 0;
}

/* introduced from mqtt_parser, might be duplicated */

/**
 * @brief decode puback/pubrec/pubrel/pubcomp
 *
 * @param msg
 * @param packet_id
 * @param reason_code
 * @param proto_ver
 * @return int
 */
int
nni_mqtt_pubres_decode(nng_msg *msg, uint16_t *packet_id, uint8_t *reason_code,
    property **prop, uint8_t proto_ver)
{
	int      rv;
	uint8_t *body   = nni_msg_body(msg);
	size_t   length = nni_msg_len(msg);

	struct pos_buf buf = { .curpos = &body[0], .endpos = &body[length] };

	if ((rv = read_uint16(&buf, packet_id)) != MQTT_SUCCESS) {
		*prop = NULL;
		return rv;
	}

	if (length == 2 || proto_ver != MQTT_PROTOCOL_VERSION_v5) {
		*prop = NULL;
		return MQTT_SUCCESS;
	}

	if ((rv = read_byte(&buf, reason_code)) != MQTT_SUCCESS) {
		*prop = NULL;
		return rv;
	}

	if ((buf.endpos - buf.curpos) <= 0) {
		*prop = NULL;
		return MQTT_SUCCESS;
	}

	uint32_t pos      = (uint32_t)(buf.curpos - body);
	uint32_t prop_len = 0;

	*prop = decode_properties(msg, &pos, &prop_len, false);
	if (check_properties(*prop) != SUCCESS) {
		property_free(*prop);
		*prop = NULL;
		return PROTOCOL_ERROR;
	}

	return MQTT_SUCCESS;
}

/**
 * @brief encode header of puback/pubrec/pubrel/pubcomp
 *
 * @param msg
 * @param cmd
 * @return int
 */
int
nni_mqtt_pubres_header_encode(nng_msg *msg, uint8_t cmd)
{
	size_t         msg_len    = nng_msg_len(msg);
	uint8_t        var_len[4] = { 0 };
	struct pos_buf buf = { .curpos = &var_len[0], .endpos = &var_len[4] };

	int bytes = write_variable_length_value(msg_len, &buf);
	if (cmd == CMD_PUBREL) {
		cmd |= 0x02;
	}
	nng_msg_header_append(msg, &cmd, 1);
	nng_msg_header_append(msg, var_len, bytes);

	return 0;
}

/**
 * @brief encode puback/pubrec/pubrel/pubcomp
 *
 * @param msg
 * @param packet_id
 * @param reason_code
 * @param prop copy content from prop
 * @param proto_ver
 * @return int
 */
int
nni_mqtt_msgack_encode(nng_msg *msg, uint16_t packet_id, uint8_t reason_code,
    property *prop, uint8_t proto_ver)
{
	uint8_t *rbuf = nni_zalloc(2);
	NNI_PUT16(rbuf, packet_id);
	nni_msg_clear(msg);
	nni_msg_append(msg, rbuf, 2);
	nni_free(rbuf, 2);

	if (proto_ver == MQTT_PROTOCOL_VERSION_v5) {
		if (reason_code == 0 && prop == NULL) {
			return MQTT_SUCCESS;
		}
		nni_msg_append(msg, &reason_code, 1);
		//All ack msgs are same
		encode_properties(msg, prop, 0);
	}

	return MQTT_SUCCESS;
}
