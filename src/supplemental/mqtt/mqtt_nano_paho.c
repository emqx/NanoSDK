#include "mqtt_nano_paho.h"
#include "core/nng_impl.h"
#include "mqtt_msg.h"
#include "mqtt_qos_db.h"
#include <string.h>

#define URI_TCP "tcp://"
#define URI_WS "ws://"
#define URI_WSS "wss://"
#define URI_SSL "ssl://"

int MQTTAsync_setCallbacks(MQTTAsync handle, void *context,
						   MQTTAsync_connectionLost *cl,
						   MQTTAsync_messageArrived *ma,
						   MQTTAsync_deliveryComplete *dc)
{
	int rc = MQTTASYNC_SUCCESS;
	MQTTAsyncs *m = handle;

	if (m == NULL || ma == NULL || m->c == NULL || m->c->connect_state != NOT_IN_PROGRESS)
		rc = MQTTASYNC_FAILURE;
	else
	{
		m->clContext = m->maContext = m->dcContext = context;
		m->cl = cl;
		m->ma = ma;
		m->dc = dc;
	}

	MQTTAsync_unlock_mutex(mqttasync_mutex);
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

    if ((m = malloc(sizeof(MQTTAsyncs))) == NULL)
	{
		rc = NNG_ENOMEM;
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
		nng_log_warn("dialer", "NONONO");
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
