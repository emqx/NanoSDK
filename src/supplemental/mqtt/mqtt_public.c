#include "mqtt_msg.h"
#include "core/nng_impl.h"
#include <string.h>
#include "mqtt_qos_db.h"

int
nng_mqtt_msg_proto_data_alloc(nng_msg *msg)
{
	return nni_mqtt_msg_proto_data_alloc(msg);
}

void
nng_mqtt_msg_proto_data_free(nng_msg *msg)
{
	nni_mqtt_msg_proto_data_free(msg);
}

int
nng_mqtt_msg_alloc(nng_msg **msg, size_t sz)
{
	return nni_mqtt_msg_alloc(msg, sz);
}

int
nng_mqtt_msg_encode(nng_msg *msg)
{
	return nni_mqtt_msg_encode(msg);
}

int
nng_mqtt_msg_decode(nng_msg *msg)
{
	return nni_mqtt_msg_decode(msg);
}

int
nng_mqttv5_msg_encode(nng_msg *msg)
{
	return nni_mqttv5_msg_encode(msg);
}

int
nng_mqttv5_msg_decode(nng_msg *msg)
{
	return nni_mqttv5_msg_decode(msg);
}

int
nng_mqtt_msg_validate(nng_msg *msg, uint8_t proto_ver)
{
	return nni_mqtt_msg_validate(msg, proto_ver);
}

void
nng_mqtt_msg_set_packet_type(nng_msg *msg, nng_mqtt_packet_type packet_type)
{
	nni_mqtt_msg_set_packet_type(msg, (nni_mqtt_packet_type) packet_type);
}

nng_mqtt_packet_type
nng_mqtt_msg_get_packet_type(nng_msg *msg)
{
	return (nng_mqtt_packet_type) nni_mqtt_msg_get_packet_type(msg);
}

void
nng_mqtt_msg_set_connect_clean_session(nng_msg *msg, bool clean_session)
{
	nni_mqtt_msg_set_connect_clean_session(msg, clean_session);
}

void
nng_mqtt_msg_set_connect_will_retain(nng_msg *msg, bool will_retain)
{
	nni_mqtt_msg_set_connect_will_retain(msg, will_retain);
}

void
nng_mqtt_msg_set_connect_will_qos(nng_msg *msg, uint8_t will_qos)
{
	nni_mqtt_msg_set_connect_will_qos(msg, will_qos);
}

property *
nng_mqtt_msg_get_connect_will_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_will_property(msg);
}

bool
nng_mqtt_msg_get_connect_clean_session(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_clean_session(msg);
}

bool
nng_mqtt_msg_get_connect_will_retain(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_will_retain(msg);
}

uint8_t
nng_mqtt_msg_get_connect_will_qos(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_will_qos(msg);
}

void
nng_mqtt_msg_set_connect_proto_version(nng_msg *msg, uint8_t proto_version)
{
	nni_mqtt_msg_set_connect_proto_version(msg, proto_version);
}

void
nng_mqtt_msg_set_connect_keep_alive(nng_msg *msg, uint16_t keep_alive)
{
	nni_mqtt_msg_set_connect_keep_alive(msg, keep_alive);
}

void
nng_mqtt_msg_set_connect_client_id(nng_msg *msg, const char *client_id)
{
	nni_mqtt_msg_set_connect_client_id(msg, client_id);
}

void
nng_mqtt_msg_set_connect_will_topic(nng_msg *msg, const char *will_topic)
{
	nni_mqtt_msg_set_connect_will_topic(msg, will_topic);
}

void
nng_mqtt_msg_set_connect_will_msg(
    nng_msg *msg, uint8_t *will_msg, uint32_t len)
{
	nni_mqtt_msg_set_connect_will_msg(msg, will_msg, len);
}

void
nng_mqtt_msg_set_connect_user_name(nng_msg *msg, const char *user_name)
{
	nni_mqtt_msg_set_connect_user_name(msg, user_name);
}

void
nng_mqtt_msg_set_connect_password(nng_msg *msg, const char *password)
{
	nni_mqtt_msg_set_connect_password(msg, password);
}

void
nng_mqtt_msg_set_connect_property(nng_msg *msg, property *p)
{
	nni_mqtt_msg_set_connect_property(msg, p);
}

void
nng_mqtt_msg_set_connect_will_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_connect_will_property(msg, prop);
}

uint8_t
nng_mqtt_msg_get_connect_proto_version(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_proto_version(msg);
}

uint16_t
nng_mqtt_msg_get_connect_keep_alive(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_keep_alive(msg);
}

const char *
nng_mqtt_msg_get_connect_client_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_client_id(msg);
}

const char *
nng_mqtt_msg_get_connect_will_topic(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_will_topic(msg);
}

uint8_t *
nng_mqtt_msg_get_connect_will_msg(nng_msg *msg, uint32_t *len)
{
	return nni_mqtt_msg_get_connect_will_msg(msg, len);
}

const char *
nng_mqtt_msg_get_connect_user_name(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_user_name(msg);
}

const char *
nng_mqtt_msg_get_connect_password(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_password(msg);
}

property *
nng_mqtt_msg_get_connect_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_connect_property(msg);
}

void
nng_mqtt_msg_set_connack_return_code(nng_msg *msg, uint8_t return_code)
{
	nni_mqtt_msg_set_connack_return_code(msg, return_code);
}

void
nng_mqtt_msg_set_connack_flags(nng_msg *msg, uint8_t flags)
{
	nni_mqtt_msg_set_connack_flags(msg, flags);
}

void
nng_mqtt_msg_set_connack_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_connack_property(msg, prop);
}

uint8_t
nng_mqtt_msg_get_connack_return_code(nng_msg *msg)
{
	return nni_mqtt_msg_get_connack_return_code(msg);
}

uint8_t
nng_mqtt_msg_get_connack_flags(nng_msg *msg)
{
	return nni_mqtt_msg_get_connack_flags(msg);
}

property *
nng_mqtt_msg_get_connack_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_connack_property(msg);
}

void
nng_mqtt_msg_set_disconnect_reason_code(nng_msg *msg, uint8_t reason_code)
{
	nni_mqtt_msg_set_disconnect_reason_code(msg, reason_code);
}

property *
nng_mqtt_msg_get_disconnect_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_disconnect_property(msg);
}

void
nng_mqtt_msg_set_disconnect_property(nng_msg *msg, property *p)
{
	nni_mqtt_msg_set_disconnect_property(msg, p);
}

void
nng_mqtt_msg_set_publish_qos(nng_msg *msg, uint8_t qos)
{
	nni_mqtt_msg_set_publish_qos(msg, qos);
}

property *
nng_mqtt_msg_get_publish_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_publish_property(msg);
}

void
nng_mqtt_msg_set_publish_property(nng_msg *msg, property *p)
{
	nni_mqtt_msg_set_publish_property(msg, p);
}

uint8_t
nng_mqtt_msg_get_publish_qos(nng_msg *msg)
{
	return nni_mqtt_msg_get_publish_qos(msg);
}

void
nng_mqtt_msg_set_publish_retain(nng_msg *msg, bool retain)
{
	nni_mqtt_msg_set_publish_retain(msg, retain);
}

bool
nng_mqtt_msg_get_publish_retain(nng_msg *msg)
{
	return nni_mqtt_msg_get_publish_retain(msg);
}

void
nng_mqtt_msg_set_publish_dup(nng_msg *msg, bool dup)
{
	nni_mqtt_msg_set_publish_dup(msg, dup);
}

bool
nng_mqtt_msg_get_publish_dup(nng_msg *msg)
{
	return nni_mqtt_msg_get_publish_dup(msg);
}

/**
 * @brief set publishing topic for this msg
 * 		return 0 if scussed -1 invalid.
 * @param msg 
 * @param topic 
 * @return int 
 */
int
nng_mqtt_msg_set_publish_topic(nng_msg *msg, const char *topic)
{
	// Topic alias
	if (topic == NULL)
		return 0;
	// wild card
	if (strchr(topic, '#') != NULL || strchr(topic, '+') != NULL) {
		return -1;
	}
	return nni_mqtt_msg_set_publish_topic(msg, topic);
}

const char *
nng_mqtt_msg_get_publish_topic(nng_msg *msg, uint32_t *topic_len)
{
	return nni_mqtt_msg_get_publish_topic(msg, topic_len);
}

void
nng_mqtt_msg_set_publish_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_publish_packet_id(msg, packet_id);
}

uint16_t
nng_mqtt_msg_get_publish_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_publish_packet_id(msg);
}

void
nng_mqtt_msg_set_publish_payload(nng_msg *msg, uint8_t *payload, uint32_t len)
{
	nni_mqtt_msg_set_publish_payload(msg, payload, len);
}

// payload & topic must be set together!
uint8_t *
nng_mqtt_msg_get_publish_payload(nng_msg *msg, uint32_t *len)
{
	return nni_mqtt_msg_get_publish_payload(msg, len);
}

uint16_t
nng_mqtt_msg_get_puback_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_puback_packet_id(msg);
}

void
nng_mqtt_msg_set_puback_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_puback_packet_id(msg, packet_id);
}

property *
nng_mqtt_msg_get_puback_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_puback_property(msg);
}

void
nng_mqtt_msg_set_puback_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_puback_property(msg, prop);
}

property *
nng_mqtt_msg_get_pubrec_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_pubrec_property(msg);
}

void
nng_mqtt_msg_set_pubrec_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_pubrec_property(msg, prop);
}

property *
nng_mqtt_msg_get_pubrel_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_pubrel_property(msg);
}

void
nng_mqtt_msg_set_pubrel_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_pubrel_property(msg, prop);
}

property *
nng_mqtt_msg_get_pubcomp_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_pubcomp_property(msg);
}

void
nng_mqtt_msg_set_pubcomp_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_pubcomp_property(msg, prop);
}

uint16_t
nng_mqtt_msg_get_pubrec_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_pubrec_packet_id(msg);
}

void
nng_mqtt_msg_set_pubrec_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_pubrec_packet_id(msg, packet_id);
}

uint16_t
nng_mqtt_msg_get_pubrel_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_pubrel_packet_id(msg);
}

void
nng_mqtt_msg_set_pubrel_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_pubrel_packet_id(msg, packet_id);
}

uint16_t
nng_mqtt_msg_get_pubcomp_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_pubcomp_packet_id(msg);
}

void
nng_mqtt_msg_set_pubcomp_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_pubcomp_packet_id(msg, packet_id);
}

uint16_t
nng_mqtt_msg_get_subscribe_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_subscribe_packet_id(msg);
}

void
nng_mqtt_msg_set_subscribe_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_subscribe_packet_id(msg, packet_id);
}

void
nng_mqtt_msg_set_subscribe_topics(
    nng_msg *msg, nng_mqtt_topic_qos *topics, uint32_t topics_count)
{
	nni_mqtt_msg_set_subscribe_topics(
	    msg, (nni_mqtt_topic_qos *) topics, topics_count);
}

nng_mqtt_topic_qos *
nng_mqtt_msg_get_subscribe_topics(nng_msg *msg, uint32_t *topics_count)
{
	return nni_mqtt_msg_get_subscribe_topics(msg, topics_count);
}

property *
nng_mqtt_msg_get_subscribe_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_subscribe_property(msg);
}

void
nng_mqtt_msg_set_subscribe_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_subscribe_property(msg, prop);
}

uint16_t
nng_mqtt_msg_get_suback_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_suback_packet_id(msg);
}

void
nng_mqtt_msg_set_suback_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_suback_packet_id(msg, packet_id);
}
void
nng_mqtt_msg_set_suback_return_codes(
    nng_msg *msg, uint8_t *return_codes, uint32_t return_codes_count)
{
	nni_mqtt_msg_set_suback_return_codes(
	    msg, return_codes, return_codes_count);
}

uint8_t *
nng_mqtt_msg_get_suback_return_codes(
    nng_msg *msg, uint32_t *return_codes_counts)
{
	return nni_mqtt_msg_get_suback_return_codes(msg, return_codes_counts);
}

property *
nng_mqtt_msg_get_suback_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_suback_property(msg);
}

void
nng_mqtt_msg_set_suback_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_suback_property(msg, prop);
}

uint16_t
nng_mqtt_msg_get_unsubscribe_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_unsubscribe_packet_id(msg);
}

void
nng_mqtt_msg_set_unsubscribe_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_unsubscribe_packet_id(msg, packet_id);
}

void
nng_mqtt_msg_set_unsubscribe_topics(
    nng_msg *msg, nng_mqtt_topic *topics, uint32_t topics_count)
{
	nni_mqtt_msg_set_unsubscribe_topics(
	    msg, (nni_mqtt_topic *) topics, topics_count);
}

nng_mqtt_topic *
nng_mqtt_msg_get_unsubscribe_topics(nng_msg *msg, uint32_t *topics_count)
{
	return nni_mqtt_msg_get_unsubscribe_topics(msg, topics_count);
}

property *
nng_mqtt_msg_get_unsubscribe_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_unsubscribe_property(msg);
}

void
nng_mqtt_msg_set_unsubscribe_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_unsubscribe_property(msg, prop);
}

void
nng_mqtt_msg_set_unsuback_packet_id(nng_msg *msg, uint16_t packet_id)
{
	nni_mqtt_msg_set_unsuback_packet_id(msg, packet_id);
}

uint16_t
nng_mqtt_msg_get_unsuback_packet_id(nng_msg *msg)
{
	return nni_mqtt_msg_get_unsuback_packet_id(msg);
}

void
nng_mqtt_msg_set_unsuback_return_codes(
    nng_msg *msg, uint8_t *return_codes, uint32_t return_codes_count)
{
	nni_mqtt_msg_set_unsuback_return_codes(
	    msg, return_codes, return_codes_count);
}

uint8_t *
nng_mqtt_msg_get_unsuback_return_codes(
    nng_msg *msg, uint32_t *return_codes_counts)
{
	return nni_mqtt_msg_get_unsuback_return_codes(msg, return_codes_counts);
}


property *
nng_mqtt_msg_get_unsuback_property(nng_msg *msg)
{
	return nni_mqtt_msg_get_unsuback_property(msg);
}

void
nng_mqtt_msg_set_unsuback_property(nng_msg *msg, property *prop)
{
	nni_mqtt_msg_set_unsuback_property(msg, prop);
}

nng_mqtt_topic *
nng_mqtt_topic_array_create(size_t n)
{
	return nni_mqtt_topic_array_create(n);
}

void
nng_mqtt_topic_array_set(
    nng_mqtt_topic *topic, size_t n, const char *topic_name)
{
	nni_mqtt_topic_array_set(topic, n, topic_name);
}

void
nng_mqtt_topic_array_free(nng_mqtt_topic *topic, size_t n)
{
	nni_mqtt_topic_array_free(topic, n);
}

nng_mqtt_topic_qos *
nng_mqtt_topic_qos_array_create(size_t n)
{
	return nni_mqtt_topic_qos_array_create(n);
}
/***
 * set each fields of nng_mqtt_topic_qos of a Sub packet
 * *topic_qos: array of topic/qos pair
 * index: target topic/qos pair
 * *topic_name: subscription topic
 * qos : MQTT QoS
 * nocal: Subscription Options  no local
 * rap: Subscription Options    Retain as publish
 * rh: Subscription Options     Retain Handling
 * 
*/
void
nng_mqtt_topic_qos_array_set(nng_mqtt_topic_qos *topic_qos, size_t index,
    const char *topic_name, uint32_t len, uint8_t qos, uint8_t nolocal, uint8_t rap, uint8_t rh)
{
	nni_mqtt_topic_qos_array_set(topic_qos, index, topic_name, len, qos, nolocal, rap, rh);
}

void
nng_mqtt_topic_qos_array_free(nng_mqtt_topic_qos *topic_qos, size_t n)
{
	nni_mqtt_topic_qos_array_free(topic_qos, n);
}

int
nng_mqtt_set_connect_cb(nng_socket sock, nng_pipe_cb cb, void *arg)
{
	return nng_pipe_notify(sock, NNG_PIPE_EV_ADD_POST, cb, arg);
}

int
nng_mqtt_set_disconnect_cb(nng_socket sock, nng_pipe_cb cb, void *arg)
{
	return nng_pipe_notify(sock, NNG_PIPE_EV_REM_POST, cb, arg);
}

void
nng_mqtt_msg_dump(
    nng_msg *msg, uint8_t *buffer, uint32_t len, bool print_bytes)
{
	nni_mqtt_msg_dump(msg, buffer, len, print_bytes);
}

uint32_t
get_mqtt_properties_len(property *prop)
{
	return get_properties_len(prop);
}

int
mqtt_property_free(property *prop)
{
	return property_free(prop);
}

void
mqtt_property_foreach(property *prop, void (*cb)(property *))
{
	return property_foreach(prop, cb);
}

int
mqtt_property_dup(property **dup, const property *src)
{
	return property_dup(dup, src);
}

property *
mqtt_property_pub_by_will(property *will_prop)
{
	return property_pub_by_will(will_prop);
}

int
mqtt_property_value_copy(property *dst, const property *src)
{
	return property_value_copy(dst, src);
}

property *
mqtt_property_alloc(void)
{
	return property_alloc();
}

property *
mqtt_property_set_value_u8(uint8_t prop_id, uint8_t value)
{
	return property_set_value_u8(prop_id, value);
}

property *
mqtt_property_set_value_u16(uint8_t prop_id, uint16_t value)
{
	return property_set_value_u16(prop_id, value);
}

property *
mqtt_property_set_value_u32(uint8_t prop_id, uint32_t value)
{
	return property_set_value_u32(prop_id, value);
}

property *
mqtt_property_set_value_varint(uint8_t prop_id, uint32_t value)
{
	return property_set_value_varint(prop_id, value);
}

property *
mqtt_property_set_value_binary(uint8_t prop_id, uint8_t *value, uint32_t len, bool copy_value)
{
	return property_set_value_binary(prop_id, value, len, copy_value);
}

property *
mqtt_property_set_value_str( uint8_t prop_id, const char *value, uint32_t len, bool copy_value)
{
	return property_set_value_str(prop_id, value, len, copy_value);
}

property *
mqtt_property_set_value_strpair(uint8_t prop_id, const char *key, uint32_t key_len, const char *value, uint32_t value_len, bool copy_value)
{
	return property_set_value_strpair(prop_id, key, key_len, value, value_len, copy_value);
}

property_type_enum
mqtt_property_get_value_type(uint8_t prop_id)
{
	return property_get_value_type(prop_id);
}

property_data *
mqtt_property_get_value(property *prop, uint8_t prop_id)
{
	return property_get_value(prop, prop_id);
}

void
mqtt_property_append(property *prop_list, property *last)
{
	return property_append(prop_list, last);
}


// static void
// mqtt_sub_aio_cancel(nni_aio *aio, void *arg, int rv)
// {
// 	NNI_ARG_UNUSED(arg);
// 	// nng_mqtt_client *client = arg;

// 	if (!nni_aio_list_active(aio)) {
// 		return;
// 	}
// 	// If receive in progress, then cancel the pending transfer.
// 	// The callback on the rxaio will cause the user aio to
// 	// be canceled too.
// 	// if (nni_list_first(&p->recvq) == aio) {
// 	// 	nni_aio_abort(p->rxaio, rv);
// 	// 	nni_mtx_unlock(&p->mtx);
// 	// 	return;
// 	// }
// 	nni_aio_list_remove(aio);
// 	nni_aio_finish_error(aio, rv);
// }

static void
nng_mqtt_client_send_cb(void* arg)
{
	nng_mqtt_client *client = (nng_mqtt_client *) arg;
	nng_aio *        aio    = client->send_aio;
	nng_msg *        msg    = nng_aio_get_msg(aio);
	nng_msg *        tmsg   = NULL;

	nni_lmq * lmq = (nni_lmq *)client->msgq;

	if (msg == NULL || nng_aio_result(aio) != 0) {
		client->send_cb(client, NULL, client->obj);
		return;
	}

	if (nni_lmq_get(lmq, &tmsg) == 0) {
		nng_aio_set_msg(client->send_aio, tmsg);
		nng_send_aio(client->sock, client->send_aio);
	}
	client->send_cb(client, msg, client->obj);
	return;
}

static void
nng_mqtt_client_recv_cb(void* arg)
{
	nng_mqtt_client *client = (nng_mqtt_client *) arg;
	nng_aio *        aio    = client->recv_aio;
	nng_msg *        msg    = nng_aio_get_msg(aio);
	int 			 rv;

	if (msg == NULL || (rv = nng_aio_result(aio)) != 0) {
		nni_plat_printf("aio recv error! %d", rv);
	}
	nng_recv_aio(client->sock, client->recv_aio);
	uint32_t topicsz, payloadsz;
	char *topic = (char *)nng_mqtt_msg_get_publish_topic(msg, &topicsz);
	char *payload =
		(char *)nng_mqtt_msg_get_publish_payload(msg, &payloadsz);

	nni_plat_printf("[Msg Arrived][%s]...\n", (char *)arg);
	nni_plat_printf("topic   => %.*s\n"
		   "payload => %.*s\n",
		   topicsz, topic, payloadsz, payload);
	client->recv_cb(client, msg, client->obj);

	return;
}

/**
 * ATTENTION: This API is still under development
 *            it might be changed in the future
 * Alloc an nng_mqtt_client object
 * set is_async as true if you wanna enable async APIs
 * Return NULL if failed
 * */
nng_mqtt_client *
nng_mqtt_client_alloc(nng_socket sock, nng_mqtt_send_cb send_cb, nng_mqtt_recv_cb recv_cb, bool is_async)
{
	nng_mqtt_client *client = NNI_ALLOC_STRUCT(client);
	client->sock            = sock;
	client->async           = is_async;
	client->send_cb         = NULL;
	client->recv_cb         = NULL;
	client->msgq            = NULL;
	client->obj             = NULL;

	if (send_cb != NULL) {
		client->send_cb = send_cb;
	}
	if (recv_cb != NULL) {
		client->recv_cb = recv_cb;
	}
	if (is_async) {
		// replace nng_mqtt_client_send_cb for lmq
		if (send_cb != NULL)
			nng_aio_alloc(&client->send_aio, nng_mqtt_client_send_cb, client);
		if (recv_cb != NULL)
			nng_aio_alloc(&client->recv_aio, nng_mqtt_client_recv_cb, client);
		if ((client->msgq = nng_alloc(sizeof(nni_lmq))) == NULL) {
			return NULL;
		}
		nni_lmq_init((nni_lmq *)client->msgq, NNG_MAX_SEND_LMQ);
	}
	return client;
}

/**
 * @brief an AIO cannot kill itself in its own callback!!
 * 
 * @param client
 * @param is_async
 */
void nng_mqtt_client_free(nng_mqtt_client *client, bool is_async)
{
	nni_aio_close(client->send_aio);
	if (client) {
		if (is_async) {
			nng_aio_free(client->send_aio);
			nni_lmq_fini((nni_lmq *) client->msgq);
			nng_free(client->msgq, sizeof(nni_lmq));
		}
		NNI_FREE_STRUCT(client);
	}
	return;
}

int
nng_mqtt_unsubscribe(nng_socket sock, nng_mqtt_topic *sbs, size_t count, property *pl)
{
	int rv = 0;
	// create a SUBSCRIBE message
	nng_msg *unsubmsg;
	nng_mqtt_msg_alloc(&unsubmsg, 0);
	nng_mqtt_msg_set_packet_type(unsubmsg, NNG_MQTT_UNSUBSCRIBE);
	nng_mqtt_msg_set_unsubscribe_topics(unsubmsg, sbs, count);

	if (pl) {
		nng_mqtt_msg_set_unsubscribe_property(unsubmsg, pl);
	}

	if ((rv = nng_sendmsg(sock, unsubmsg, NNG_FLAG_ALLOC)) != 0) {
		nng_msg_free(unsubmsg);
	}

	return rv;
}

int 
nng_mqtt_unsubscribe_async(nng_mqtt_client *client, nng_mqtt_topic *sbs, size_t count, property *pl)
{
	nng_msg *unsubmsg;
	nng_mqtt_msg_alloc(&unsubmsg, 0);
	nng_mqtt_msg_set_packet_type(unsubmsg, NNG_MQTT_UNSUBSCRIBE);
	nng_mqtt_msg_set_unsubscribe_topics(unsubmsg, sbs, count);

	if (pl) {
		nng_mqtt_msg_set_subscribe_property(unsubmsg, pl);
	}
	if (nng_aio_busy(client->send_aio)) {
		if (nni_lmq_put((nni_lmq *)client->msgq, unsubmsg) != 0) {
			nni_plat_println("unsubscribe failed!");
		}
		return 1;
	}
	nng_aio_set_msg(client->send_aio, unsubmsg);
	nng_send_aio(client->sock, client->send_aio);

	return 0;
}

int
nng_mqtt_subscribe(nng_socket sock, nng_mqtt_topic_qos *sbs, size_t count, property *pl)
{
	int rv = 0;
	// create a SUBSCRIBE message
	nng_msg *submsg;
	nng_mqtt_msg_alloc(&submsg, 0);
	nng_mqtt_msg_set_packet_type(submsg, NNG_MQTT_SUBSCRIBE);
	nng_mqtt_msg_set_subscribe_topics(submsg, sbs, count);

	if (pl) {
		nng_mqtt_msg_set_subscribe_property(submsg, pl);
	}

	if ((rv = nng_sendmsg(sock, submsg, NNG_FLAG_ALLOC)) != 0) {
		nng_msg_free(submsg);
	}

	return rv;
}

int 
nng_mqtt_subscribe_async(nng_mqtt_client *client, nng_mqtt_topic_qos *sbs, size_t count, property *pl)
{
	// create a SUBSCRIBE message
	nng_msg *submsg;
	nng_mqtt_msg_alloc(&submsg, 0);
	nng_mqtt_msg_set_packet_type(submsg, NNG_MQTT_SUBSCRIBE);
	nng_mqtt_msg_set_subscribe_topics(submsg, sbs, count);

	if (pl) {
		nng_mqtt_msg_set_subscribe_property(submsg, pl);
	}
	if (nng_aio_busy(client->send_aio)) {
		if (nni_lmq_put((nni_lmq *)client->msgq, submsg) != 0) {
			nni_plat_println("subscribe failed!");
		}
		return 1;
	}
	nng_aio_set_msg(client->send_aio, submsg);
	if (client->send_cb != NULL)
		nng_send_aio(client->sock, client->send_aio);
	if (client->recv_cb != NULL)
		nng_recv_aio(client->sock, client->recv_aio);

	return 0;
}

int
nng_mqtt_disconnect(nng_socket *sock, uint8_t reason_code, property *pl)
{
	// create a DISCONNECT message
	int      rv = 0;
	nng_msg *disconnmsg;
	nng_mqtt_msg_alloc(&disconnmsg, 0);
	nng_mqtt_msg_set_packet_type(disconnmsg, NNG_MQTT_DISCONNECT);
	if (0 != reason_code) {
		nng_mqtt_msg_set_disconnect_reason_code(
		    disconnmsg, reason_code);
	}

	if (NULL != pl) {
		nng_mqtt_msg_set_disconnect_property(disconnmsg, pl);
	}

	if ((rv = nng_sendmsg(*sock, disconnmsg, 0)) != 0) {
		nng_msg_free(disconnmsg);
	}

	nng_close(*sock);

	return (rv);
}

#if defined(NNG_SUPP_SQLITE)

void
nng_mqtt_sqlite_db_init(nng_mqtt_sqlite_option *opt, const char *db_name, uint8_t proto)
{
	nni_mqtt_sqlite_db_init(opt, db_name, proto);
}

void
nng_mqtt_sqlite_db_fini(nng_mqtt_sqlite_option *opt)
{
	nni_mqtt_sqlite_db_fini(opt);
}

size_t
nng_mqtt_sqlite_db_get_cached_size(nng_mqtt_sqlite_option *sqlite)
{
	return nni_mqtt_sqlite_db_get_cached_size(sqlite);
}

int
nng_mqtt_alloc_sqlite_opt(nng_mqtt_sqlite_option **sqlite)
{
	int                     rv  = 0;
	nng_mqtt_sqlite_option *opt = NULL;
	if ((opt = nni_zalloc(sizeof(nng_mqtt_sqlite_option))) == NULL) {
		return (NNG_ENOMEM);
	}

	opt->enable              = true;
	opt->disk_cache_size     = 102400;
	opt->mounted_file_path   = NULL;
	opt->flush_mem_threshold = 100;
	opt->mqtt_version        = MQTT_PROTOCOL_VERSION_v311;

	*sqlite                  = opt;
	return (rv);
}

int
nng_mqtt_free_sqlite_opt(nng_mqtt_sqlite_option *opt)
{
	if (opt) {
		nni_mqtt_sqlite_db_fini(opt);
		nni_free(opt, sizeof(nng_mqtt_sqlite_option));
	}
	return 0;
}

void
nng_mqtt_set_sqlite_enable(nng_mqtt_sqlite_option *sqlite, bool enable)
{
	sqlite->enable = enable;
}

void
nng_mqtt_set_sqlite_db_dir(nng_mqtt_sqlite_option *sqlite, const char *dir)
{
	sqlite->mounted_file_path = nni_strdup(dir);
}

void
nng_mqtt_set_sqlite_max_rows(nng_mqtt_sqlite_option *sqlite, size_t max_rows)
{
	sqlite->disk_cache_size = max_rows;
}

void
nng_mqtt_set_sqlite_flush_threshold(
    nng_mqtt_sqlite_option *sqlite, size_t msg_count)
{
	sqlite->flush_mem_threshold = msg_count;
}
#endif
