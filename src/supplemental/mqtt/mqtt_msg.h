#ifndef NNG_SUPPLEMENTAL_MQTT_MQTT_MSG_H
#define NNG_SUPPLEMENTAL_MQTT_MQTT_MSG_H

// #include "mqtt_codec.h"
#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32
#include <stdint.h>
#else
#include <inttypes.h>
#endif

#include "core/nng_impl.h"
#include "nng/mqtt/mqtt_client.h"
#include "nng/nng.h"

#define MQTT_PROTOCOL_NAME "MQTT"

#define MQTT_MAX_MSG_LEN 268435455

#define MQTT_MAX_LENGTH_BYTES 4
#define MQTT_LENGTH_VALUE_MASK 0x7F
#define MQTT_LENGTH_CONTINUATION_BIT 0x80
#define MQTT_LENGTH_SHIFT 7

typedef struct mqtt_msg_t    nni_mqtt_proto_data;
typedef nng_mqtt_packet_type nni_mqtt_packet_type;
typedef union mqtt_payload   nni_mqtt_payload;
typedef nng_mqtt_topic_qos   nni_mqtt_topic_qos;
typedef nng_mqtt_buffer      nni_mqtt_buffer;
typedef nng_mqtt_topic       nni_mqtt_topic;

/* Quality of Service types. */
#define MQTT_QOS_0_AT_MOST_ONCE 0
#define MQTT_QOS_1_AT_LEAST_ONCE 1
#define MQTT_QOS_2_EXACTLY_ONCE 2

/* Function return codes */
// ONLY FOR INTERNAL USAGE!
#define MQTT_SUCCESS 0
#define MQTT_ERR_NOMEM 1
#define MQTT_ERR_PROTOCOL 2
#define MQTT_ERR_INVAL 3
#define MQTT_ERR_PAYLOAD_SIZE 4
#define MQTT_ERR_NOT_SUPPORTED 5
#define MQTT_ERR_NOT_FOUND 6
#define MQTT_ERR_MALFORMED 7

struct pos_buf {
	uint8_t *curpos;
	uint8_t *endpos;
};

/* CONNECT flags */
typedef struct conn_flags_t {
	uint8_t reserved : 1;
	uint8_t clean_session : 1;
	uint8_t will_flag : 1;
	uint8_t will_qos : 2;
	uint8_t will_retain : 1;
	uint8_t password_flag : 1;
	uint8_t username_flag : 1;
} conn_flags;

/*****************************************************************************
 * Variable header parts
 ****************************************************************************/
typedef struct mqtt_connect_vhdr_t {
	mqtt_buf   protocol_name;
	uint8_t    protocol_version;
	conn_flags conn_flags;
	uint16_t   keep_alive;
	// MQTTV5
	property * properties;
} mqtt_connect_vhdr;

typedef struct mqtt_disconnect_vhdr_t {
	// MQTTV5
	uint8_t   reason_code;
	property *properties;
} mqtt_disconnect_vhdr;

typedef struct mqtt_connack_vhdr_t {
	uint8_t connack_flags;
	uint8_t conn_return_code;
	// MQTTV5
	property *properties;
} mqtt_connack_vhdr;

typedef struct mqtt_publish_vhdr_t {
	mqtt_buf  topic_name;
	uint16_t  packet_id;
	property *properties;
} mqtt_publish_vhdr;

typedef struct mqtt_puback_vhdr_t {
	uint16_t    packet_id;
	reason_code code;
	property *properties;
} mqtt_puback_vhdr;

typedef struct mqtt_pubrec_vhdr_t {
	uint16_t packet_id;
	reason_code code;
	property *properties;
} mqtt_pubrec_vhdr;

typedef struct mqtt_pubrel_vhdr_t {
	uint16_t packet_id;
	reason_code code;
	property *properties;
} mqtt_pubrel_vhdr;

typedef struct mqtt_pubcomp_vhdr_t {
	uint16_t packet_id;
	reason_code code;
	property *properties;
} mqtt_pubcomp_vhdr;

typedef struct mqtt_subscribe_vhdr_t {
	uint16_t packet_id;
	// MQTTV5
	property *properties;
} mqtt_subscribe_vhdr;

typedef struct mqtt_suback_vhdr_t {
	uint16_t packet_id;
	// MQTTV5
	property *properties;
} mqtt_suback_vhdr;

typedef struct mqtt_unsubscribe_vhdr_t {
	uint16_t packet_id;
	// MQTTV5
	property *properties;
} mqtt_unsubscribe_vhdr;

typedef struct mqtt_unsuback_vhdr_t {
	uint16_t packet_id;
	// MQTTV5
	property *properties;
} mqtt_unsuback_vhdr;

/*****************************************************************************
 * Union to cover all Variable Header types
 ****************************************************************************/
union mqtt_variable_header {
	mqtt_connect_vhdr     connect;
	mqtt_disconnect_vhdr  disconnect;
	mqtt_connack_vhdr     connack;
	mqtt_publish_vhdr     publish;
	mqtt_puback_vhdr      puback;
	mqtt_pubrec_vhdr      pubrec;
	mqtt_pubrel_vhdr      pubrel;
	mqtt_pubcomp_vhdr     pubcomp;
	mqtt_subscribe_vhdr   subscribe;
	mqtt_suback_vhdr      suback;
	mqtt_unsubscribe_vhdr unsubscribe;
	mqtt_unsuback_vhdr    unsuback;
};

/*****************************************************************************
 * Payloads for mqtt
 ****************************************************************************/
typedef struct {
	mqtt_buf client_id;
	property *will_properties;
	mqtt_buf will_topic;
	mqtt_buf will_msg;
	mqtt_buf user_name;
	mqtt_buf password;
} mqtt_connect_payload;

typedef struct {
	mqtt_buf payload;
} mqtt_publish_payload;

typedef struct {
	mqtt_topic_qos *topic_arr; /* array of mqtt_topic_qos instances
	                              continuous in memory */
	uint32_t topic_count;      /* not included in the message itself */
} mqtt_subscribe_payload;

typedef struct {
	uint8_t *ret_code_arr; /* array of return codes continuous in memory */
	uint32_t ret_code_count; /* not included in the message itself */
} mqtt_suback_payload;

typedef struct {
	mqtt_buf *topic_arr;   /* array of topic_arr continuous in memory */
	uint32_t  topic_count; /* not included in the message itself */
} mqtt_unsubscribe_payload;

typedef struct {
	uint8_t *ret_code_arr; /* array of return codes continuous in memory */
	uint32_t ret_code_count; /* not included in the message itself */
} mqtt_unsuback_payload;

/*****************************************************************************
 * Union to cover all Payload types
 ****************************************************************************/
union mqtt_payload {
	mqtt_connect_payload     connect;
	mqtt_publish_payload     publish;
	mqtt_subscribe_payload   subscribe;
	mqtt_suback_payload      suback;
	mqtt_unsubscribe_payload unsubscribe;
	mqtt_unsuback_payload    unsuback;
};

typedef struct {
	uint8_t bit_0 : 1;
	uint8_t bit_1 : 1;
	uint8_t bit_2 : 1;
	uint8_t bit_3 : 1;
	uint8_t packet_type : 4;
} mqtt_common_hdr;

typedef struct {
	uint8_t retain : 1;
	uint8_t qos : 2;
	uint8_t dup : 1;
	uint8_t packet_type : 4;
} mqtt_pub_hdr;

typedef struct mqtt_fixed_hdr_t {
	union {
		mqtt_common_hdr common;
		mqtt_pub_hdr    publish;
	};

	uint32_t remaining_length; /* up to 268,435,455 (256 MB) */
} mqtt_fixed_hdr;

typedef struct mqtt_msg_t {
	/* Fixed header part */
	nni_aio *                  aio; // QoS AIO
	mqtt_fixed_hdr             fixed_header;
	union mqtt_variable_header var_header;
	union mqtt_payload         payload;

	uint8_t used_bytes : 4; /* byte count for used remainingLength
	                         representation This information (combined with
	                         packetType and packetFlags)  may be used to
	                         jump the point where the actual data starts */
	bool is_decoded : 1; /* message is obtained from decoded or encoded */
	bool is_copied : 1;  /* indicates string or array members are copied */
	bool initialized : 1; /* message is decoded or encoded*/
	uint8_t _unused : 1;
} mqtt_msg;

NNG_DECL int mqtt_get_remaining_length(
    uint8_t *, uint32_t, uint32_t *, uint8_t *);
NNG_DECL int byte_number_for_variable_length(uint32_t);
NNG_DECL int write_variable_length_value(uint32_t, struct pos_buf *);
NNG_DECL int write_byte(uint8_t, struct pos_buf *);
NNG_DECL int write_uint16(uint16_t, struct pos_buf *);
NNG_DECL int write_uint32(uint32_t, struct pos_buf *);
NNG_DECL int write_uint64(uint64_t, struct pos_buf *);
NNG_DECL int write_bytes(uint8_t *, size_t, struct pos_buf *);
NNG_DECL int write_byte_string(mqtt_buf *, struct pos_buf *);

NNG_DECL int read_variable_integer(struct pos_buf *, uint32_t *);
NNG_DECL int read_byte(struct pos_buf *, uint8_t *);
NNG_DECL int read_uint16(struct pos_buf *, uint16_t *);
NNG_DECL int read_uint32(struct pos_buf *, uint32_t *);
NNG_DECL int read_uint64(struct pos_buf *, uint64_t *);
NNG_DECL int read_bytes(struct pos_buf *, uint8_t **, size_t);
NNG_DECL int read_utf8_str(struct pos_buf *, mqtt_buf *);
NNG_DECL int read_str_data(struct pos_buf *, mqtt_buf *);
NNG_DECL int read_packet_length(struct pos_buf *, uint32_t *);

NNG_DECL int      mqtt_buf_create(mqtt_buf *, const uint8_t *, uint32_t);
NNG_DECL int      mqtt_buf_dup(mqtt_buf *, const mqtt_buf *);
NNG_DECL void     mqtt_buf_free(mqtt_buf *);
NNG_DECL nni_aio *nni_mqtt_msg_get_aio(nni_msg *);
NNG_DECL void     nni_mqtt_msg_set_aio(nni_msg *, nni_aio *);

NNG_DECL int mqtt_kv_create(
    mqtt_kv *, const char *, size_t, const char *, size_t);
NNG_DECL int  mqtt_kv_dup(mqtt_kv *, const mqtt_kv *);
NNG_DECL void mqtt_kv_free(mqtt_kv *);

NNG_DECL const char *get_packet_type_str(nni_mqtt_packet_type packtype);

NNG_DECL mqtt_msg *mqtt_msg_create(nni_mqtt_packet_type);

NNG_DECL int mqtt_msg_dump(mqtt_msg *, mqtt_buf *, mqtt_buf *, bool);

NNG_DECL int mqtt_pipe_recv_msgq_putq(nni_lmq *, nni_msg *);
// nni_msg proto_data alloc/free
NNG_DECL int  nni_mqtt_msg_proto_data_alloc(nni_msg *);
NNG_DECL void nni_mqtt_msg_proto_data_free(nni_msg *);
NNG_DECL int  nni_mqtt_msg_free(void *);
NNG_DECL int  nni_mqtt_msg_dup(void **, const void *);

// mqtt message alloc/encode/decode
NNG_DECL int nni_mqtt_msg_alloc(nni_msg **, size_t);

NNG_DECL int nni_mqtt_msg_encode(nni_msg *);
NNG_DECL int nni_mqtt_msg_decode(nni_msg *);

// mqtt message encode/decode for v5
NNG_DECL int nni_mqttv5_msg_encode(nni_msg *);
NNG_DECL int nni_mqttv5_msg_decode(nni_msg *);

NNG_DECL int nni_mqtt_msg_validate(nni_msg *, uint8_t);
NNG_DECL int nni_mqtt_msg_packet_validate(uint8_t *, size_t, size_t, uint8_t);

// mqtt packet_type
NNG_DECL void nni_mqtt_msg_set_packet_type(nni_msg *, nni_mqtt_packet_type);
NNG_DECL nni_mqtt_packet_type nni_mqtt_msg_get_packet_type(nni_msg *);

// mqtt packet id
// NOTE: not all packet have a packet id field
NNG_DECL void     nni_mqtt_msg_set_packet_id(nni_msg *, uint16_t);
NNG_DECL uint16_t nni_mqtt_msg_get_packet_id(nni_msg *);

// mqtt connect
NNG_DECL void nni_mqtt_msg_set_connect_clean_session(nni_msg *, bool);
NNG_DECL void nni_mqtt_msg_set_connect_proto_version(nni_msg *, uint8_t);
NNG_DECL void nni_mqtt_msg_set_connect_keep_alive(nni_msg *, uint16_t);
NNG_DECL void nni_mqtt_msg_set_connect_client_id(nni_msg *, const char *);
NNG_DECL void nni_mqtt_msg_set_connect_user_name(nni_msg *, const char *);
NNG_DECL void nni_mqtt_msg_set_connect_password(nni_msg *, const char *);
NNG_DECL void nni_mqtt_msg_set_connect_will_retain(nni_msg *, bool);
NNG_DECL void nni_mqtt_msg_set_connect_will_topic(nni_msg *, const char *);
NNG_DECL void nni_mqtt_msg_set_connect_will_msg(
    nni_msg *, uint8_t *, uint32_t);
NNG_DECL void nni_mqtt_msg_set_connect_will_qos(nni_msg *, uint8_t);
NNG_DECL bool nni_mqtt_msg_get_connect_clean_session(nni_msg *);
NNG_DECL void nni_mqtt_msg_set_connect_will_property(nni_msg *, property *);

NNG_DECL uint8_t     nni_mqtt_msg_get_connect_proto_version(nni_msg *);
NNG_DECL uint16_t    nni_mqtt_msg_get_connect_keep_alive(nni_msg *);
NNG_DECL const char *nni_mqtt_msg_get_connect_user_name(nni_msg *);
NNG_DECL const char *nni_mqtt_msg_get_connect_password(nni_msg *);
NNG_DECL mqtt_buf    nni_mqtt_msg_get_connect_client_id(nni_msg *);
NNG_DECL const char *nni_mqtt_msg_get_connect_will_topic(nni_msg *);
NNG_DECL bool        nni_mqtt_msg_get_connect_will_retain(nni_msg *);
NNG_DECL uint8_t *nni_mqtt_msg_get_connect_will_msg(nni_msg *, uint32_t *);
NNG_DECL uint8_t  nni_mqtt_msg_get_connect_will_qos(nni_msg *);
NNG_DECL property *nni_mqtt_msg_get_connect_will_property(nni_msg *);

// mqtt disconnect
NNG_DECL void nni_mqtt_msg_set_disconnect_reason_code(nng_msg *, uint8_t);
NNG_DECL void nni_mqtt_msg_set_disconnect_property(nng_msg *, property *);

// mqtt conack
NNG_DECL void      nni_mqtt_msg_set_connack_return_code(nni_msg *, uint8_t);
NNG_DECL void      nni_mqtt_msg_set_connack_flags(nni_msg *, uint8_t);
NNG_DECL void      nni_mqtt_msg_set_connack_property(nni_msg *, property *);
NNG_DECL uint8_t   nni_mqtt_msg_get_connack_return_code(nni_msg *);
NNG_DECL uint8_t   nni_mqtt_msg_get_connack_flags(nni_msg *);
NNG_DECL property *nni_mqtt_msg_get_connack_property(nni_msg *);

// mqtt publish
NNG_DECL property *nni_mqtt_msg_get_publish_property(nni_msg *msg);
NNG_DECL void nni_mqtt_msg_set_publish_property(nni_msg *msg, property *prop);
NNG_DECL void        nni_mqtt_msg_set_publish_qos(nni_msg *, uint8_t);
NNG_DECL uint8_t     nni_mqtt_msg_get_publish_qos(nni_msg *);
NNG_DECL void        nni_mqtt_msg_set_publish_retain(nni_msg *, bool);
NNG_DECL bool        nni_mqtt_msg_get_publish_retain(nni_msg *);
NNG_DECL void        nni_mqtt_msg_set_publish_dup(nni_msg *, bool);
NNG_DECL bool        nni_mqtt_msg_get_publish_dup(nni_msg *);
NNG_DECL int         nni_mqtt_msg_set_publish_topic(nni_msg *, const char *);
NNG_DECL const char *nni_mqtt_msg_get_publish_topic(nni_msg *, uint32_t *);
NNG_DECL void        nni_mqtt_msg_set_publish_packet_id(nni_msg *, uint16_t);
NNG_DECL uint16_t    nni_mqtt_msg_get_publish_packet_id(nni_msg *);
NNG_DECL void        nni_mqtt_msg_set_publish_payload(nni_msg *, uint8_t *, uint32_t);
NNG_DECL uint8_t    *nni_mqtt_msg_get_publish_payload(nni_msg *, uint32_t *);

// mqtt puback
NNG_DECL uint16_t nni_mqtt_msg_get_puback_packet_id(nni_msg *);
NNG_DECL void     nni_mqtt_msg_set_puback_packet_id(nni_msg *, uint16_t);
NNG_DECL property *nni_mqtt_msg_get_puback_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_puback_property(nni_msg *, property *);

// mqtt pubrec
NNG_DECL uint16_t nni_mqtt_msg_get_pubrec_packet_id(nni_msg *);
NNG_DECL void     nni_mqtt_msg_set_pubrec_packet_id(nni_msg *, uint16_t);
NNG_DECL property *nni_mqtt_msg_get_pubrec_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_pubrec_property(nni_msg *, property *);

// mqtt pubrel
NNG_DECL uint16_t nni_mqtt_msg_get_pubrel_packet_id(nni_msg *);
NNG_DECL void     nni_mqtt_msg_set_pubrel_packet_id(nni_msg *, uint16_t);
NNG_DECL property *nni_mqtt_msg_get_pubrel_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_pubrel_property(nni_msg *, property *);

// mqtt pubcomp
NNG_DECL uint16_t nni_mqtt_msg_get_pubcomp_packet_id(nni_msg *);
NNG_DECL void     nni_mqtt_msg_set_pubcomp_packet_id(nni_msg *, uint16_t);
NNG_DECL property *nni_mqtt_msg_get_pubcomp_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_pubcomp_property(nni_msg *, property *);

// mqtt subscribe
NNG_DECL uint16_t  nni_mqtt_msg_get_subscribe_packet_id(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_subscribe_packet_id(nni_msg *, uint16_t);
NNG_DECL nni_mqtt_topic_qos *nni_mqtt_msg_get_subscribe_topics(
    nni_msg *, uint32_t *);
NNG_DECL void      nni_mqtt_msg_set_subscribe_topics(
        nni_msg *, nni_mqtt_topic_qos *, uint32_t);
NNG_DECL property *nni_mqtt_msg_get_subscribe_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_subscribe_property(nni_msg *, property *);

// mqtt suback
NNG_DECL uint16_t  nni_mqtt_msg_get_suback_packet_id(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_suback_packet_id(nni_msg *, uint16_t);
NNG_DECL void      nni_mqtt_msg_set_suback_return_codes(
        nni_msg *, uint8_t *, uint32_t);
NNG_DECL uint8_t  *nni_mqtt_msg_get_suback_return_codes(nni_msg *, uint32_t *);
NNG_DECL property *nni_mqtt_msg_get_suback_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_suback_property(nni_msg *, property *);

// mqtt unsubscribe
NNG_DECL uint16_t  nni_mqtt_msg_get_unsubscribe_packet_id(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_unsubscribe_packet_id(nni_msg *, uint16_t);
NNG_DECL void      nni_mqtt_msg_set_unsubscribe_topics(
        nni_msg *, nni_mqtt_topic *, uint32_t);
NNG_DECL nni_mqtt_topic *nni_mqtt_msg_get_unsubscribe_topics(
    nni_msg *, uint32_t *);
NNG_DECL property *nni_mqtt_msg_get_unsubscribe_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_unsubscribe_property(nni_msg *, property *);

// mqtt unsuback
NNG_DECL void      nni_mqtt_msg_set_unsuback_packet_id(nni_msg *, uint16_t);
NNG_DECL uint16_t  nni_mqtt_msg_get_unsuback_packet_id(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_unsuback_return_codes(
        nni_msg *, uint8_t *, uint32_t);
NNG_DECL uint8_t  *nni_mqtt_msg_get_unsuback_return_codes(nni_msg *, uint32_t *);
NNG_DECL property *nni_mqtt_msg_get_unsuback_property(nni_msg *);
NNG_DECL void      nni_mqtt_msg_set_unsuback_property(nni_msg *, property *);

// mqtt disconnect
NNG_DECL void nni_mqtt_msg_set_disconnect_reason_code(nng_msg *msg, uint8_t reason_code);
NNG_DECL property *nni_mqtt_msg_get_disconnect_property(nng_msg *msg);
NNG_DECL void nni_mqtt_msg_set_disconnect_property(nng_msg *msg, property *prop);

NNG_DECL void nni_mqtt_msg_dump(nni_msg *, uint8_t *, uint32_t, bool);
// mqtt topic create/free
NNG_DECL nni_mqtt_topic *nni_mqtt_topic_array_create(size_t n);
NNG_DECL void nni_mqtt_topic_array_set(nni_mqtt_topic *, size_t, const char *);
NNG_DECL void nni_mqtt_topic_array_free(nni_mqtt_topic *, size_t);

// mqtt topic_qos create/free/set
NNG_DECL nni_mqtt_topic_qos *nni_mqtt_topic_qos_array_create(size_t);
NNG_DECL void nni_mqtt_topic_qos_array_set(nni_mqtt_topic_qos *,
        size_t, const char *, uint32_t, uint8_t, uint8_t, uint8_t, uint8_t);
NNG_DECL void nni_mqtt_topic_qos_array_free(nni_mqtt_topic_qos *, size_t);

NNG_DECL void mqtt_close_unack_msg_cb(void *, void *);

NNG_DECL uint16_t nni_msg_get_pub_pid(nni_msg *);

NNG_DECL void nni_mqtt_msg_set_connect_property(nni_msg *, property *);
NNG_DECL property* nni_mqtt_msg_get_connect_property(nni_msg *);

NNG_DECL reason_code check_properties(property *prop);
NNG_DECL property *decode_buf_properties(uint8_t *packet, uint32_t packet_len, uint32_t *pos, uint32_t *len, bool copy_value);
NNG_DECL property *decode_properties(nng_msg *msg, uint32_t *pos, uint32_t *len, bool copy_value);
NNG_DECL int      encode_properties(nng_msg *msg, property *prop, uint8_t cmd);

NNG_DECL uint32_t get_properties_len(property *prop);
NNG_DECL int      property_free(property *prop);
NNG_DECL void      property_foreach(property *prop, void (*cb)(property *));
NNG_DECL int       property_dup(property **dup, const property *src);
NNG_DECL property *property_pub_by_will(property *will_prop);

NNG_DECL property *property_alloc(void);
NNG_DECL property *property_set_value_u8(uint8_t prop_id, uint8_t value);
NNG_DECL property *property_set_value_u16(uint8_t prop_id, uint16_t value);
NNG_DECL property *property_set_value_u32(uint8_t prop_id, uint32_t value);
NNG_DECL property *property_set_value_varint(uint8_t prop_id, uint32_t value);
NNG_DECL property *property_set_value_binary(uint8_t prop_id, uint8_t *value, uint32_t len, bool copy_value);
NNG_DECL property *property_set_value_str( uint8_t prop_id, const char *value, uint32_t len, bool copy_value);
NNG_DECL property *property_set_value_strpair(uint8_t prop_id, const char *key, uint32_t key_len, const char *value, uint32_t value_len, bool copy_value);

NNG_DECL property_type_enum property_get_value_type(uint8_t prop_id);
NNG_DECL property_data *    property_get_value(property *prop, uint8_t prop_id);
NNG_DECL void               property_append(property *prop_list, property *last);
NNG_DECL int                property_value_copy(property *dest,const property *src);

/* introduced from mqtt_parser, might be duplicated */
NNG_DECL int  nni_mqtt_pubres_decode(nng_msg *msg, uint16_t *packet_id,
     uint8_t *reason_code, property **prop, uint8_t proto_ver);
NNG_DECL int  nni_mqtt_msgack_encode(nng_msg *msg, uint16_t packet_id,
     uint8_t reason_code, property *prop, uint8_t proto_ver);
NNG_DECL int  nni_mqtt_pubres_header_encode(nng_msg *msg, uint8_t cmd);

#ifdef __cplusplus
}
#endif

#endif
