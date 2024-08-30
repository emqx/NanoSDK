#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mqtt_nano_paho.h"
#include "core/nng_impl.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "mqtt_qos_db.h"

#define URI_TCP "mqtt-tcp://"
#define URI_WS "ws://"
#define URI_WSS "wss://"
#define URI_SSL "tls+mqtt-tcp://"

static const char* UTF8_char_validate(int len, const char* data);


const char *MQTTAsync_strerror(int code)
{
	static char buf[30];
	int chars = 0;

	switch (code)
	{
	case MQTTASYNC_SUCCESS:
		return "Success";
	case MQTTASYNC_FAILURE:
		return "Failure";
	case MQTTASYNC_PERSISTENCE_ERROR:
		return "Persistence error";
	case MQTTASYNC_DISCONNECTED:
		return "Disconnected";
	case MQTTASYNC_MAX_MESSAGES_INFLIGHT:
		return "Maximum in-flight messages amount reached";
	case MQTTASYNC_BAD_UTF8_STRING:
		return "Invalid UTF8 string";
	case MQTTASYNC_NULL_PARAMETER:
		return "Invalid (NULL) parameter";
	case MQTTASYNC_TOPICNAME_TRUNCATED:
		return "Topic containing NULL characters has been truncated";
	case MQTTASYNC_BAD_STRUCTURE:
		return "Bad structure";
	case MQTTASYNC_BAD_QOS:
		return "Invalid QoS value";
	case MQTTASYNC_NO_MORE_MSGIDS:
		return "Too many pending commands";
	case MQTTASYNC_OPERATION_INCOMPLETE:
		return "Operation discarded before completion";
	case MQTTASYNC_MAX_BUFFERED_MESSAGES:
		return "No more messages can be buffered";
	case MQTTASYNC_SSL_NOT_SUPPORTED:
		return "SSL is not supported";
	case MQTTASYNC_BAD_PROTOCOL:
		return "Invalid protocol scheme";
	case MQTTASYNC_BAD_MQTT_OPTION:
		return "Options for wrong MQTT version";
	case MQTTASYNC_WRONG_MQTT_VERSION:
		return "Client created for another version of MQTT";
	case MQTTASYNC_0_LEN_WILL_TOPIC:
		return "Zero length will topic on connect";
	case MQTTASYNC_COMMAND_IGNORED:
		return "Connect or disconnect command ignored";
	}

	chars = snprintf(buf, sizeof(buf), "Unknown error code %d", code);
	if (chars >= sizeof(buf))
	{
		buf[sizeof(buf) - 1] = '\0';
		nng_log_info("LOG_ERROR", "Error writing %d chars with snprintf", chars);
	}
	return buf;
}

/**
 * Validate a single UTF-8 character
 * @param len the length of the string in "data"
 * @param data the bytes to check for a valid UTF-8 char
 * @return pointer to the start of the next UTF-8 character in "data"
 */
static const char* UTF8_char_validate(int len, const char* data)
{
	int good = 0;
	int charlen = 2;
	int i, j;
	const char *rc = NULL;

	if (data == NULL)
		goto exit;	/* don't have data, can't continue */

	/* first work out how many bytes this char is encoded in */
	if ((data[0] & 128) == 0)
		charlen = 1;
	else if ((data[0] & 0xF0) == 0xF0)
		charlen = 4;
	else if ((data[0] & 0xE0) == 0xE0)
		charlen = 3;

	if (charlen > len)
		goto exit;	/* not enough characters in the string we were given */

	for (i = 0; i < ARRAY_SIZE(valid_ranges); ++i)
	{ /* just has to match one of these rows */
		if (valid_ranges[i].len == charlen)
		{
			good = 1;
			for (j = 0; j < charlen; ++j)
			{
				if (data[j] < valid_ranges[i].bytes[j].lower ||
						data[j] > valid_ranges[i].bytes[j].upper)
				{
					good = 0;  /* failed the check */
					break;
				}
			}
			if (good)
				break;
		}
	}

	if (good)
		rc = data + charlen;
	exit:
	return rc;
}

/**
 * Validate a length-delimited string has only UTF-8 characters
 * @param len the length of the string in "data"
 * @param data the bytes to check for valid UTF-8 characters
 * @return 1 (true) if the string has only UTF-8 characters, 0 (false) otherwise
 */
int UTF8_validate(int len, const char* data)
{
	const char* curdata = NULL;
	int rc = 0;

	if (len == 0 || data == NULL)
	{
		rc = 1;
		goto exit;
	}
	curdata = UTF8_char_validate(len, data);
	while (curdata && (curdata < data + len))
		curdata = UTF8_char_validate((int)(data + len - curdata), curdata);

	rc = curdata != NULL;
exit:
	return rc;
}

/**
 * Validate a null-terminated string has only UTF-8 characters
 * @param string the string to check for valid UTF-8 characters
 * @return 1 (true) if the string has only UTF-8 characters, 0 (false) otherwise
 */
int UTF8_validateString(const char* string)
{
	int rc = 0;

	if (string != NULL)
	{
		rc = UTF8_validate((int)strlen(string), string);
	}
	return rc;
}

void MQTTAsync_free(void *memory)
{
	free(memory);
}

void MQTTAsync_freeMessage(MQTTAsync_message **message)
{
	MQTTProperties_free(&(*message)->properties);
	free((*message)->payload);
	free(*message);
	*message = NULL;
}

int
MQTTProperty_getType(enum MQTTPropertyCodes value)
{
	int i, rc = -1;

	for (i = 0; i < ARRAY_SIZE(namesToTypes); ++i) {
		if (namesToTypes[i].name == value) {
			rc = namesToTypes[i].type;
			break;
		}
	}
	return rc;
}

const char *
MQTTPropertyName(enum MQTTPropertyCodes value)
{
	int         i      = 0;
	const char *result = NULL;

	for (i = 0; i < ARRAY_SIZE(nameToString); ++i) {
		if (nameToString[i].value == value) {
			result = nameToString[i].name;
			break;
		}
	}
	return result;
}

void
MQTTProperties_free(MQTTProperties *props)
{
	int i = 0;

	if (props == NULL)
		return;
	for (i = 0; i < props->count; ++i) {
		int id   = props->array[i].identifier;
		int type = MQTTProperty_getType(id);

		switch (type) {
		case MQTTPROPERTY_TYPE_BINARY_DATA:
		case MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING:
		case MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR:
			free(props->array[i].value.data.data);
			if (type == MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR)
				free(props->array[i].value.value.data);
			break;
		}
	}
	if (props->array)
		free(props->array);
	memset(props, '\0', sizeof(MQTTProperties)); /* zero all fields */
}

static
MQTTAsync_message* prepare_paho_msg(nng_msg *msg)
{
	MQTTAsync_message *mm = NULL;
	MQTTAsync_message initialized = MQTTAsync_message_initializer;
	int rc = 0;

	if ((mm = nni_alloc(sizeof(MQTTAsync_message))) == NULL)
		goto exit;
	memcpy(mm, &initialized, sizeof(MQTTAsync_message));

    uint8_t *payload;
	uint32_t payload_len;
    payload = nng_mqtt_msg_get_publish_payload(msg, &payload_len);
    if ((mm->payload = nni_alloc(payload_len)) == NULL)
    {
        nni_free(mm, payload_len);
        goto exit;
    }
    memcpy(mm->payload, payload, payload_len);
    mm->payloadlen = payload_len;
	mm->qos = nng_mqtt_msg_get_publish_qos(msg);
    mm->retained = nng_mqtt_msg_get_publish_retain(msg) == true ?  1 : 0;
	if (mm->qos > 0)
        mm->dup = nng_mqtt_msg_get_publish_dup(msg) == true ? 1 : 0;
    else {
        mm->dup = 0;
    }

	mm->msgid = nng_mqtt_msg_get_publish_packet_id(msg);

	// if (publish->MQTTVersion >= MQTTVERSION_5)
	// 	mm->properties = MQTTProperties_copy(&publish->properties);
    return mm;
exit:
    return NULL;
}

static void
recv_callback(nng_mqtt_client *client, nng_msg *msg, void *arg)
{
	nng_aio    *aio = client->recv_aio;
	uint32_t    count;
	uint8_t    *code;
	MQTTAsyncs *handle = arg;

	if (msg == NULL)
		return;
	switch (nng_mqtt_msg_get_packet_type(msg)) {
	case NNG_MQTT_PUBLISH:
        uint32_t len;
        MQTTAsync_message *mm = prepare_paho_msg(msg);
        const char *buf = nng_mqtt_msg_get_publish_topic(msg, &len);
        char *topic = nni_zalloc(len + 1);
        memcpy(topic, buf, len);
        // need to copy topic only
        handle->ma(handle->maContext, topic, len, mm);
		break;
	case NNG_MQTT_DISCONNECT:
        if (handle->disconnect.onSuccess) {
            (*(handle->publish.onSuccess))(handle->publish.context, NULL);
        }
		break;
	default:
		printf("Sending in async way is done.\n");
		break;
	}
	nng_log_debug("test", "aio mqtt result %d \n", nng_aio_result(aio));
	nng_msg_free(msg);
}

static void
send_callback(nng_mqtt_client *client, nng_msg *msg, void *arg) {
	nng_aio *        aio    = client->send_aio;
	uint32_t         count;
	uint8_t *        code;
    MQTTAsyncs *handle  = arg;

	if (msg == NULL) {
        nng_log_warn("publish", "NULL msg");
        return;
    }

    int rv = nng_aio_result(aio);
    if (rv == 0 && handle->publish.onSuccess) {
        MQTTAsync_successData data;
        data.token = rv;
        // data.alt.pub.destinationName = NULL;
        // uint32_t payload_len;
        // data.alt.pub.message.payload = nng_mqtt_msg_get_publish_payload(msg, &payload_len);
        // data.alt.pub.message.payloadlen = payload_len;
        // data.alt.pub.message.qos = nng_mqtt_msg_get_publish_qos(msg);
        // data.alt.pub.message.retained = nng_mqtt_msg_get_publish_retain(msg);
        (*(handle->publish.onSuccess))(handle->publish.context, &data);
    } else if (rv == 0 && handle->publish.onSuccess5) {
        // MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;
        // data.token = rv;
        // data.alt.pub.destinationName = command->details.pub.destinationName;
        // data.alt.pub.message.payload = command->details.pub.payload;
        // data.alt.pub.message.payloadlen = command->details.pub.payloadlen;
        // data.alt.pub.message.qos = command->details.pub.qos;
        // data.alt.pub.message.retained = command->details.pub.retained;
        // data.properties = command->properties;
        (*(handle->publish.onSuccess5))(handle->publish.context, NULL);
    } else if (rv != 0 && handle->publish.onFailure5) {
        MQTTAsync_failureData5 data;
        data.token = handle->publish.token;
        data.code = rv;
        data.message = NULL;
        data.packet_type = PUBLISH;
        (*(handle->publish.onFailure5))(handle->publish.context, &data);
    } else if (rv != 0 && handle->publish.onFailure) {
        MQTTAsync_failureData data;
        data.token = handle->publish.token;
        data.code = rv;
        data.message = NULL;
        (*(handle->publish.onFailure))(handle->publish.context, &data);
    }

	switch (nng_mqtt_msg_get_packet_type(msg)) {
	case NNG_MQTT_PUBLISH:
        // nng_msg_free(msg);
        break;
    case NNG_MQTT_SUBACK:
		code = nng_mqtt_msg_get_suback_return_codes(msg, &count);
		nng_log_info("Send callback", "SUBACK reason codes are: ");
		for (int i = 0; i < (int)count; ++i) {
            nng_log_info("Topic result", "[%d] ", code[i]);
            			
            if (code[i] <= 2 && handle->subscribe.onSuccess) {
                MQTTAsync_successData data;
                data.alt.qos = code[i];
                data.token = handle->subscribe.token;
                (*(handle->subscribe.onSuccess))(handle->subscribe.context, &data);
            } else if (code[i] > 2 && handle->publish.onFailure) {
                MQTTAsync_failureData data;
                data.code = code[i];
                data.token = handle->subscribe.token;
                (*(handle->subscribe.onFailure))(handle->subscribe.context, &data);
            }
        }

		break;
	case NNG_MQTT_UNSUBACK:
		code = nng_mqtt_msg_get_unsuback_return_codes(
		    msg, &count);
		printf("UNSUBACK reason codes are: ");
		for (int i = 0; i < (int)count; ++i)
			printf("[%d] ", code[i]);
		break;
	case NNG_MQTT_PUBACK:
        uint16_t token = nng_mqtt_msg_get_puback_packet_id(msg);
		nng_log_info("TRANSPORT", "PUBACK of %d received",
                token);
        if (handle->dc)
            handle->dc(handle->dcContext, token);
		break;
	case NNG_MQTT_PUBCOMP:
        uint16_t token2 = nng_mqtt_msg_get_pubcomp_packet_id(msg);
        nng_log_info("TRANSPORT", "PUBACK of %d received",
                token2);
        if (handle->dc)
		    handle->dc(handle->dcContext, token2);
		break;
	default:
		printf("Sending in async way is done.\n");
		break;
	}
	nng_log_debug("Send callback", "aio mqtt result %d \n", nng_aio_result(aio));
	nng_msg_free(msg);
}

/**
 * ATTENTION: Forbidden direct touch with nng_mqtt_client
 *            if you decide to go with Paho Async
 * */
int
MQTTAsync_setCallbacks(MQTTAsync handle, void *context,
    MQTTAsync_connectionLost *cl, MQTTAsync_messageArrived *ma,
    MQTTAsync_deliveryComplete *dc)
{
	int         rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs *m  = handle;

	if (m == NULL || ma == NULL || m->nanosdk_client != NULL)
		rc = MQTTASYNC_FAILURE;
	else {
		m->clContext = m->maContext = m->dcContext = context;
		m->cl                                      = cl;
		m->ma                                      = ma;
		m->dc                                      = dc;
		nng_mqtt_client *client = nng_mqtt_paho_client_alloc(
		    *m->sock, &send_callback, &recv_callback, m);
		m->nanosdk_client = client;
	}

	return rc;
}

int
MQTTAsync_createWithOptions(MQTTAsync *handle, const char *serverURI,
    const char *clientId, int persistence_type, void *persistence_context,
    MQTTAsync_createOptions *options)
{
    int rc = 0;
	MQTTAsyncs *m = NULL;
	if (serverURI == NULL || clientId == NULL) {
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}

	if (!UTF8_validateString(clientId))
	{
		rc = MQTTASYNC_BAD_UTF8_STRING;
		goto exit;
	}

	if (strlen(clientId) == 0 &&
	    persistence_type == MQTTCLIENT_PERSISTENCE_DEFAULT) {
		rc = MQTTASYNC_PERSISTENCE_ERROR;
		goto exit;
	}
	if (persistence_type != MQTTCLIENT_PERSISTENCE_NONE) {
		rc = MQTTASYNC_PERSISTENCE_ERROR;
		goto exit;
	}

	if (strstr(serverURI, "://") != NULL) {
		if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) != 0
#if defined(OPENSSL)
		    && strncmp(URI_SSL, serverURI, strlen(URI_SSL)) != 0 &&
		    strncmp(URI_WSS, serverURI, strlen(URI_WSS)) != 0
#endif
		) {
			rc = MQTTASYNC_BAD_PROTOCOL;
			goto exit;
		}
	}
	if (options &&
	    (strncmp(options->struct_id, "MQCO", 4) != 0 ||
	        options->struct_version < 0 || options->struct_version > 2)) {
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}
    if (options &&
        options->MQTTVersion > MQTTVERSION_5)
	{
		rc = MQTTASYNC_WRONG_MQTT_VERSION;
		goto exit;
	}
	// To enable SCRAM open a MQTTv5 socket
    // here starts NanoSDK

    	// To enable SCRAM open a MQTTv5 socket

    if ((m = nni_zalloc(sizeof(MQTTAsyncs))) == NULL)
	{
		rc = MQTTASYNC_FAILURE;
		goto exit;
	}
    memset(m, '\0', sizeof(MQTTAsyncs));
    *handle = m;
    if (options)
        m->MQTTVersion = options->MQTTVersion;
    else
        m->MQTTVersion = MQTTVERSION_3_1_1;
    m->sock   = (nng_socket *) nng_alloc(sizeof(nng_socket));
    if (options && options->MQTTVersion >= MQTTVERSION_5) {
        if ((rc = nng_mqttv5_client_open(m->sock)) != 0) {
            rc = NNG_ENOMEM;
		    goto exit;
        }
    } else {
        if ((rc = nng_mqtt_client_open(m->sock)) != 0) {
            rc = NNG_ENOMEM;
		    goto exit;
        }
    }
    m->dialer   = (nng_dialer *) nng_alloc(sizeof(nng_dialer));
    if ((rc = nng_dialer_create(m->dialer, *m->sock, serverURI))) {
		nng_log_warn("dialer", "Dialer init failed!");
        rc = MQTTASYNC_FAILURE;
    }

exit:
	return rc;
}

int
MQTTAsync_create(MQTTAsync *handle, const char *serverURI,
    const char *clientId, int persistence_type, void *persistence_context)
{
	// MQTTAsync_init_rand();

	return MQTTAsync_createWithOptions(handle, serverURI, clientId,
	    persistence_type, persistence_context, NULL);
}

static void
connect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
    MQTTAsyncs *m = arg;
	int reason;
	// get connect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_CONNECT_REASON, &reason);
	// get property for MQTT V5
	// property *prop;
	// nng_pipe_get_ptr(p, NNG_OPT_MQTT_CONNECT_PROPERTY, &prop);
    nng_log_warn("NanoSDK-Internal!", "Pipe is Connected");
    if (reason == MQTTREASONCODE_SUCCESS) {
        m->shouldBeConnected = 1;
        if (m->connect.onSuccess) {
            MQTTAsync_successData data;
            memset(&data, '\0', sizeof(data));
            if ((m->serverURIcount > 0) && (m->connect.details.conn.currentURI < m->serverURIcount))
                data.alt.connect.serverURI = m->serverURIs[m->connect.details.conn.currentURI];
            else
                data.alt.connect.serverURI = m->serverURI;
            data.alt.connect.MQTTVersion = m->connect.details.conn.MQTTVersion;
            // data.alt.connect.sessionPresent = sessionPresent;
            (*(m->connect.onSuccess))(m->connect.context, &data);
            /* Null out callback pointers so they aren't accidentally called again */
            m->connect.onSuccess = NULL;
            m->connect.onFailure = NULL;
        } else if (m->connect.onSuccess5) {
            MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;
            if (m->serverURIcount > 0)
                data.alt.connect.serverURI = m->serverURIs[m->connect.details.conn.currentURI];
            else
                data.alt.connect.serverURI = m->serverURI;
            data.alt.connect.MQTTVersion = m->connect.details.conn.MQTTVersion;
            data.reasonCode = reason;
            // data.alt.connect.sessionPresent = sessionPresent;
            // data.properties = connack->properties;
            // data.reasonCode = connack->rc;
            (*(m->connect.onSuccess5))(m->connect.context, &data);
            /* Null out callback pointers so they aren't accidentally called again */
            m->connect.onSuccess5 = NULL;
            m->connect.onFailure5 = NULL;
        }
        if (m->connected) {
            (*(m->connected))(m->connected_context, "MQTTREASONCODE_SUCCESS");
        }
    } else {
        // TODO test
        if (m->connect.onFailure)
		{
			MQTTAsync_failureData data;
			data.token = 0;
			data.code = reason;
			data.message = NULL;
			nng_log_info("CONNECT", "Calling connect failure for client");
			(*(m->connect.onFailure))(m->connect.context, &data);
			/* Null out callback pointers so they aren't accidentally called again */
			m->connect.onFailure = NULL;
			m->connect.onSuccess = NULL;
		}
		else if (m->connect.onFailure5)
		{
			MQTTAsync_failureData5 data = MQTTAsync_failureData5_initializer;
			data.token = 0;
			data.code = reason;
			data.message = NULL;
			nng_log_info("CONNECT", "Calling connect failure for client");
            (*(m->connect.onFailure5))(m->connect.context, &data);
			/* Null out callback pointers so they aren't accidentally called again */
			m->connect.onFailure5 = NULL;
			m->connect.onSuccess5 = NULL;
		}
    }
}

static void
disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
    MQTTAsyncs *m = arg;
	int reason = 0;
	// get connect reason
	nng_pipe_get_int(p, NNG_OPT_MQTT_DISCONNECT_REASON, &reason);
	// property *prop;
	// nng_pipe_get_ptr(p, NNG_OPT_MQTT_DISCONNECT_PROPERTY, &prop);
	// nng_socket_get?
	printf("%s: disconnected! RC [%d] \n", __FUNCTION__, reason);
    m->shouldBeConnected = 0;
	(void) ev;
    if (m->cl)
    {
        m->cl(m->clContext, NULL);
    }
}

int MQTTAsync_connect(MQTTAsync handle, const MQTTAsync_connectOptions *options)
{
    int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs *m = handle;
    nng_socket *sock = m->sock;
    nng_dialer *dialer = m->dialer;

    // create a CONNECT message
	/* CONNECT */
	nng_msg *connmsg = NULL;
    nng_dialer_get_ptr(*dialer, NNG_OPT_MQTT_CONNMSG, (void **)&connmsg);
    if (connmsg != NULL) {
        nng_log_warn("MQTTAsync_connect", "Auto reconnect is enalbed by default!");
        return MQTTASYNC_FAILURE;
    }
	nng_mqtt_msg_alloc(&connmsg, 0);
	nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
	if (options == NULL)
	{
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}
	if (strncmp(options->struct_id, "MQTC", 4) != 0 || options->struct_version < 0 || options->struct_version > 8)
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}
#ifdef OPENSSL
	// if (m->ssl && options->ssl == NULL)
	// {
	// 	rc = MQTTASYNC_NULL_PARAMETER;
	// 	goto exit;
	// }
	// To enable SCRAM, version must be MQTTv5
#endif
	if (options->MQTTVersion >= MQTTVERSION_5)
		nng_mqtt_msg_set_connect_proto_version(
		    connmsg, MQTT_PROTOCOL_VERSION_v5);
	else
		nng_mqtt_msg_set_connect_proto_version(
		    connmsg, MQTT_PROTOCOL_VERSION_v311);
    if (options->keepAliveInterval < 0 || options->keepAliveInterval > 0xFFFF)
        goto exit;
	nng_mqtt_msg_set_connect_keep_alive(
	    connmsg, (uint16_t) options->keepAliveInterval);
    if (options->username)
	    nng_mqtt_msg_set_connect_user_name(connmsg, options->username);
	if (options->password)
        nng_mqtt_msg_set_connect_password(connmsg, options->password);
	else if (options->struct_version >= 5 && options->binarypwd.data) {
        char *passwd = nni_zalloc(options->binarypwd.len + 1);
        memcpy((void *)passwd, options->binarypwd.data, options->binarypwd.len);
		nng_mqtt_msg_set_connect_password(connmsg, passwd);
        nni_free(passwd, options->binarypwd.len + 1);
	}
    if (options->retryInterval > 0) {
        nng_duration retryInterval = options->retryInterval * 1000;
	    nng_socket_set_ms(*m->sock, NNG_OPT_MQTT_RETRY_INTERVAL, retryInterval);
    }

	if (options->will) /* check validity of will options structure */
	{
		if (strncmp(options->will->struct_id, "MQTW", 4) != 0 || (options->will->struct_version != 0 && options->will->struct_version != 1))
		{
			rc = MQTTASYNC_BAD_STRUCTURE;
			goto exit;
		}
		if (options->will->qos < 0 || options->will->qos > 2)
		{
			rc = MQTTASYNC_BAD_QOS;
			goto exit;
		}
		if (options->will->topicName == NULL)
		{
			rc = MQTTASYNC_NULL_PARAMETER;
			goto exit;
		}
		else if (strlen(options->will->topicName) == 0)
		{
			rc = MQTTASYNC_0_LEN_WILL_TOPIC;
			goto exit;
		}
	}
	if (options->struct_version != 0 && options->ssl) /* check validity of SSL options structure */
	{
		if (strncmp(options->ssl->struct_id, "MQTS", 4) != 0 || options->ssl->struct_version < 0 || options->ssl->struct_version > 5)
		{
			rc = MQTTASYNC_BAD_STRUCTURE;
			goto exit;
		}
	}
	if (options->MQTTVersion >= MQTTVERSION_5)
	{
		rc = MQTTASYNC_WRONG_MQTT_VERSION;
		goto exit;
	}
	if (options->MQTTVersion >= MQTTVERSION_5 && options->struct_version < 6)
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}
	if (options->MQTTVersion >= MQTTVERSION_5 && options->cleansession != 0)
	{
		rc = MQTTASYNC_BAD_MQTT_OPTION;
		goto exit;
	}
	if (options->MQTTVersion < MQTTVERSION_5 && options->struct_version >= 6)
	{
		if (options->cleanstart != 0 || options->onFailure5 || options->onSuccess5 ||
			options->connectProperties || options->willProperties)
		{
			rc = MQTTASYNC_BAD_MQTT_OPTION;
			goto exit;
		}
	}

	m->connect.onSuccess = options->onSuccess;
	m->connect.onFailure = options->onFailure;
	if (options->struct_version >= 6)
	{
		m->connect.onSuccess5 = options->onSuccess5;
		m->connect.onFailure5 = options->onFailure5;
	}
	m->connect.context = options->context;  // We need to convey async handle to next cb
    m->connect.context = m;
	m->connectTimeout = options->connectTimeout;

    nng_mqtt_msg_set_connect_clean_session(connmsg, options->cleansession);

	if (options->maxInflight > 0) {
        nni_lmq_resize((nni_lmq *)m->nanosdk_client->msgq, (size_t)options->maxInflight);
    }
	if (options->struct_version >= 4)
	{
        nng_duration duration = (nng_duration) options->maxRetryInterval * 1000;
	    nng_dialer_set(*m->dialer, NNG_OPT_MQTT_RECONNECT_BACKOFF_MAX, &duration, sizeof(nng_duration));
	}
	if (options->struct_version >= 8)
	{
		if (options->httpProxy || options->httpsProxy)
            nng_log_warn("we dont support paho http proxy!", "");
	}

    if (options->will && (options->will->struct_version == 0 || options->will->struct_version == 1)) {
		if (options->will->message || (options->will->struct_version == 1 && options->will->payload.data)) {
			if (options->will->struct_version == 1 && options->will->payload.data)
			{
                nng_mqtt_msg_set_connect_will_msg(
	                connmsg, (uint8_t *)options->will->payload.data, (uint32_t)options->will->payload.len);
			} else {
				nng_mqtt_msg_set_connect_will_msg(
	                connmsg, (uint8_t *)options->will->message, strlen(options->will->message));
			}
            nng_mqtt_msg_set_connect_will_topic(connmsg, options->will->topicName);
            nng_mqtt_msg_set_connect_will_qos(connmsg, options->will->qos);
            bool retained = options->will->retained == 1 ? true : false;
            nng_mqtt_msg_set_connect_will_retain(connmsg, retained);
		} else {
			// no will msg is set
		}
	}
#ifdef OPENSSL
    if (options->struct_version != 0 && options->ssl)
	{
        if (m->MQTTVersion >= MQTTVERSION_5) {
            // Enable scram
	        bool enable_scram = true;
	        nng_dialer_set(*m->dialer, NNG_OPT_MQTT_ENABLE_SCRAM, &enable_scram, sizeof(bool));
        }
		if ((m->c->sslopts = malloc(sizeof(MQTTClient_SSLOptions))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		memset(m->c->sslopts, '\0', sizeof(MQTTClient_SSLOptions));
		m->c->sslopts->struct_version = options->ssl->struct_version;
		if (options->ssl->trustStore)
			m->c->sslopts->trustStore = MQTTStrdup(options->ssl->trustStore);
		if (options->ssl->keyStore)
			m->c->sslopts->keyStore = MQTTStrdup(options->ssl->keyStore);
		if (options->ssl->privateKey)
			m->c->sslopts->privateKey = MQTTStrdup(options->ssl->privateKey);
		if (options->ssl->dprivateKey)
			m->c->sslopts->dprivateKey = MQTTStrdup(options->ssl->dprivateKey);
		if (options->ssl->dkeyStore)
			m->c->sslopts->dkeyStore = MQTTStrdup(options->ssl->dkeyStore);
		if (options->ssl->privateKeyPassword)
			m->c->sslopts->privateKeyPassword = MQTTStrdup(options->ssl->privateKeyPassword);
		if (options->ssl->enabledCipherSuites)
			m->c->sslopts->enabledCipherSuites = MQTTStrdup(options->ssl->enabledCipherSuites);
		m->c->sslopts->enableServerCertAuth = options->ssl->enableServerCertAuth;
		if (m->c->sslopts->struct_version >= 1)
			m->c->sslopts->sslVersion = options->ssl->sslVersion;
		if (m->c->sslopts->struct_version >= 2)
		{
			m->c->sslopts->verify = options->ssl->verify;
			if (options->ssl->CApath)
				m->c->sslopts->CApath = MQTTStrdup(options->ssl->CApath);
		}
		if (m->c->sslopts->struct_version >= 3)
		{
			m->c->sslopts->ssl_error_cb = options->ssl->ssl_error_cb;
			m->c->sslopts->ssl_error_context = options->ssl->ssl_error_context;
		}
		if (m->c->sslopts->struct_version >= 4)
		{
			m->c->sslopts->ssl_psk_cb = options->ssl->ssl_psk_cb;
			m->c->sslopts->ssl_psk_context = options->ssl->ssl_psk_context;
			m->c->sslopts->disableDefaultTrustStore = options->ssl->disableDefaultTrustStore;
		}
		if (m->c->sslopts->struct_version >= 5)
		{
			m->c->sslopts->protos = options->ssl->protos;
			m->c->sslopts->protos_len = options->ssl->protos_len;
		}
	}
#else
	// if (options->struct_version != 0 && options->ssl)
	// {
	// 	rc = MQTTASYNC_SSL_NOT_SUPPORTED;
	// 	goto exit;
	// }
#endif
    if (m->connectProps)
	{
		MQTTProperties_free(m->connectProps);
		nni_free(m->connectProps, sizeof(m->connectProps));
		m->connectProps = NULL;
        nng_log_warn("paho property is not support!", "");
	}
	if (m->willProps)
	{   // MQTTProperties_copy
		MQTTProperties_free(m->willProps);
		nni_free(m->willProps, sizeof(m->willProps));
		m->willProps = NULL;
        nng_log_warn("paho property is not support!", "");
	}
    // property * p = mqtt_property_alloc();
	// property *p1 = mqtt_property_set_value_u32(MAXIMUM_PACKET_SIZE, 120);
	// property *p2 = mqtt_property_set_value_u16(TOPIC_ALIAS_MAXIMUM, 65535);
	// mqtt_property_append(p, p1);
	// mqtt_property_append(p, p2);
	// nng_mqtt_msg_set_connect_property(connmsg, p);

	// property *will_prop = mqtt_property_alloc();
	// property *will_up   = mqtt_property_set_value_strpair(USER_PROPERTY,
    //         "user", strlen("user"), "pass", strlen("pass"), true);
	// mqtt_property_append(will_prop, will_up);
	// nng_mqtt_msg_set_connect_will_property(connmsg, will_prop);
	nng_mqtt_set_connect_cb(*sock, connect_cb, m);
	nng_mqtt_set_disconnect_cb(*sock, disconnect_cb, m);
    if (m->MQTTVersion >= MQTTVERSION_5)
        nng_mqttv5_msg_encode(connmsg);
    else
        nng_mqtt_msg_encode(connmsg);
	if (nng_dialer_set_ptr(*dialer, NNG_OPT_MQTT_CONNMSG, connmsg) != 0) {
        nng_log_warn("nng_dialer_set_ptr", "set failed");
        rc = MQTTASYNC_FAILURE;
        goto exit;
    }
	nng_dialer_start(*dialer, NNG_FLAG_NONBLOCK);
    return rc;
exit:
    nng_msg_free(connmsg);
	return rc;
}

int MQTTAsync_subscribeMany(MQTTAsync handle, int count, char *const *topic, const int *qos, MQTTAsync_responseOptions *response)
{
	MQTTAsyncs *m = handle;
    nng_mqtt_client *client = m->nanosdk_client;
	int i = 0;
	int rc = MQTTASYNC_SUCCESS;

    if (count ==0 || topic == NULL) {
        return MQTTASYNC_FAILURE;
    }
    if (m->MQTTVersion >= MQTTVERSION_5 && count > 1 && (count != response->subscribeOptionsCount && response->subscribeOptionsCount != 0))
		rc = MQTTASYNC_BAD_MQTT_OPTION;
	else if (response)
	{
		if (m->MQTTVersion >= MQTTVERSION_5)
		{
			if (response->struct_version == 0 || response->onFailure || response->onSuccess)
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
		else if (m->MQTTVersion < MQTTVERSION_5)
		{
			if (response->struct_version >= 1 && (response->onFailure5 || response->onSuccess5))
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
	}
	if (rc != MQTTASYNC_SUCCESS)
		return rc;

	nng_mqtt_topic_qos *topic_qos =
	    nng_mqtt_topic_qos_array_create((size_t)count);
	for (size_t i = 0; i < (size_t)count; ++i) {
		nng_mqtt_topic_qos_array_set(topic_qos, i,
		    topic[i], strlen(topic[i]), qos[i], 1, 1, 0);
		nng_log_info("Performing subscription",
		    "Bridge client subscribed topic %s (qos %d rap %d rh %d).",
		    topic[i], qos[i], 1, 1);
	}

	if (response) {
		m->subscribe.onSuccess  = response->onSuccess;
		m->subscribe.onFailure  = response->onFailure;
		m->subscribe.onSuccess5 = response->onSuccess5;
		m->subscribe.onFailure5 = response->onFailure5;
		m->subscribe.context    = response->context;
		response->token         = m->subscribe.token; // msg id
		if (m->MQTTVersion >= MQTTVERSION_5) {
			// if (response->properties) {
				nng_log_warn("Incompatible paho API!",
				    "preopertuy of paho is not support");
				// MQTTProperties_free(response->properties);
			// }
		}
	}
	nng_mqtt_subscribe_async(client, topic_qos, count, NULL);
    nng_mqtt_topic_qos_array_free(topic_qos, count);
	return rc;
}

int MQTTAsync_subscribe(MQTTAsync handle, const char *topic, int qos, MQTTAsync_responseOptions *response)
{
	int rc = 0;
	rc = MQTTAsync_subscribeMany(handle, 1, (char *const *)(&topic), &qos, response);
	return rc;
}

int MQTTAsync_setConnected(MQTTAsync handle, void *context, MQTTAsync_connected *connected)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs *m = handle;

	if (m == NULL)
		rc = MQTTASYNC_FAILURE;
	else
	{
        if (context == NULL)
            m->connected_context = m;
        else
		    m->connected_context = context;
		m->connected = connected;
	}

	return rc;
}

int MQTTAsync_unsubscribeMany(MQTTAsync handle, int count, char *const *topic, MQTTAsync_responseOptions *response)
{
    return MQTTASYNC_FAILURE;
}


int MQTTAsync_send(MQTTAsync handle, const char *destinationName, int payloadlen, const void *payload,
				   int qos, int retained, MQTTAsync_responseOptions *response)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs *m = handle;
	nng_mqtt_client *client = m->nanosdk_client;

	if (m == NULL)
		return MQTTASYNC_FAILURE;
	if (!UTF8_validateString(destinationName))
		rc = MQTTASYNC_BAD_UTF8_STRING;
	else if (qos < 0 || qos > 2)
		rc = MQTTASYNC_BAD_QOS;
	if (response)
	{
		if (m->MQTTVersion >= MQTTVERSION_5)
		{
			if (response->struct_version == 0 || response->onFailure || response->onSuccess)
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
		else if (m->MQTTVersion < MQTTVERSION_5)
		{
			if (response->struct_version >= 1 && (response->onFailure5 || response->onSuccess5))
				rc = MQTTASYNC_BAD_MQTT_OPTION;
		}
	}
    if (response) {
		m->publish.onSuccess = response->onSuccess;
		m->publish.onFailure = response->onFailure;
		m->publish.onSuccess5 = response->onSuccess5;
		m->publish.onFailure5 = response->onFailure5;
		m->publish.context = response->context;
		response->token = m->publish.token;
		if (m->MQTTVersion >= MQTTVERSION_5) {
            nng_log_warn("Paho", "Publish property with PAHO API is not support");
			// pub->command.properties = MQTTProperties_copy(&response->properties);
            // MQTTProperties_free(&response->properties);
        }
	}
	if (rc != MQTTASYNC_SUCCESS)
		goto exit;

    nng_msg *pubmsg;
	nng_mqtt_msg_alloc(&pubmsg, 0);
	nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_dup(pubmsg, 0);
	nng_mqtt_msg_set_publish_qos(pubmsg, qos);
	nng_mqtt_msg_set_publish_retain(pubmsg, retained);
	nng_mqtt_msg_set_publish_payload(
	    pubmsg, (uint8_t *) payload, payloadlen);
	nng_mqtt_msg_set_publish_topic(pubmsg, destinationName);
    if (nng_mqtt_publish_async(client, pubmsg) < 0)
        rc = MQTTASYNC_FAILURE;
exit:
	return rc;
}

int MQTTAsync_sendMessage(MQTTAsync handle, const char *destinationName, const MQTTAsync_message *message,
						  MQTTAsync_responseOptions *response)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs *m = handle;

	if (message == NULL)
	{
		rc = MQTTASYNC_NULL_PARAMETER;
		goto exit;
	}
	if (strncmp(message->struct_id, "MQTM", 4) != 0 ||
		(message->struct_version != 0 && message->struct_version != 1))
	{
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}

	if (m->MQTTVersion >= MQTTVERSION_5 && response)
		response->properties = message->properties;

	rc = MQTTAsync_send(handle, destinationName, message->payloadlen, message->payload,
						message->qos, message->retained, response);
exit:
	return rc;
}

int MQTTAsync_disconnect(MQTTAsync handle, const MQTTAsync_disconnectOptions *options)
{
    MQTTAsyncs *m = handle;
	int rc = MQTTASYNC_SUCCESS;

	if (options != NULL && (strncmp(options->struct_id, "MQTD", 4) != 0 || options->struct_version < 0 || options->struct_version > 1))
		return MQTTASYNC_BAD_STRUCTURE;
	if (m == NULL)
	{
		return MQTTASYNC_FAILURE;
	}
    if (m->shouldBeConnected == 0)
	{
		return MQTTASYNC_DISCONNECTED;
	}
    if (options)
	{
		m->disconnect.onSuccess = options->onSuccess;
		m->disconnect.onFailure = options->onFailure;
		m->disconnect.onSuccess5 = options->onSuccess5;
		m->disconnect.onFailure5 = options->onFailure5;
		m->disconnect.context = options->context;
		m->disconnect.details.dis.timeout = options->timeout;
		if (m->MQTTVersion >= MQTTVERSION_5 && options->struct_version >= 1)
		{
            nng_log_warn("Paho", "Disconnect property with PAHO API is not support");
			// dis->command.properties = MQTTProperties_copy(&options->properties);
			// dis->command.details.dis.reasonCode = options->reasonCode;
		}
	}
	m->disconnect.type = DISCONNECT;
	m->disconnect.details.dis.internal = 0; // ???
    // TODO clone disconnect msg to support on disconnect
    nng_mqtt_disconnect(m->sock, options->reasonCode, NULL);
    m->shouldBeConnected = 0;
    return rc;
}

void MQTTAsync_destroy(MQTTAsync *handle)
{
	MQTTAsyncs *m = *handle;
    nng_socket *sock = m->sock;
    if (m->shouldBeConnected = 1)
        nng_mqtt_disconnect(m->sock, MQTTREASONCODE_NORMAL_DISCONNECTION, NULL);
    nng_close(*sock);

    // free objects
    nng_mqtt_client_free(m->nanosdk_client, true);
    nng_free(m->sock, sizeof(nng_socket));
    m->sock = NULL;
    nng_free(m->dialer, sizeof(nng_dialer));
    m->dialer = NULL;
    nng_free(m, sizeof(MQTTAsyncs));
}