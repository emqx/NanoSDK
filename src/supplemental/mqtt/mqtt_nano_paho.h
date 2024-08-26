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

#include "nng/mqtt/mqtt_paho.h"
#include "core/nng_impl.h"
#include "nng/mqtt/mqtt_client.h"
#include "nng/nng.h"

#define MQTTAsync_successData5_initializer                                          \
  {                                                                                 \
    {'M', 'Q', 'S', 'D'}, 0, 0, MQTTREASONCODE_SUCCESS, MQTTProperties_initializer, \
    {                                                                               \
      .sub = { 0,                                                                   \
               0 }                                                                  \
    }                                                                               \
  }
typedef struct
{
	int type;
	MQTTAsync_onSuccess* onSuccess;
	MQTTAsync_onFailure* onFailure;
	MQTTAsync_onSuccess5* onSuccess5;
	MQTTAsync_onFailure5* onFailure5;
	MQTTAsync_token token;
	void* context;
	// START_TIME_TYPE start_time;
	MQTTProperties properties;
	union
	{
		struct
		{
			int count;
			char** topics;
			int* qoss;
			MQTTSubscribe_options opts;
			MQTTSubscribe_options* optlist;
		} sub;
		struct
		{
			int count;
			char** topics;
		} unsub;
		struct
		{
			char* destinationName;
			int payloadlen;
			void* payload;
			int qos;
			int retained;
		} pub;
		struct
		{
			int internal;
			int timeout;
			enum MQTTReasonCodes reasonCode;
		} dis;
		struct
		{
			int currentURI;
			int MQTTVersion; /**< current MQTT version being used to connect */
		} conn;
	} details;
} MQTTAsync_command;

typedef struct MQTTAsync_struct
{
	char* serverURI;
	int ssl;
	int websocket;
	// Clients* c;

	/* "Global", to the client, callback definitions */
	MQTTAsync_connectionLost* cl;
	MQTTAsync_messageArrived* ma;
	MQTTAsync_deliveryComplete* dc;
	void* clContext; /* the context to be associated with the conn lost callback*/
	void* maContext; /* the context to be associated with the msg arrived callback*/
	void* dcContext; /* the context to be associated with the deliv complete callback*/

	MQTTAsync_connected* connected;
	void* connected_context; /* the context to be associated with the connected callback*/

	MQTTAsync_disconnected* disconnected;
	void* disconnected_context; /* the context to be associated with the disconnected callback*/

	// MQTTAsync_updateConnectOptions* updateConnectOptions;
	void* updateConnectOptions_context;

	/* Each time connect is called, we store the options that were used.  These are reused in
	   any call to reconnect, or an automatic reconnect attempt */
	MQTTAsync_command connect;		/* Connect operation properties */
	MQTTAsync_command disconnect;		/* Disconnect operation properties */
	MQTTAsync_command* pending_write;       /* Is there a socket write pending? */

	// List* responses;
	unsigned int command_seqno;

	// MQTTPacket* pack;

	/* added for offline buffering */
	MQTTAsync_createOptions* createOptions;
	int shouldBeConnected;
	int noBufferedMessages; /* the current number of buffered (publish) messages for this client */

	/* added for automatic reconnect */
	int automaticReconnect;
	int minRetryInterval;
	int maxRetryInterval;
	int serverURIcount;
	char** serverURIs;
	int connectTimeout;

	int currentInterval;
	int currentIntervalBase;
	// START_TIME_TYPE lastConnectionFailedTime;
	int retrying;
	int reconnectNow;

	/* MQTT V5 properties */
	MQTTProperties* connectProps;
	MQTTProperties* willProps;

    /* Belows are what really matters */
    nng_mqtt_client *nanosdk_client;
    nng_dialer *dialer;
    nng_socket *sock;
} MQTTAsyncs;

#ifdef __cplusplus
}
#endif

#endif







