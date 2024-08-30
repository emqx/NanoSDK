#ifndef NNG_SUPPLEMENTAL_MQTT_NANO_PAHO_H
#define NNG_SUPPLEMENTAL_MQTT_NANO_PAHO_H

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

#define PAHO_MEMORY_ERROR -99

struct {
	enum MQTTPropertyCodes value;
	const char* name;
} nameToString[] =
{
  {MQTTPROPERTY_CODE_PAYLOAD_FORMAT_INDICATOR, "PAYLOAD_FORMAT_INDICATOR"},
  {MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL, "MESSAGE_EXPIRY_INTERVAL"},
  {MQTTPROPERTY_CODE_CONTENT_TYPE, "CONTENT_TYPE"},
  {MQTTPROPERTY_CODE_RESPONSE_TOPIC, "RESPONSE_TOPIC"},
  {MQTTPROPERTY_CODE_CORRELATION_DATA, "CORRELATION_DATA"},
  {MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER, "SUBSCRIPTION_IDENTIFIER"},
  {MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL, "SESSION_EXPIRY_INTERVAL"},
  {MQTTPROPERTY_CODE_ASSIGNED_CLIENT_IDENTIFER, "ASSIGNED_CLIENT_IDENTIFER"},
  {MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE, "SERVER_KEEP_ALIVE"},
  {MQTTPROPERTY_CODE_AUTHENTICATION_METHOD, "AUTHENTICATION_METHOD"},
  {MQTTPROPERTY_CODE_AUTHENTICATION_DATA, "AUTHENTICATION_DATA"},
  {MQTTPROPERTY_CODE_REQUEST_PROBLEM_INFORMATION, "REQUEST_PROBLEM_INFORMATION"},
  {MQTTPROPERTY_CODE_WILL_DELAY_INTERVAL, "WILL_DELAY_INTERVAL"},
  {MQTTPROPERTY_CODE_REQUEST_RESPONSE_INFORMATION, "REQUEST_RESPONSE_INFORMATION"},
  {MQTTPROPERTY_CODE_RESPONSE_INFORMATION, "RESPONSE_INFORMATION"},
  {MQTTPROPERTY_CODE_SERVER_REFERENCE, "SERVER_REFERENCE"},
  {MQTTPROPERTY_CODE_REASON_STRING, "REASON_STRING"},
  {MQTTPROPERTY_CODE_RECEIVE_MAXIMUM, "RECEIVE_MAXIMUM"},
  {MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM, "TOPIC_ALIAS_MAXIMUM"},
  {MQTTPROPERTY_CODE_TOPIC_ALIAS, "TOPIC_ALIAS"},
  {MQTTPROPERTY_CODE_MAXIMUM_QOS, "MAXIMUM_QOS"},
  {MQTTPROPERTY_CODE_RETAIN_AVAILABLE, "RETAIN_AVAILABLE"},
  {MQTTPROPERTY_CODE_USER_PROPERTY, "USER_PROPERTY"},
  {MQTTPROPERTY_CODE_MAXIMUM_PACKET_SIZE, "MAXIMUM_PACKET_SIZE"},
  {MQTTPROPERTY_CODE_WILDCARD_SUBSCRIPTION_AVAILABLE, "WILDCARD_SUBSCRIPTION_AVAILABLE"},
  {MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE, "SUBSCRIPTION_IDENTIFIERS_AVAILABLE"},
  {MQTTPROPERTY_CODE_SHARED_SUBSCRIPTION_AVAILABLE, "SHARED_SUBSCRIPTION_AVAILABLE"}
};

enum msgTypes
{
	CONNECT = 1, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL,
	PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK,
	PINGREQ, PINGRESP, DISCONNECT, AUTH
};

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
    int MQTTVersion; /**< the version of MQTT being used */
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
    // To be compatible with Paho style Callback
    MQTTAsync_command subscribe;
    MQTTAsync_command unsubscribe;
    MQTTAsync_command publish;

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

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

/**
 * Structure to hold the valid ranges of UTF-8 characters, for each byte up to 4
 */
struct
{
	int len; /**< number of elements in the following array (1 to 4) */
	struct
	{
		char lower; /**< lower limit of valid range */
		char upper; /**< upper limit of valid range */
	} bytes[4];   /**< up to 4 bytes can be used per character */
}
valid_ranges[] =
{
		{1, { {00, 0x7F} } },
		{2, { {0xC2, 0xDF}, {0x80, 0xBF} } },
		{3, { {0xE0, 0xE0}, {0xA0, 0xBF}, {0x80, 0xBF} } },
		{3, { {0xE1, 0xEC}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{3, { {0xED, 0xED}, {0x80, 0x9F}, {0x80, 0xBF} } },
		{3, { {0xEE, 0xEF}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{4, { {0xF0, 0xF0}, {0x90, 0xBF}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{4, { {0xF1, 0xF3}, {0x80, 0xBF}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{4, { {0xF4, 0xF4}, {0x80, 0x8F}, {0x80, 0xBF}, {0x80, 0xBF} } },
};

int UTF8_validate(int len, const char *data);
int UTF8_validateString(const char* string);

#define MQTTAsync_failureData5_initializer                                                     \
  {                                                                                            \
    {'M', 'Q', 'F', 'D'}, 0, 0, MQTTREASONCODE_SUCCESS, MQTTProperties_initializer, 0, NULL, 0 \
  }


/** The one byte MQTT V5 property type */
enum MQTTPropertyTypes {
  MQTTPROPERTY_TYPE_BYTE,
  MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER,
  MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER,
  MQTTPROPERTY_TYPE_VARIABLE_BYTE_INTEGER,
  MQTTPROPERTY_TYPE_BINARY_DATA,
  MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING,
  MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR
};

static struct nameToType
{
  enum MQTTPropertyCodes name;
  enum MQTTPropertyTypes type;
} namesToTypes[] =
{
  {MQTTPROPERTY_CODE_PAYLOAD_FORMAT_INDICATOR, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL, MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_CONTENT_TYPE, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_RESPONSE_TOPIC, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_CORRELATION_DATA, MQTTPROPERTY_TYPE_BINARY_DATA},
  {MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER, MQTTPROPERTY_TYPE_VARIABLE_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL, MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_ASSIGNED_CLIENT_IDENTIFER, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE, MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_AUTHENTICATION_METHOD, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_AUTHENTICATION_DATA, MQTTPROPERTY_TYPE_BINARY_DATA},
  {MQTTPROPERTY_CODE_REQUEST_PROBLEM_INFORMATION, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_WILL_DELAY_INTERVAL, MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_REQUEST_RESPONSE_INFORMATION, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_RESPONSE_INFORMATION, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_SERVER_REFERENCE, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_REASON_STRING, MQTTPROPERTY_TYPE_UTF_8_ENCODED_STRING},
  {MQTTPROPERTY_CODE_RECEIVE_MAXIMUM, MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM, MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_TOPIC_ALIAS, MQTTPROPERTY_TYPE_TWO_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_MAXIMUM_QOS, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_RETAIN_AVAILABLE, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_USER_PROPERTY, MQTTPROPERTY_TYPE_UTF_8_STRING_PAIR},
  {MQTTPROPERTY_CODE_MAXIMUM_PACKET_SIZE, MQTTPROPERTY_TYPE_FOUR_BYTE_INTEGER},
  {MQTTPROPERTY_CODE_WILDCARD_SUBSCRIPTION_AVAILABLE, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE, MQTTPROPERTY_TYPE_BYTE},
  {MQTTPROPERTY_CODE_SHARED_SUBSCRIPTION_AVAILABLE, MQTTPROPERTY_TYPE_BYTE}
};

/**
 * Returns a printable string description of an MQTT V5 property code.
 * @param value an MQTT V5 property code.
 * @return the printable string description of the input property code.
 * NULL if the code was not found.
 */
extern const char* MQTTPropertyName(enum MQTTPropertyCodes value);

/**
 * Returns the MQTT V5 type code of an MQTT V5 property.
 * @param value an MQTT V5 property code.
 * @return the MQTT V5 type code of the input property. -1 if the code was not found.
 */
extern int MQTTProperty_getType(enum MQTTPropertyCodes value);

/**
 * Free all memory allocated to the property list, including any to individual
 * properties.
 * @param properties pointer to the property list.
 */
extern void MQTTProperties_free(MQTTProperties *properties);

/** The connect options that can be updated before an automatic reconnect. */
typedef struct {
	/** The eyecatcher for this structure.  Will be MQCD. */
	char struct_id[4];
	/** The version number of this structure.  Will be 0 */
	int struct_version;
	/**
	 * MQTT servers that support the MQTT v3.1 protocol provide
	 * authentication and authorisation by user name and password. This is
	 * the user name parameter. Set data to NULL to remove.  To change,
	 * allocate new storage with ::MQTTAsync_allocate - this will then be
	 * free later by the library.
	 */
	const char *username;
	/**
	 * The password parameter of the MQTT authentication.
	 * Set data to NULL to remove.  To change, allocate new
	 * storage with ::MQTTAsync_allocate - this will then be free later by
	 * the library.
	 */
	struct {
		int         len;  /**< binary password length */
		const void *data; /**< binary password data */
	} binarypwd;
} MQTTAsync_connectData;

#define MQTTAsync_connectData_initializer      \
  {                                            \
    {'M', 'Q', 'C', 'D'}, 0, NULL, { 0, NULL } \
  }

/**
 * This is a callback function which will allow the client application to
 * update the connection data.
 * @param data The connection data which can be modified by the application.
 * @return Return a non-zero value to update the connect data, zero to keep the
 * same data.
 */
typedef int MQTTAsync_updateConnectOptions(void *context, MQTTAsync_connectData *data);

#ifdef __cplusplus
}
#endif

#endif