#include "mqtt_nano_paho.h"
#include "core/nng_impl.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "mqtt_qos_db.h"
#include <string.h>

#define URI_TCP "tcp://"
#define URI_WS "ws://"
#define URI_WSS "wss://"
#define URI_SSL "ssl://"

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

// 	if (rc == 0) /* if message was not delivered, queue it up */
// 	{
// 		qEntry *qe = malloc(sizeof(qEntry));

// 		if (!qe)
// 			goto exit;
// 		qe->msg = mm;
// 		qe->topicName = publish->topic;
// 		qe->topicLen = publish->topiclen;
// 		ListAppend(client->messageQueue, qe, sizeof(qe) + sizeof(mm) + mm->payloadlen + strlen(qe->topicName) + 1);
// #if !defined(NO_PERSISTENCE)
// 		if (client->persistence)
// 			MQTTPersistence_persistQueueEntry(client, (MQTTPersistence_qEntry *)qe);
// #endif
// 	}
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
        nng_msg *pmsg = msg;
        uint32_t len;
        MQTTAsync_message *mm = prepare_paho_msg(msg);
        handle->ma(handle->maContext, nng_mqtt_msg_get_publish_topic(msg, &len), len, mm);
		break;
	case NNG_MQTT_DISCONNECT:
		break;
	default:
		printf("Sending in async way is done.\n");
		break;
	}
	printf("Aio mqtt result %d \n", nng_aio_result(aio));
	nng_msg_free(msg);
}

static void
send_callback(nng_mqtt_client *client, nng_msg *msg, void *arg) {
	nng_aio *        aio    = client->send_aio;
	uint32_t         count;
	uint8_t *        code;
    MQTTAsyncs *handle  = arg;

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
        handle->dc(handle->dcContext, NULL);
		break;
	case NNG_MQTT_PUBCOMP:
		handle->dc(handle->dcContext, NULL);
		break;
	default:
		printf("Sending in async way is done.\n");
		break;
	}
	printf("Aio mqtt result %d \n", nng_aio_result(aio));
	// printf("suback %d \n", *code);
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

	// if (!UTF8_validateString(clientId))
	// {
	// 	rc = MQTTASYNC_BAD_UTF8_STRING;
	// 	goto exit;
	// }

	if (strlen(clientId) == 0 &&
	    persistence_type == MQTTCLIENT_PERSISTENCE_DEFAULT) {
		rc = MQTTASYNC_PERSISTENCE_ERROR;
		goto exit;
	}
	if (persistence_type != MQTTCLIENT_PERSISTENCE_NONE) {
		rc = MQTTASYNC_PERSISTENCE_ERROR;
		goto exit;
	}

// 	if (strstr(serverURI, "://") != NULL) {
// 		if (strncmp(URI_TCP, serverURI, strlen(URI_TCP)) != 0 &&
// 		    strncmp(URI_WS, serverURI, strlen(URI_WS)) != 0
// #if defined(OPENSSL)
// 		    && strncmp(URI_SSL, serverURI, strlen(URI_SSL)) != 0 &&
// 		    strncmp(URI_WSS, serverURI, strlen(URI_WSS)) != 0
// #endif
// 		) {
// 			rc = MQTTASYNC_BAD_PROTOCOL;
// 			goto exit;
// 		}
// 	}
	if (options &&
	    (strncmp(options->struct_id, "MQCO", 4) != 0 ||
	        options->struct_version < 0 || options->struct_version > 2)) {
		rc = MQTTASYNC_BAD_STRUCTURE;
		goto exit;
	}
    if (options &&
        options->MQTTVersion >= MQTTVERSION_5)
	{
		rc = MQTTASYNC_WRONG_MQTT_VERSION;
		goto exit;
	}
	// if (options) {
	// 	if ((m->createOptions =
	// 	            malloc(sizeof(MQTTAsync_createOptions))) == NULL) {
	// 		rc = PAHO_MEMORY_ERROR;
	// 		goto exit;
	// 	}
	// 	memcpy(m->createOptions, options,
	// 	    sizeof(MQTTAsync_createOptions));
	// 	if (options->struct_version > 0)
	// 		m->c->MQTTVersion = options->MQTTVersion;
	// }
	// if (options->MQTTVersion ==)
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
    m->sock   = (nng_socket *) nng_alloc(sizeof(nng_socket));
    if (options && options->MQTTVersion == MQTTVERSION_5) {
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
		goto exit;
	}
    if (options && options->MQTTVersion == MQTTVERSION_5) {
        // Enable scram
	    bool enable_scram = true;
	    nng_dialer_set(*m->dialer, NNG_OPT_MQTT_ENABLE_SCRAM, &enable_scram, sizeof(bool));
    }
    nng_duration retry = 10000;
	nng_socket_set_ms(*m->sock, NNG_OPT_MQTT_RETRY_INTERVAL, retry);

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
	printf("%s: connected! RC [%d] \n", __FUNCTION__, reason);
    if (m->connect.onSuccess)
        {
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
        }
        else if (m->connect.onSuccess5)
        {
            MQTTAsync_successData5 data = MQTTAsync_successData5_initializer;
            if (m->serverURIcount > 0)
                data.alt.connect.serverURI = m->serverURIs[m->connect.details.conn.currentURI];
            else
                data.alt.connect.serverURI = m->serverURI;
            data.alt.connect.MQTTVersion = m->connect.details.conn.MQTTVersion;
            // data.alt.connect.sessionPresent = sessionPresent;
            // data.properties = connack->properties;
            // data.reasonCode = connack->rc;
            (*(m->connect.onSuccess5))(m->connect.context, &data);
            /* Null out callback pointers so they aren't accidentally called again */
            m->connect.onSuccess5 = NULL;
            m->connect.onFailure5 = NULL;
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
	(void) ev;
    m->cl(m->clContext, "fucku");
}

int MQTTAsync_connect(MQTTAsync handle, const MQTTAsync_connectOptions *options)
{
    int rc;
	MQTTAsyncs *m = handle;
    nng_socket *sock = m->sock;
    nng_dialer *dialer = m->dialer;
	
    // create a CONNECT message
	/* CONNECT */
	nng_msg *connmsg;
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
	if (options->MQTTVersion == MQTTVERSION_5)
		nng_mqtt_msg_set_connect_proto_version(
		    connmsg, MQTT_PROTOCOL_VERSION_v5);
	else
		nng_mqtt_msg_set_connect_proto_version(
		    connmsg, MQTT_PROTOCOL_VERSION_v311);
    if (options->keepAliveInterval < 0 || options->keepAliveInterval > 0xFFFF)
        goto exit;
	nng_mqtt_msg_set_connect_keep_alive(
	    connmsg, (uint16_t) options->keepAliveInterval);

	nng_mqtt_msg_set_connect_user_name(connmsg, options->username);
	nng_mqtt_msg_set_connect_password(connmsg, options->password);
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
	m->connect.context = options->context;
	m->connectTimeout = options->connectTimeout;

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
	nng_mqtt_set_connect_cb(*sock, connect_cb, m);
	nng_mqtt_set_disconnect_cb(*sock, disconnect_cb, m);
	nng_dialer_set_ptr(*dialer, NNG_OPT_MQTT_CONNMSG, connmsg);
	nng_dialer_start(*dialer, NNG_FLAG_NONBLOCK);
    return;
exit:
    nng_msg_free(connmsg);
	return MQTTASYNC_BAD_PROTOCOL;
}