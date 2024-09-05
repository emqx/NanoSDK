//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// This file is meant to be compatible with Eclipse Paho

#ifndef NNG_MQTT_CLIENT_PAHO_H
#define NNG_MQTT_CLIENT_PAHO_H

#include <nng/nng.h>
#include <stdbool.h>
#include <stdint.h>

#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(MQTTCLIENTPERSISTENCE_H)
#define MQTTCLIENTPERSISTENCE_H
/*
/// @endcond
*/

/**
 * This <i>persistence_type</i> value specifies the default file system-based
 * persistence mechanism (see MQTTClient_create()).
 */
#define MQTTCLIENT_PERSISTENCE_DEFAULT 0
/**
 * This <i>persistence_type</i> value specifies a memory-based
 * persistence mechanism (see MQTTClient_create()).
 */
#define MQTTCLIENT_PERSISTENCE_NONE 1
/**
 * This <i>persistence_type</i> value specifies an application-specific
 * persistence mechanism (see MQTTClient_create()).
 */
#define MQTTCLIENT_PERSISTENCE_USER 2

/**
 * Application-specific persistence functions must return this error code if
 * there is a problem executing the function.
 */
#define MQTTCLIENT_PERSISTENCE_ERROR -2

/**
 * Returns a pointer to a string representation of the error code, or NULL.
 * Do not free after use. Returns NULL if the error code is unknown.
 * @param code the MQTTASYNC_ return code.
 * @return a static string representation of the error code.
 */
NNG_DECL const char *MQTTAsync_strerror(int code);

/**
 * @brief Initialize the persistent store.
 *
 * Either open the existing persistent store for this client ID or create a new
 * one if one doesn't exist. If the persistent store is already open, return
 * without taking any action.
 *
 * An application can use the same client identifier to connect to many
 * different servers. The <i>clientid</i> in conjunction with the
 * <i>serverURI</i> uniquely identifies the persistence store required.
 *
 * @param handle The address of a pointer to a handle for this persistence
 * implementation. This function must set handle to a valid reference to the
 * persistence following a successful return.
 * The handle pointer is passed as an argument to all the other
 * persistence functions. It may include the context parameter and/or any other
 * data for use by the persistence functions.
 * @param clientID The client identifier for which the persistent store should
 * be opened.
 * @param serverURI The connection string specified when the MQTT client was
 * created (see MQTTClient_create()).
 * @param context A pointer to any data required to initialize the persistent
 * store (see ::MQTTClient_persistence).
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_open)(
    void **handle, const char *clientID, const char *serverURI, void *context);

/**
 * @brief Close the persistent store referred to by the handle.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_close)(void *handle);

/**
 * @brief Put the specified data into the persistent store.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @param key A string used as the key for the data to be put in the store. The
 * key is later used to retrieve data from the store with Persistence_get().
 * @param bufcount The number of buffers to write to the persistence store.
 * @param buffers An array of pointers to the data buffers associated with
 * this <i>key</i>.
 * @param buflens An array of lengths of the data buffers. <i>buflen[n]</i>
 * gives the length of <i>buffer[n]</i>.
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_put)(
    void *handle, char *key, int bufcount, char *buffers[], int buflens[]);

/**
 * @brief Retrieve the specified data from the persistent store.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @param key A string that is the key for the data to be retrieved. This is
 * the same key used to save the data to the store with Persistence_put().
 * @param buffer The address of a pointer to a buffer. This function sets the
 * pointer to point at the retrieved data, if successful.
 * @param buflen The address of an int that is set to the length of
 * <i>buffer</i> by this function if successful.
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_get)(
    void *handle, char *key, char **buffer, int *buflen);

/**
 * @brief Remove the data for the specified key from the store.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @param key A string that is the key for the data to be removed from the
 * store. This is the same key used to save the data to the store with
 * Persistence_put().
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_remove)(void *handle, char *key);

/**
 * @brief Returns the keys in this persistent data store.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @param keys The address of a pointer to pointers to strings. Assuming
 * successful execution, this function allocates memory to hold the returned
 * keys (strings used to store the data with Persistence_put()). It also
 * allocates memory to hold an array of pointers to these strings. <i>keys</i>
 * is set to point to the array of pointers to strings.
 * @param nkeys A pointer to the number of keys in this persistent data store.
 * This function sets the number of keys, if successful.
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_keys)(void *handle, char ***keys, int *nkeys);

/**
 * @brief Clears the persistence store, so that it no longer contains any
 * persisted data.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @return Return 0 if the function completes successfully, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_clear)(void *handle);

/**
 * @brief Returns whether any data has been persisted using the specified key.
 *
 * @param handle The handle pointer from a successful call to
 * Persistence_open().
 * @param key The string to be tested for existence in the store.
 * @return Return 0 if the key was found in the store, otherwise return
 * ::MQTTCLIENT_PERSISTENCE_ERROR.
 */
typedef int (*Persistence_containskey)(void *handle, char *key);

/**
 * @brief A structure containing the function pointers to a persistence
 * implementation and the context or state that will be shared across all
 * the persistence functions.
 */
typedef struct {
	/**
	 * A pointer to any data required to initialize the persistent store.
	 */
	void *context;
	/**
	 * A function pointer to an implementation of Persistence_open().
	 */
	Persistence_open popen;
	/**
	 * A function pointer to an implementation of Persistence_close().
	 */
	Persistence_close pclose;
	/**
	 * A function pointer to an implementation of Persistence_put().
	 */
	Persistence_put pput;
	/**
	 * A function pointer to an implementation of Persistence_get().
	 */
	Persistence_get pget;
	/**
	 * A function pointer to an implementation of Persistence_remove().
	 */
	Persistence_remove premove;
	/**
	 * A function pointer to an implementation of Persistence_keys().
	 */
	Persistence_keys pkeys;
	/**
	 * A function pointer to an implementation of Persistence_clear().
	 */
	Persistence_clear pclear;
	/**
	 * A function pointer to an implementation of
	 * Persistence_containskey().
	 */
	Persistence_containskey pcontainskey;
} MQTTClient_persistence;

/**
 * A callback which is invoked just before a write to persistence.  This can be
 * used to transform the data, for instance to encrypt it.
 * @param context The context as set in ::MQTTAsync_setBeforePersistenceWrite
 * @param bufcount The number of buffers to write to the persistence store.
 * @param buffers An array of pointers to the data buffers.
 * @param buflens An array of lengths of the data buffers.
 * @return Return 0 if the function completes successfully, otherwise non 0.
 */
typedef int MQTTPersistence_beforeWrite(
    void *context, int bufcount, char *buffers[], int buflens[]);

/**
 * A callback which is invoked just after a read from persistence.  This can be
 * used to transform the data, for instance to decrypt it.
 * @param context The context as set in ::MQTTAsync_setAfterPersistenceRead
 * @param buffer The address of a pointer to a buffer.
 * @param buflen The address of an int that is the length of the buffer.
 * @return Return 0 if the function completes successfully, otherwise non 0.
 */
typedef int MQTTPersistence_afterRead(
    void *context, char **buffer, int *buflen);

#endif

/**
 * Return code: No error. Indicates successful completion of an MQTT client
 * operation.
 */
#define MQTTASYNC_SUCCESS 0
/**
 * Return code: A generic error code indicating the failure of an MQTT client
 * operation.
 */
#define MQTTASYNC_FAILURE -1

/* error code -2 is MQTTAsync_PERSISTENCE_ERROR */

#define MQTTASYNC_PERSISTENCE_ERROR -2

/**
 * Return code: The client is disconnected.
 */
#define MQTTASYNC_DISCONNECTED -3
/**
 * Return code: The maximum number of messages allowed to be simultaneously
 * in-flight has been reached.
 */
#define MQTTASYNC_MAX_MESSAGES_INFLIGHT -4
/**
 * Return code: An invalid UTF-8 string has been detected.
 */
#define MQTTASYNC_BAD_UTF8_STRING -5
/**
 * Return code: A NULL parameter has been supplied when this is invalid.
 */
#define MQTTASYNC_NULL_PARAMETER -6
/**
 * Return code: The topic has been truncated (the topic string includes
 * embedded NULL characters). String functions will not access the full topic.
 * Use the topic length value to access the full topic.
 */
#define MQTTASYNC_TOPICNAME_TRUNCATED -7
/**
 * Return code: A structure parameter does not have the correct eyecatcher
 * and version number.
 */
#define MQTTASYNC_BAD_STRUCTURE -8
/**
 * Return code: A qos parameter is not 0, 1 or 2
 */
#define MQTTASYNC_BAD_QOS -9
/**
 * Return code: All 65535 MQTT msgids are being used
 */
#define MQTTASYNC_NO_MORE_MSGIDS -10
/**
 * Return code: the request is being discarded when not complete
 */
#define MQTTASYNC_OPERATION_INCOMPLETE -11
/**
 * Return code: no more messages can be buffered
 */
#define MQTTASYNC_MAX_BUFFERED_MESSAGES -12
/**
 * Return code: Attempting SSL connection using non-SSL version of library
 */
#define MQTTASYNC_SSL_NOT_SUPPORTED -13
/**
 * Return code: protocol prefix in serverURI should be tcp://, ssl://, ws:// or
 * wss:// The TLS enabled prefixes (ssl, wss) are only valid if the TLS version
 * of the library is linked with.
 */
#define MQTTASYNC_BAD_PROTOCOL -14
/**
 * Return code: don't use options for another version of MQTT
 */
#define MQTTASYNC_BAD_MQTT_OPTION -15
/**
 * Return code: call not applicable to the client's version of MQTT
 */
#define MQTTASYNC_WRONG_MQTT_VERSION -16
/**
 *  Return code: 0 length will topic
 */
#define MQTTASYNC_0_LEN_WILL_TOPIC -17
/*
 * Return code: connect or disconnect command ignored because there is already
 * a connect or disconnect command at the head of the list waiting to be
 * processed. Use the onSuccess/onFailure callbacks to wait for the previous
 * connect or disconnect command to be complete.
 */
#define MQTTASYNC_COMMAND_IGNORED -18
/**
 * Default MQTT version to connect with.  Use 3.1.1 then fall back to 3.1
 */
#define MQTTVERSION_DEFAULT 0
/**
 * MQTT version to connect with: 3.1
 */
#define MQTTVERSION_3_1 3
/**
 * MQTT version to connect with: 3.1.1
 */
#define MQTTVERSION_3_1_1 4
/**
 * MQTT version to connect with: 5
 */
#define MQTTVERSION_5 5
/**
 * Bad return code from subscribe, as defined in the 3.1.1 specification
 */
#define MQTT_BAD_SUBSCRIBE 0x80

#define MQTTAsync_willOptions_initializer                    \
	{                                                    \
		{ 'M', 'Q', 'T', 'W' }, 1, NULL, NULL, 0, 0, \
		{                                            \
			0, NULL                              \
		}                                            \
	}

#define MQTT_SSL_VERSION_DEFAULT 0
#define MQTT_SSL_VERSION_TLS_1_0 1
#define MQTT_SSL_VERSION_TLS_1_1 2
#define MQTT_SSL_VERSION_TLS_1_2 3
#define MQTTProperties_initializer { 0, 0, 0, NULL }

#define MQTTSubscribe_options_initializer \
	{ { 'M', 'Q', 'S', 'O' }, 0, 0, 0, 0 }

#define MQTTAsync_responseOptions_initializer                      \
	{ { 'M', 'Q', 'T', 'R' }, 1, NULL, NULL, 0, 0, NULL, NULL, \
		MQTTProperties_initializer,                        \
		MQTTSubscribe_options_initializer, 0, NULL }

/** A synonym for responseOptions to better reflect its usage since MQTT 5.0 */
typedef struct MQTTAsync_responseOptions MQTTAsync_callOptions;
#define MQTTAsync_callOptions_initializer MQTTAsync_responseOptions_initializer

#define MQTTAsync_connectOptions_initializer                                \
	{ { 'M', 'Q', 'T', 'C' }, 8, 60, 1, 65535, NULL, NULL, NULL, 30, 0, \
		NULL, NULL, NULL, NULL, 0, NULL, MQTTVERSION_DEFAULT, 0, 1, \
		60, { 0, NULL }, 0, NULL, NULL, NULL, NULL, NULL, NULL,     \
		NULL, 0 }
#define MQTTAsync_connectOptions_initializer5                               \
	{ { 'M', 'Q', 'T', 'C' }, 8, 60, 0, 65535, NULL, NULL, NULL, 30, 0, \
		NULL, NULL, NULL, NULL, 0, NULL, MQTTVERSION_5, 0, 1, 60,   \
		{ 0, NULL }, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0 }
#define MQTTAsync_connectOptions_initializer_ws                             \
	{ { 'M', 'Q', 'T', 'C' }, 8, 45, 1, 65535, NULL, NULL, NULL, 30, 0, \
		NULL, NULL, NULL, NULL, 0, NULL, MQTTVERSION_DEFAULT, 0, 1, \
		60, { 0, NULL }, 0, NULL, NULL, NULL, NULL, NULL, NULL,     \
		NULL, 0 }
#define MQTTAsync_connectOptions_initializer5_ws                            \
	{ { 'M', 'Q', 'T', 'C' }, 8, 45, 0, 65535, NULL, NULL, NULL, 30, 0, \
		NULL, NULL, NULL, NULL, 0, NULL, MQTTVERSION_5, 0, 1, 60,   \
		{ 0, NULL }, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0 }
#define MQTTAsync_createOptions_initializer \
	{ { 'M', 'Q', 'C', 'O' }, 2, 0, 100, MQTTVERSION_DEFAULT, 0, 0, 1, 1 }

#define MQTTAsync_createOptions_initializer5 \
	{ { 'M', 'Q', 'C', 'O' }, 2, 0, 100, MQTTVERSION_5, 0, 0, 1, 1 }
#define MQTTAsync_disconnectOptions_initializer                           \
	{ { 'M', 'Q', 'T', 'D' }, 0, 0, NULL, NULL, NULL,                 \
		MQTTProperties_initializer, MQTTREASONCODE_SUCCESS, NULL, \
		NULL }

#define MQTTAsync_disconnectOptions_initializer5                          \
	{ { 'M', 'Q', 'T', 'D' }, 1, 0, NULL, NULL, NULL,                 \
		MQTTProperties_initializer, MQTTREASONCODE_SUCCESS, NULL, \
		NULL }
#define MQTTAsync_SSLOptions_initializer                                      \
	{ { 'M', 'Q', 'T', 'S' }, 5, NULL, NULL, NULL, NULL, NULL, NULL,      \
		NULL, 1, MQTT_SSL_VERSION_DEFAULT, 0, NULL, NULL, NULL, NULL, \
		NULL, 0, NULL, 0 }
/**
 * The data for a length delimited string
 */
typedef struct {
	int   len;  /**< the length of the string */
	char *data; /**< pointer to the string data */
} MQTTLenString;

/** The one byte MQTT V5 property indicator */
enum MQTTPropertyCodes {
	MQTTPROPERTY_CODE_PAYLOAD_FORMAT_INDICATOR = 1, /**< The value is 1 */
	MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL  = 2, /**< The value is 2 */
	MQTTPROPERTY_CODE_CONTENT_TYPE             = 3, /**< The value is 3 */
	MQTTPROPERTY_CODE_RESPONSE_TOPIC           = 8, /**< The value is 8 */
	MQTTPROPERTY_CODE_CORRELATION_DATA         = 9, /**< The value is 9 */
	MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIER = 11, /**< The value is 11 */
	MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL = 17, /**< The value is 17 */
	MQTTPROPERTY_CODE_ASSIGNED_CLIENT_IDENTIFER =
	    18,                                       /**< The value is 18 */
	MQTTPROPERTY_CODE_SERVER_KEEP_ALIVE     = 19, /**< The value is 19 */
	MQTTPROPERTY_CODE_AUTHENTICATION_METHOD = 21, /**< The value is 21 */
	MQTTPROPERTY_CODE_AUTHENTICATION_DATA   = 22, /**< The value is 22 */
	MQTTPROPERTY_CODE_REQUEST_PROBLEM_INFORMATION =
	    23,                                     /**< The value is 23 */
	MQTTPROPERTY_CODE_WILL_DELAY_INTERVAL = 24, /**< The value is 24 */
	MQTTPROPERTY_CODE_REQUEST_RESPONSE_INFORMATION =
	    25,                                      /**< The value is 25 */
	MQTTPROPERTY_CODE_RESPONSE_INFORMATION = 26, /**< The value is 26 */
	MQTTPROPERTY_CODE_SERVER_REFERENCE     = 28, /**< The value is 28 */
	MQTTPROPERTY_CODE_REASON_STRING        = 31, /**< The value is 31 */
	MQTTPROPERTY_CODE_RECEIVE_MAXIMUM      = 33, /**< The value is 33*/
	MQTTPROPERTY_CODE_TOPIC_ALIAS_MAXIMUM  = 34, /**< The value is 34 */
	MQTTPROPERTY_CODE_TOPIC_ALIAS          = 35, /**< The value is 35 */
	MQTTPROPERTY_CODE_MAXIMUM_QOS          = 36, /**< The value is 36 */
	MQTTPROPERTY_CODE_RETAIN_AVAILABLE     = 37, /**< The value is 37 */
	MQTTPROPERTY_CODE_USER_PROPERTY        = 38, /**< The value is 38 */
	MQTTPROPERTY_CODE_MAXIMUM_PACKET_SIZE  = 39, /**< The value is 39 */
	MQTTPROPERTY_CODE_WILDCARD_SUBSCRIPTION_AVAILABLE =
	    40, /**< The value is 40 */
	MQTTPROPERTY_CODE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE =
	    41, /**< The value is 41 */
	MQTTPROPERTY_CODE_SHARED_SUBSCRIPTION_AVAILABLE =
	    42 /**< The value is 241 */
};

/**
 * Structure to hold an MQTT version 5 property of any type
 */
typedef struct {
	enum MQTTPropertyCodes
	    identifier; /**<  The MQTT V5 property id. A multi-byte integer. */
	/** The value of the property, as a union of the different possible
	 * types. */
	union {
		unsigned char
		    byte; /**< holds the value of a byte property type */
		unsigned short integer2; /**< holds the value of a 2 byte
		                            integer property type */
		unsigned int integer4; /**< holds the value of a 4 byte integer
		                          property type */
		struct {
			MQTTLenString
			    data; /**< The value of a string property, or the
			             name of a user property. */
			MQTTLenString
			    value; /**< The value of a user property. */
		};
	} value;
} MQTTProperty;

/**
 * MQTT version 5 property list
 */
typedef struct MQTTProperties {
	int count;            /**< number of property entries in the array */
	int max_count;        /**< max number of properties that the currently
	                         allocated array can store */
	int           length; /**< mbi: byte length of all properties */
	MQTTProperty *array;  /**< array of properties */
} MQTTProperties;

/** The MQTT V5 one byte reason code */
enum MQTTReasonCodes {
	MQTTREASONCODE_SUCCESS                                = 0,
	MQTTREASONCODE_NORMAL_DISCONNECTION                   = 0,
	MQTTREASONCODE_GRANTED_QOS_0                          = 0,
	MQTTREASONCODE_GRANTED_QOS_1                          = 1,
	MQTTREASONCODE_GRANTED_QOS_2                          = 2,
	MQTTREASONCODE_DISCONNECT_WITH_WILL_MESSAGE           = 4,
	MQTTREASONCODE_NO_MATCHING_SUBSCRIBERS                = 16,
	MQTTREASONCODE_NO_SUBSCRIPTION_FOUND                  = 17,
	MQTTREASONCODE_CONTINUE_AUTHENTICATION                = 24,
	MQTTREASONCODE_RE_AUTHENTICATE                        = 25,
	MQTTREASONCODE_UNSPECIFIED_ERROR                      = 128,
	MQTTREASONCODE_MALFORMED_PACKET                       = 129,
	MQTTREASONCODE_PROTOCOL_ERROR                         = 130,
	MQTTREASONCODE_IMPLEMENTATION_SPECIFIC_ERROR          = 131,
	MQTTREASONCODE_UNSUPPORTED_PROTOCOL_VERSION           = 132,
	MQTTREASONCODE_CLIENT_IDENTIFIER_NOT_VALID            = 133,
	MQTTREASONCODE_BAD_USER_NAME_OR_PASSWORD              = 134,
	MQTTREASONCODE_NOT_AUTHORIZED                         = 135,
	MQTTREASONCODE_SERVER_UNAVAILABLE                     = 136,
	MQTTREASONCODE_SERVER_BUSY                            = 137,
	MQTTREASONCODE_BANNED                                 = 138,
	MQTTREASONCODE_SERVER_SHUTTING_DOWN                   = 139,
	MQTTREASONCODE_BAD_AUTHENTICATION_METHOD              = 140,
	MQTTREASONCODE_KEEP_ALIVE_TIMEOUT                     = 141,
	MQTTREASONCODE_SESSION_TAKEN_OVER                     = 142,
	MQTTREASONCODE_TOPIC_FILTER_INVALID                   = 143,
	MQTTREASONCODE_TOPIC_NAME_INVALID                     = 144,
	MQTTREASONCODE_PACKET_IDENTIFIER_IN_USE               = 145,
	MQTTREASONCODE_PACKET_IDENTIFIER_NOT_FOUND            = 146,
	MQTTREASONCODE_RECEIVE_MAXIMUM_EXCEEDED               = 147,
	MQTTREASONCODE_TOPIC_ALIAS_INVALID                    = 148,
	MQTTREASONCODE_PACKET_TOO_LARGE                       = 149,
	MQTTREASONCODE_MESSAGE_RATE_TOO_HIGH                  = 150,
	MQTTREASONCODE_QUOTA_EXCEEDED                         = 151,
	MQTTREASONCODE_ADMINISTRATIVE_ACTION                  = 152,
	MQTTREASONCODE_PAYLOAD_FORMAT_INVALID                 = 153,
	MQTTREASONCODE_RETAIN_NOT_SUPPORTED                   = 154,
	MQTTREASONCODE_QOS_NOT_SUPPORTED                      = 155,
	MQTTREASONCODE_USE_ANOTHER_SERVER                     = 156,
	MQTTREASONCODE_SERVER_MOVED                           = 157,
	MQTTREASONCODE_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED     = 158,
	MQTTREASONCODE_CONNECTION_RATE_EXCEEDED               = 159,
	MQTTREASONCODE_MAXIMUM_CONNECT_TIME                   = 160,
	MQTTREASONCODE_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 161,
	MQTTREASONCODE_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED   = 162
};

/**
 *  Initialization options
 */
typedef struct {
	/** The eyecatcher for this structure.  Must be MQTG. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0 */
	int struct_version;
	/** 1 = we do openssl init, 0 = leave it to the application */
	int do_openssl_init;
} MQTTAsync_init_options;

#define MQTTAsync_init_options_initializer { { 'M', 'Q', 'T', 'G' }, 0, 0 }

/**
 * Global init of mqtt library. Call once on program start to set global
 * behaviour. handle_openssl_init - if mqtt library should handle openssl init
 * (1) or rely on the caller to init it before using mqtt (0)
 */
//   LIBMQTT_API void MQTTAsync_global_init(MQTTAsync_init_options *inits);

/**
 * A handle representing an MQTT client. A valid client handle is available
 * following a successful call to MQTTAsync_create().
 */
typedef void *MQTTAsync;
/**
 * A value representing an MQTT message. A token is returned to the
 * client application when a message is published. The token can then be used
 * to check that the message was successfully delivered to its destination (see
 * MQTTAsync_publish(),
 * MQTTAsync_publishMessage(),
 * MQTTAsync_deliveryComplete(), and
 * MQTTAsync_getPendingTokens()).
 */
typedef int MQTTAsync_token;

/** Utility structure where name/value pairs are needed */
typedef struct {
	const char *name;  /**< name string */
	const char *value; /**< value string */
} MQTTAsync_nameValue;

/**
 * A structure representing the payload and attributes of an MQTT message. The
 * message topic is not part of this structure (see MQTTAsync_publishMessage(),
 * MQTTAsync_publish(), MQTTAsync_receive(), MQTTAsync_freeMessage()
 * and MQTTAsync_messageArrived()).
 */
typedef struct {
	/** The eyecatcher for this structure.  must be MQTM. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0 or 1.
	 *  0 indicates no message properties */
	int struct_version;
	/** The length of the MQTT message payload in bytes. */
	int payloadlen;
	/** A pointer to the payload of the MQTT message. */
	void *payload;
	/**
	 * The quality of service (QoS) assigned to the message.
	 * There are three levels of QoS:
	 * <DL>
	 * <DT><B>QoS0</B></DT>
	 * <DD>Fire and forget - the message may not be delivered</DD>
	 * <DT><B>QoS1</B></DT>
	 * <DD>At least once - the message will be delivered, but may be
	 * delivered more than once in some circumstances.</DD>
	 * <DT><B>QoS2</B></DT>
	 * <DD>Once and one only - the message will be delivered exactly
	 * once.</DD>
	 * </DL>
	 */
	int qos;
	/**
	 * The retained flag serves two purposes depending on whether the
	 * message it is associated with is being published or received.
	 *
	 * <b>retained = true</b><br>
	 * For messages being published, a true setting indicates that the MQTT
	 * server should retain a copy of the message. The message will then be
	 * transmitted to new subscribers to a topic that matches the message
	 * topic. For subscribers registering a new subscription, the flag
	 * being true indicates that the received message is not a new one, but
	 * one that has been retained by the MQTT server.
	 *
	 * <b>retained = false</b> <br>
	 * For publishers, this indicates that this message should not be
	 * retained by the MQTT server. For subscribers, a false setting
	 * indicates this is a normal message, received as a result of it being
	 * published to the server.
	 */
	int retained;
	/**
	 * The dup flag indicates whether or not this message is a duplicate.
	 * It is only meaningful when receiving QoS1 messages. When true, the
	 * client application should take appropriate action to deal with the
	 * duplicate message.  This is an output parameter only.
	 */
	int dup;
	/** The message identifier is reserved for internal use by the
	 * MQTT client and server.  It is an output parameter only - writing
	 * to it will serve no purpose.  It contains the MQTT message id of
	 * an incoming publish message.
	 */
	int msgid;
	/**
	 * The MQTT V5 properties associated with the message.
	 */
	MQTTProperties properties;
} MQTTAsync_message;

/** The data returned on completion of a successful API call in the response
 * callback onSuccess. */
typedef struct {
	/** A token identifying the successful request. Can be used to refer to
	 * the request later. */
	MQTTAsync_token token;
	/** A union of the different values that can be returned for subscribe,
	 * unsubscribe and publish. */
	union {
		/** For subscribe, the granted QoS of the subscription returned
		 * by the server. Also for subscribeMany, if only 1
		 * subscription was requested. */
		int qos;
		/** For subscribeMany, if more than one subscription was
		 * requested, the list of granted QoSs of the subscriptions
		 * returned by the server. */
		int *qosList;
		/** For publish, the message being sent to the server. */
		struct {
			MQTTAsync_message message; /**< the message being sent
			                              to the server */
			char *destinationName; /**< the topic destination for
			                          the message */
		} pub;
		/* For connect, the server connected to, MQTT version used, and
		 * sessionPresent flag */
		struct {
			char *serverURI; /**< the connection string of the
			                    server */
			int MQTTVersion; /**< the version of MQTT being used */
			int sessionPresent; /**< the session present flag
			                       returned from the server */
		} connect;
	} alt;
} MQTTAsync_successData;

/** The data returned on completion of a successful API call in the response
 * callback onSuccess. */
typedef struct {
	char struct_id[4];  /**< The eyecatcher for this structure.  Will be
	                       MQSD. */
	int struct_version; /**< The version number of this structure.  Will be
	                       0 */
	/** A token identifying the successful request. Can be used to refer to
	 * the request later. */
	MQTTAsync_token      token;
	enum MQTTReasonCodes reasonCode; /**< MQTT V5 reason code returned */
	property *properties; /**< MQTT V5 properties returned, if any */
	/** A union of the different values that can be returned for subscribe,
	 * unsubscribe and publish. */
	union {
		/** For subscribeMany, the list of reasonCodes returned by the
		 * server. */
		struct {
			int reasonCodeCount; /**< the number of reason codes in
			                        the reasonCodes array */
			uint8_t *reasonCodes; /**< an array of reasonCodes */
		} sub;
		/** For publish, the message being sent to the server. */
		struct {
			MQTTAsync_message message; /**< the message being sent
			                              to the server */
			char *destinationName; /**< the topic destination for
			                          the message */
		} pub;
		/* For connect, the server connected to, MQTT version used, and
		 * sessionPresent flag */
		struct {
			char *serverURI; /**< the connection string of the
			                    server */
			int MQTTVersion; /**< the version of MQTT being used */
			int sessionPresent; /**< the session present flag
			                       returned from the server */
		} connect;
		/** For unsubscribeMany, the list of reasonCodes returned by
		 * the server. */
		struct {
			int reasonCodeCount; /**< the number of reason codes in
			                        the reasonCodes array */
			uint8_t
			    *reasonCodes; /**< an array of reasonCodes */
		} unsub;
	} alt;
} MQTTAsync_successData5;

/** The data returned on completion of an unsuccessful API call in the response
 * callback onFailure. */
typedef struct {
	/** A token identifying the failed request. */
	MQTTAsync_token token;
	/** A numeric code identifying the error. */
	int code;
	/** Optional text explaining the error. Can be NULL. */
	const char *message;
} MQTTAsync_failureData;

/** The data returned on completion of an unsuccessful API call in the response
 * callback onFailure. */
typedef struct {
	/** The eyecatcher for this structure.  Will be MQFD. */
	char struct_id[4];
	/** The version number of this structure.  Will be 0 */
	int struct_version;
	/** A token identifying the failed request. */
	MQTTAsync_token token;
	/** The MQTT reason code returned. */
	enum MQTTReasonCodes reasonCode;
	/** The MQTT properties on the ack, if any. */
	property *properties;
	/** A numeric code identifying the MQTT client library error. */
	int code;
	/** Optional further text explaining the error. Can be NULL. */
	const char *message;
	/** Packet type on which the failure occurred - used for publish QoS
	 * 1/2 exchanges*/
	int packet_type;
} MQTTAsync_failureData5;

/**
 * This is a callback function. The client application
 * must provide an implementation of this function to enable asynchronous
 * notification of the successful completion of an API call. The function is
 * registered with the client library by passing it as an argument in
 * ::MQTTAsync_responseOptions.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * ::MQTTAsync_responseOptions, which contains any application-specific
 * context.
 * @param response Any success data associated with the API completion.
 */
typedef void MQTTAsync_onSuccess(
    void *context, MQTTAsync_successData *response);

/**
 * This is a callback function, the MQTT V5 version of ::MQTTAsync_onSuccess.
 * The client application
 * must provide an implementation of this function to enable asynchronous
 * notification of the successful completion of an API call. The function is
 * registered with the client library by passing it as an argument in
 * ::MQTTAsync_responseOptions.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * ::MQTTAsync_responseOptions, which contains any application-specific
 * context.
 * @param response Any success data associated with the API completion.
 */
typedef void MQTTAsync_onSuccess5(
    void *context, MQTTAsync_successData5 *response);

/**
 * This is a callback function. The client application
 * must provide an implementation of this function to enable asynchronous
 * notification of the unsuccessful completion of an API call. The function is
 * registered with the client library by passing it as an argument in
 * ::MQTTAsync_responseOptions.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * ::MQTTAsync_responseOptions, which contains any application-specific
 * context.
 * @param response Failure data associated with the API completion.
 */
typedef void MQTTAsync_onFailure(
    void *context, MQTTAsync_failureData *response);

/**
 * This is a callback function, the MQTT V5 version of ::MQTTAsync_onFailure.
 * The application must provide an implementation of this function to enable
 * asynchronous notification of the unsuccessful completion of an API call. The
 * function is registered with the client library by passing it as an argument
 * in
 * ::MQTTAsync_responseOptions.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * ::MQTTAsync_responseOptions, which contains any application-specific
 * context.
 * @param response Failure data associated with the API completion.
 */
typedef void MQTTAsync_onFailure5(
    void *context, MQTTAsync_failureData5 *response);

/** Options for the ::MQTTAsync_createWithOptions call */
typedef struct {
	/** The eyecatcher for this structure.  must be MQCO. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0, 1, 2 or 3
	 * 0 means no MQTTVersion
	 * 1 means no allowDisconnectedSendAtAnyTime, deleteOldestMessages,
	 * restoreMessages 2 means no persistQoS0
	 */
	int struct_version;
	/** Whether to allow messages to be sent when the client library is not
	 * connected. */
	int sendWhileDisconnected;
	/** The maximum number of messages allowed to be buffered while not
	 * connected. */
	int maxBufferedMessages;
	/** Whether the MQTT version is 3.1, 3.1.1, or 5.  To use V5, this must
	 * be set. MQTT V5 has to be chosen here, because during the create
	 * call the message persistence is initialized, and we want to know
	 * whether the format of any persisted messages is appropriate for the
	 * MQTT version we are going to connect with.  Selecting 3.1 or 3.1.1
	 * and attempting to read 5.0 persisted messages will result in an
	 * error on create.  */
	int MQTTVersion;
	/**
	 * Allow sending of messages while disconnected before a first
	 * successful connect.
	 */
	int allowDisconnectedSendAtAnyTime;
	/*
	 * When the maximum number of buffered messages is reached, delete the
	 * oldest rather than the newest.
	 */
	int deleteOldestMessages;
	/*
	 * Restore messages from persistence on create - or clear it.
	 */
	int restoreMessages;
	/*
	 * Persist QoS0 publish commands - an option to not persist them.
	 */
	int persistQoS0;
} MQTTAsync_createOptions;

/** Options for the ::MQTTAsync_disconnect call */
typedef struct {
	/** The eyecatcher for this structure. Must be MQTD. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0 or 1.  0 signifies
	 * no V5 properties */
	int struct_version;
	/**
	 * The client delays disconnection for up to this time (in
	 * milliseconds) in order to allow in-flight message transfers to
	 * complete.
	 */
	int timeout;
	/**
	 * A pointer to a callback function to be called if the disconnect
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onSuccess *onSuccess;
	/**
	 * A pointer to a callback function to be called if the disconnect
	 * fails. Can be set to NULL, in which case no indication of
	 * unsuccessful completion will be received.
	 */
	MQTTAsync_onFailure *onFailure;
	/**
	 * A pointer to any application-specific context. The
	 * the <i>context</i> pointer is passed to success or failure callback
	 * functions to provide access to the context information in the
	 * callback.
	 */
	void *context;
	/**
	 * MQTT V5 input properties
	 */
	MQTTProperties properties;
	/**
	 * Reason code for MQTTV5 disconnect
	 */
	enum MQTTReasonCodes reasonCode;
	/**
	 * A pointer to a callback function to be called if the disconnect
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onSuccess5 *onSuccess5;
	/**
	 * A pointer to a callback function to be called if the disconnect
	 * fails. Can be set to NULL, in which case no indication of
	 * unsuccessful completion will be received.
	 */
	MQTTAsync_onFailure5 *onFailure5;
} MQTTAsync_disconnectOptions;

/**
 * MQTTAsync_willOptions defines the MQTT "Last Will and Testament" (LWT)
 * settings for the client. In the event that a client unexpectedly loses its
 * connection to the server, the server publishes the LWT message to the LWT
 * topic on behalf of the client. This allows other clients (subscribed to the
 * LWT topic) to be made aware that the client has disconnected. To enable the
 * LWT function for a specific client, a valid pointer to an
 * MQTTAsync_willOptions structure is passed in the MQTTAsync_connectOptions
 * structure used in the MQTTAsync_connect() call that connects the client to
 * the server. The pointer to MQTTAsync_willOptions can be set to NULL if the
 * LWT function is not required.
 */
typedef struct {
	/** The eyecatcher for this structure.  must be MQTW. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0 or 1
	    0 indicates no binary will message support
	 */
	int struct_version;
	/** The LWT topic to which the LWT message will be published. */
	const char *topicName;
	/** The LWT payload. */
	const char *message;
	/**
	 * The retained flag for the LWT message (see
	 * MQTTAsync_message.retained).
	 */
	int retained;
	/**
	 * The quality of service setting for the LWT message (see
	 * MQTTAsync_message.qos and @ref qos).
	 */
	int qos;
	/** The LWT payload in binary form. This is only checked and used if
	 * the message option is NULL */
	struct {
		int         len;  /**< binary payload length */
		const void *data; /**< binary payload data */
	} payload;
} MQTTAsync_willOptions;

/**
 * MQTTAsync_sslProperties defines the settings to establish an SSL/TLS
 * connection using the OpenSSL library. It covers the following scenarios:
 * - Server authentication: The client needs the digital certificate of the
 * server. It is included in a store containting trusted material (also known
 * as "trust store").
 * - Mutual authentication: Both client and server are authenticated during the
 * SSL handshake. In addition to the digital certificate of the server in a
 * trust store, the client will need its own digital certificate and the
 * private key used to sign its digital certificate stored in a "key store".
 * - Anonymous connection: Both client and server do not get authenticated and
 * no credentials are needed to establish an SSL connection. Note that this
 * scenario is not fully secure since it is subject to man-in-the-middle
 * attacks.
 */
typedef struct {
	/** The eyecatcher for this structure.  Must be MQTS */
	char struct_id[4];

	/** The version number of this structure. Must be 0, 1, 2, 3, 4 or 5.
	 * 0 means no sslVersion
	 * 1 means no verify, CApath
	 * 2 means no ssl_error_context, ssl_error_cb
	 * 3 means no ssl_psk_cb, ssl_psk_context, disableDefaultTrustStore
	 * 4 means no protos, protos_len
	 */
	int struct_version;

	/** The file in PEM format containing the public digital certificates
	 * trusted by the client. */
	const char *trustStore;

	/** The file in PEM format containing the public certificate chain of
	 * the client. It may also include the client's private key.
	 */
	const char *keyStore;

	const char *dkeyStore;

	/** If not included in the sslKeyStore, this setting points to the file
	 * in PEM format containing the client's private key.
	 */
	const char *privateKey;

	const char *dprivateKey;

	/** The password to load the client's privateKey if encrypted. */
	const char *privateKeyPassword;

	/**
	 * The list of cipher suites that the client will present to the server
	 * during the SSL handshake. For a full explanation of the cipher list
	 * format, please see the OpenSSL on-line documentation:
	 * http://www.openssl.org/docs/apps/ciphers.html#CIPHER_LIST_FORMAT
	 * If this setting is ommitted, its default value will be "ALL", that
	 * is, all the cipher suites -excluding those offering no encryption-
	 * will be considered. This setting can be used to set an SSL anonymous
	 * connection ("aNULL" string value, for instance).
	 */
	const char *enabledCipherSuites;

	/** True/False option to enable verification of the server certificate
	 * **/
	int enableServerCertAuth;

	/** The SSL/TLS version to use. Specify one of MQTT_SSL_VERSION_DEFAULT
	 * (0), MQTT_SSL_VERSION_TLS_1_0 (1), MQTT_SSL_VERSION_TLS_1_1 (2) or
	 * MQTT_SSL_VERSION_TLS_1_2 (3). Only used if struct_version is >= 1.
	 */
	int sslVersion;

	/**
	 * Whether to carry out post-connect checks, including that a
	 * certificate matches the given host name. Exists only if
	 * struct_version >= 2
	 */
	int verify;

	/**
	 * From the OpenSSL documentation:
	 * If CApath is not NULL, it points to a directory containing CA
	 * certificates in PEM format. Exists only if struct_version >= 2
	 */
	const char *CApath;

	/**
	 * Callback function for OpenSSL error handler ERR_print_errors_cb
	 * Exists only if struct_version >= 3
	 */
	int (*ssl_error_cb)(const char *str, size_t len, void *u);

	/**
	 * Application-specific contex for OpenSSL error handler
	 * ERR_print_errors_cb Exists only if struct_version >= 3
	 */
	void *ssl_error_context;

	/**
	 * Callback function for setting TLS-PSK options. Parameters correspond
	 * to that of SSL_CTX_set_psk_client_callback, except for u which is
	 * the pointer ssl_psk_context. Exists only if struct_version >= 4
	 */
	unsigned int (*ssl_psk_cb)(const char *hint, char *identity,
	    unsigned int max_identity_len, unsigned char *psk,
	    unsigned int max_psk_len, void *u);

	/**
	 * Application-specific contex for ssl_psk_cb
	 * Exists only if struct_version >= 4
	 */
	void *ssl_psk_context;

	/**
	 * Don't load default SSL CA. Should be used together with PSK to make
	 * sure regular servers with certificate in place is not accepted.
	 * Exists only if struct_version >= 4
	 */
	int disableDefaultTrustStore;

	/**
	 * The protocol-lists must be in wire-format, which is defined as a
	 * vector of non-empty, 8-bit length-prefixed, byte strings. The
	 * length-prefix byte is not included in the length. Each string is
	 * limited to 255 bytes. A byte-string length of 0 is invalid. A
	 * truncated byte-string is invalid. Check documentation for
	 * SSL_CTX_set_alpn_protos Exists only if struct_version >= 5
	 */
	const unsigned char *protos;

	/**
	 * The length of the vector protos vector
	 * Exists only if struct_version >= 5
	 */
	unsigned int protos_len;
} MQTTAsync_SSLOptions;

/**
 * MQTTAsync_connectOptions defines several settings that control the way the
 * client connects to an MQTT server.  Default values are set in
 * MQTTAsync_connectOptions_initializer.
 */
typedef struct {
	/** The eyecatcher for this structure.  must be MQTC. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0, 1, 2, 3 4 5 6, 7
	 * or 8. 0 signifies no SSL options and no serverURIs 1 signifies no
	 * serverURIs 2 signifies no MQTTVersion 3 signifies no automatic
	 * reconnect options 4 signifies no binary password option (just
	 * string) 5 signifies no MQTTV5 properties 6 signifies no HTTP headers
	 * option 7 signifies no HTTP proxy and HTTPS proxy options
	 */
	int struct_version;
	/** The "keep alive" interval, measured in seconds, defines the maximum
	 * time that should pass without communication between the client and
	 * the server The client will ensure that at least one message travels
	 * across the network within each keep alive period.  In the absence of
	 * a data-related message during the time period, the client sends a
	 * very small MQTT "ping" message, which the server will acknowledge.
	 * The keep alive interval enables the client to detect when the server
	 * is no longer available without having to wait for the long TCP/IP
	 * timeout. Set to 0 if you do not want any keep alive processing.
	 */
	int keepAliveInterval;
	/**
	 * This is a boolean value. The cleansession setting controls the
	 * behaviour of both the client and the server at connection and
	 * disconnection time. The client and server both maintain session
	 * state information. This information is used to ensure "at least
	 * once" and "exactly once" delivery, and "exactly once" receipt of
	 * messages. Session state also includes subscriptions created by an
	 * MQTT client. You can choose to maintain or discard state information
	 * between sessions.
	 *
	 * When cleansession is true, the state information is discarded at
	 * connect and disconnect. Setting cleansession to false keeps the
	 * state information. When you connect an MQTT client application with
	 * MQTTAsync_connect(), the client identifies the connection using the
	 * client identifier and the address of the server. The server checks
	 * whether session information for this client
	 * has been saved from a previous connection to the server. If a
	 * previous session still exists, and cleansession=true, then the
	 * previous session information at the client and server is cleared. If
	 * cleansession=false, the previous session is resumed. If no previous
	 * session exists, a new session is started.
	 */
	int cleansession;
	/**
	 * This controls how many messages can be in-flight simultaneously.
	 */
	int maxInflight;
	/**
	 * This is a pointer to an MQTTAsync_willOptions structure. If your
	 * application does not make use of the Last Will and Testament
	 * feature, set this pointer to NULL.
	 */
	MQTTAsync_willOptions *will;
	/**
	 * MQTT servers that support the MQTT v3.1 protocol provide
	 * authentication and authorisation by user name and password. This is
	 * the user name parameter.
	 */
	const char *username;
	/**
	 * MQTT servers that support the MQTT v3.1 protocol provide
	 * authentication and authorisation by user name and password. This is
	 * the password parameter.
	 */
	const char *password;
	/**
	 * The time interval in seconds to allow a connect to complete.
	 */
	int connectTimeout;
	/**
	 * The time interval in seconds after which unacknowledged publish
	 * requests are retried during a TCP session.  With MQTT 3.1.1 and
	 * later, retries are not required except on reconnect.  0 turns off
	 * in-session retries, and is the recommended setting.  Adding retries
	 * to an already overloaded network only exacerbates the problem.
	 */
	int retryInterval;
	/**
	 * This is a pointer to an MQTTAsync_SSLOptions structure. If your
	 * application does not make use of SSL, set this pointer to NULL.
	 */
	MQTTAsync_SSLOptions *ssl;
	/**
	 * A pointer to a callback function to be called if the connect
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onSuccess *onSuccess;
	/**
	 * A pointer to a callback function to be called if the connect fails.
	 * Can be set to NULL, in which case no indication of unsuccessful
	 * completion will be received.
	 */
	MQTTAsync_onFailure *onFailure;
	/**
	 * A pointer to any application-specific context. The
	 * the <i>context</i> pointer is passed to success or failure callback
	 * functions to provide access to the context information in the
	 * callback.
	 */
	void *context;
	/**
	 * The number of entries in the serverURIs array.
	 */
	int serverURIcount;
	/**
	 * An array of null-terminated strings specifying the servers to
	 * which the client will connect. Each string takes the form
	 * <i>protocol://host:port</i>. <i>protocol</i> must be <i>tcp</i>,
	 * <i>ssl</i>, <i>ws</i> or <i>wss</i>. The TLS enabled prefixes (ssl,
	 * wss) are only valid if a TLS version of the library is linked with.
	 * For <i>host</i>, you can
	 * specify either an IP address or a domain name. For instance, to
	 * connect to a server running on the local machines with the default
	 * MQTT port, specify <i>tcp://localhost:1883</i>.
	 */
	char *const *serverURIs;
	/**
	 * Sets the version of MQTT to be used on the connect.
	 * MQTTVERSION_DEFAULT (0) = default: start with 3.1.1, and if that
	 * fails, fall back to 3.1 MQTTVERSION_3_1 (3) = only try version 3.1
	 * MQTTVERSION_3_1_1 (4) = only try version 3.1.1
	 */
	int MQTTVersion;
	/**
	 * Reconnect automatically in the case of a connection being lost?
	 */
	int automaticReconnect; // not valid in nanosdk
	/**
	 * Minimum retry interval in seconds.  Doubled on each failed retry.
	 */
	int minRetryInterval;
	/**
	 * Maximum retry interval in seconds.  The doubling stops here on
	 * failed retries.
	 */
	int maxRetryInterval;
	/**
	 * Optional binary password.  Only checked and used if the password
	 * option is NULL
	 */
	struct {
		int         len;  /**< binary password length */
		const void *data; /**< binary password data */
	} binarypwd;
	/*
	 * MQTT V5 clean start flag.  Only clears state at the beginning of the
	 * session.
	 */
	int cleanstart;
	/**
	 * MQTT V5 properties for connect
	 */
	MQTTProperties *connectProperties;
	/**
	 * MQTT V5 properties for the will message in the connect
	 */
	MQTTProperties *willProperties;
	/**
	 * A pointer to a callback function to be called if the connect
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onSuccess5 *onSuccess5;
	/**
	 * A pointer to a callback function to be called if the connect fails.
	 * Can be set to NULL, in which case no indication of unsuccessful
	 * completion will be received.
	 */
	MQTTAsync_onFailure5 *onFailure5;
	/**
	 * HTTP headers for websockets
	 */
	const MQTTAsync_nameValue *httpHeaders;
	/**
	 * HTTP proxy for websockets
	 */
	const char *httpProxy;
	/**
	 * HTTPS proxy for websockets
	 */
	const char *httpsProxy;
	/**
	 * MQTTV5 SCRAM enhanced authentication
	 */
	bool scram;
} MQTTAsync_connectOptions;

#define MQTTAsync_message_initializer                     \
	{ { 'M', 'Q', 'T', 'M' }, 1, 0, NULL, 0, 0, 0, 0, \
		MQTTProperties_initializer }

/**
 * This is a callback function. The client application
 * must provide an implementation of this function to enable asynchronous
 * receipt of messages. The function is registered with the client library by
 * passing it as an argument to MQTTAsync_setCallbacks(). It is
 * called by the client library when a new message that matches a client
 * subscription has been received from the server. This function is executed on
 * a separate thread to the one on which the client application is running.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * MQTTAsync_setCallbacks(), which contains any application-specific context.
 * @param topicName The topic associated with the received message.
 * @param topicLen The length of the topic if there are one
 * more NULL characters embedded in <i>topicName</i>, otherwise <i>topicLen</i>
 * is 0. If <i>topicLen</i> is 0, the value returned by
 * <i>strlen(topicName)</i> can be trusted. If <i>topicLen</i> is greater than
 * 0, the full topic name can be retrieved by accessing <i>topicName</i> as a
 * byte array of length <i>topicLen</i>.
 * @param message The MQTTAsync_message structure for the received message.
 * This structure contains the message payload and attributes.
 * @return This function must return 0 or 1 indicating whether or not
 * the message has been safely received by the client application. <br>
 * Returning 1 indicates that the message has been successfully handled.
 * To free the message storage, ::MQTTAsync_freeMessage must be called.
 * To free the topic name storage, ::MQTTAsync_free must be called.<br>
 * Returning 0 indicates that there was a problem. In this
 * case, the client library will reinvoke MQTTAsync_messageArrived() to
 * attempt to deliver the message to the application again.
 * Do not free the message and topic storage when returning 0, otherwise
 * the redelivery will fail.
 */
typedef int MQTTAsync_messageArrived(
    void *context, char *topicName, int topicLen, MQTTAsync_message *message);

/**
 * This is a callback function. The client application
 * must provide an implementation of this function to enable asynchronous
 * notification of delivery of messages to the server. The function is
 * registered with the client library by passing it as an argument to
 * MQTTAsync_setCallbacks(). It is called by the client library after the
 * client application has published a message to the server. It indicates that
 * the necessary handshaking and acknowledgements for the requested quality of
 * service (see MQTTAsync_message.qos) have been completed. This function is
 * executed on a separate thread to the one on which the client application is
 * running.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * MQTTAsync_setCallbacks(), which contains any application-specific context.
 * @param token The ::MQTTAsync_token associated with
 * the published message. Applications can check that all messages have been
 * correctly published by matching the tokens returned from calls to
 * MQTTAsync_send() and MQTTAsync_sendMessage() with the tokens passed
 * to this callback.
 */
typedef void MQTTAsync_deliveryComplete(void *context, MQTTAsync_token token);

/**
 * This is a callback function. The client application
 * must provide an implementation of this function to enable asynchronous
 * notification of the loss of connection to the server. The function is
 * registered with the client library by passing it as an argument to
 * MQTTAsync_setCallbacks(). It is called by the client library if the client
 * loses its connection to the server. The client application must take
 * appropriate action, such as trying to reconnect or reporting the problem.
 * This function is executed on a separate thread to the one on which the
 * client application is running.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * MQTTAsync_setCallbacks(), which contains any application-specific context.
 * @param cause The reason for the disconnection.
 * Currently, <i>cause</i> is always set to NULL.
 */
typedef void MQTTAsync_connectionLost(void *context, char *cause);

/**
 * This is a callback function, which will be called when the client
 * library successfully connects.  This is superfluous when the connection
 * is made in response to a MQTTAsync_connect call, because the onSuccess
 * callback can be used.  It is intended for use when automatic reconnect
 * is enabled, so that when a reconnection attempt succeeds in the background,
 * the application is notified and can take any required actions.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * MQTTAsync_setCallbacks(), which contains any application-specific context.
 * @param cause The reason for the disconnection.
 * Currently, <i>cause</i> is always set to NULL.
 */
typedef void MQTTAsync_connected(void *context, char *cause);

/**
 * This is a callback function, which will be called when the client
 * library receives a disconnect packet.
 *
 * <b>Note:</b> Neither MQTTAsync_create() nor MQTTAsync_destroy() should be
 * called within this callback.
 * @param context A pointer to the <i>context</i> value originally passed to
 * MQTTAsync_setCallbacks(), which contains any application-specific context.
 * @param properties the properties in the disconnect packet.
 * @param properties the reason code from the disconnect packet
 * Currently, <i>cause</i> is always set to NULL.
 */
typedef void MQTTAsync_disconnected(void *context, MQTTProperties *properties,
    enum MQTTReasonCodes reasonCode);

/** The MQTT V5 subscribe options, apart from QoS which existed before V5. */
typedef struct MQTTSubscribe_options {
	/** The eyecatcher for this structure. Must be MQSO. */
	char struct_id[4];
	/** The version number of this structure.  Must be 0.
	 */
	int struct_version;
	/** To not receive our own publications, set to 1.
	 *  0 is the original MQTT behaviour - all messages matching the
	 * subscription are received.
	 */
	unsigned char noLocal;
	/** To keep the retain flag as on the original publish message, set
	 * to 1. If 0, defaults to the original MQTT behaviour where the retain
	 * flag is only set on publications sent by a broker if in response to
	 * a subscribe request.
	 */
	unsigned char retainAsPublished;
	/** 0 - send retained messages at the time of the subscribe (original
	 * MQTT behaviour) 1 - send retained messages on subscribe only if the
	 * subscription is new 2 - do not send retained messages at all
	 */
	unsigned char retainHandling;
} MQTTSubscribe_options;

/** Structure to define call options.  For MQTT 5.0 there is input data as well
 * as that describing the response method.  So there is now also a synonym
 * ::MQTTAsync_callOptions to better reflect the use.  This responseOptions
 * name is kept for backward compatibility.
 */
typedef struct MQTTAsync_responseOptions {
	/** The eyecatcher for this structure.  Must be MQTR */
	char struct_id[4];
	/** The version number of this structure.  Must be 0 or 1
	 *   if 0, no MQTTV5 options */
	int struct_version;
	/**
	 * A pointer to a callback function to be called if the API call
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onSuccess *onSuccess;
	/**
	 * A pointer to a callback function to be called if the API call fails.
	 * Can be set to NULL, in which case no indication of unsuccessful
	 * completion will be received.
	 */
	MQTTAsync_onFailure *onFailure;
	/**
	 * A pointer to any application-specific context. The
	 * the <i>context</i> pointer is passed to success or failure callback
	 * functions to provide access to the context information in the
	 * callback.
	 */
	void *context;
	/**
	 * A token is returned from the call.  It can be used to track
	 * the state of this request, both in the callbacks and in future calls
	 * such as ::MQTTAsync_waitForCompletion.
	 */
	MQTTAsync_token token;
	/**
	 * A pointer to a callback function to be called if the API call
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onSuccess5 *onSuccess5;
	/**
	 * A pointer to a callback function to be called if the API call
	 * successfully completes.  Can be set to NULL, in which case no
	 * indication of successful completion will be received.
	 */
	MQTTAsync_onFailure5 *onFailure5;
	/**
	 * MQTT V5 input properties
	 */
	MQTTProperties properties;
	/*
	 * MQTT V5 subscribe options, when used with subscribe only.
	 */
	MQTTSubscribe_options subscribeOptions;
	/*
	 * MQTT V5 subscribe option count, when used with subscribeMany only.
	 * The number of entries in the subscribe_options_list array.
	 */
	int subscribeOptionsCount;
	/*
	 * MQTT V5 subscribe option array, when used with subscribeMany only.
	 */
	MQTTSubscribe_options *subscribeOptionsList;
} MQTTAsync_responseOptions;

/**
 * This function sets the global callback functions for a specific client.
 * If your client application doesn't use a particular callback, set the
 * relevant parameter to NULL. Any necessary message acknowledgements and
 * status communications are handled in the background without any intervention
 * from the client application.  If you do not set a messageArrived callback
 * function, you will not be notified of the receipt of any messages as a
 * result of a subscription.
 *
 * <b>Note:</b> The MQTT client must be disconnected when this function is
 * called.
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param context A pointer to any application-specific context. The
 * the <i>context</i> pointer is passed to each of the callback functions to
 * provide access to the context information in the callback.
 * @param cl A pointer to an MQTTAsync_connectionLost() callback
 * function. You can set this to NULL if your application doesn't handle
 * disconnections.
 * @param ma A pointer to an MQTTAsync_messageArrived() callback
 * function.  If this callback is not set, an error will be returned.
 * You must set this callback because otherwise there would be
 * no way to deliver any incoming messages.
 * @param dc A pointer to an MQTTAsync_deliveryComplete() callback
 * function. You can set this to NULL if you do not want to check
 * for successful delivery.
 * @return ::MQTTASYNC_SUCCESS if the callbacks were correctly set,
 * ::MQTTASYNC_FAILURE if an error occurred.
 */
NNG_DECL int MQTTAsync_setCallbacks(MQTTAsync handle, void *context,
    MQTTAsync_connectionLost *cl, MQTTAsync_messageArrived *ma,
    MQTTAsync_deliveryComplete *dc);

// typedef nng_mqtt_msg_format_t nng_mqtt_msg_format_s;
/**
 * This function attempts to connect a previously-created client (see
 * MQTTAsync_create()) to an MQTT server using the specified options. If you
 * want to enable asynchronous message and status notifications, you must call
 * MQTTAsync_setCallbacks() prior to MQTTAsync_connect().
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param options A pointer to a valid MQTTAsync_connectOptions
 * structure.
 * @return ::MQTTASYNC_SUCCESS if the client connect request was accepted.
 * If the client was unable to connect to the server, an error code is
 * returned via the onFailure callback, if set.
 * Error codes greater than 0 are returned by the MQTT protocol:<br><br>
 * <b>1</b>: Connection refused: Unacceptable protocol version<br>
 * <b>2</b>: Connection refused: Identifier rejected<br>
 * <b>3</b>: Connection refused: Server unavailable<br>
 * <b>4</b>: Connection refused: Bad user name or password<br>
 * <b>5</b>: Connection refused: Not authorized<br>
 * <b>6-255</b>: Reserved for future use<br>
 */
NNG_DECL int MQTTAsync_connect(
    MQTTAsync handle, const MQTTAsync_connectOptions *options);

NNG_DECL int MQTTAsync_create(MQTTAsync *handle, const char *serverURI,
    const char *clientId, int persistence_type, void *persistence_context);
NNG_DECL int MQTTAsync_createWithOptions(MQTTAsync *handle,
    const char *serverURI, const char *clientId, int persistence_type,
    void *persistence_context, MQTTAsync_createOptions *options);
NNG_DECL int MQTTAsync_create(MQTTAsync *handle, const char *serverURI,
    const char *clientId, int persistence_type, void *persistence_context);

/**
 * This function attempts to subscribe a client to a single topic, which may
 * contain wildcards (see @ref wildcard). This call also specifies the
 * @ref qos requested for the subscription
 * (see also MQTTAsync_subscribeMany()).
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param topic The subscription topic, which may include wildcards.
 * @param qos The requested quality of service for the subscription.
 * @param response A pointer to a response options structure. Used to set
 * callback functions.
 * @return ::MQTTASYNC_SUCCESS if the subscription request is successful.
 * An error code is returned if there was a problem registering the
 * subscription.
 */
NNG_DECL int MQTTAsync_subscribe(MQTTAsync handle, const char *topic, int qos,
    MQTTAsync_responseOptions *response);

/**
 * This function attempts to subscribe a client to a list of topics, which may
 * contain wildcards (see @ref wildcard). This call also specifies the
 * @ref qos requested for each topic (see also MQTTAsync_subscribe()).
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param count The number of topics for which the client is requesting
 * subscriptions.
 * @param topic An array (of length <i>count</i>) of pointers to
 * topics, each of which may include wildcards.
 * @param qos An array (of length <i>count</i>) of @ref qos
 * values. qos[n] is the requested QoS for topic[n].
 * @param response A pointer to a response options structure. Used to set
 * callback functions.
 * @return ::MQTTASYNC_SUCCESS if the subscription request is successful.
 * An error code is returned if there was a problem registering the
 * subscriptions.
 */
NNG_DECL int MQTTAsync_subscribeMany(MQTTAsync handle, int count,
    char *const *topic, const int *qos, MQTTAsync_responseOptions *response);

/**
 * This function attempts to remove an existing subscription made by the
 * specified client.
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param topic The topic for the subscription to be removed, which may
 * include wildcards (see @ref wildcard).
 * @param response A pointer to a response options structure. Used to set
 * callback functions.
 * @return ::MQTTASYNC_SUCCESS if the subscription is removed.
 * An error code is returned if there was a problem removing the
 * subscription.
 */
NNG_DECL int MQTTAsync_unsubscribe(
    MQTTAsync handle, const char *topic, MQTTAsync_responseOptions *response);

/**
 * This function attempts to remove existing subscriptions to a list of topics
 * made by the specified client.
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param count The number subscriptions to be removed.
 * @param topic An array (of length <i>count</i>) of pointers to the topics of
 * the subscriptions to be removed, each of which may include wildcards.
 * @param response A pointer to a response options structure. Used to set
 * callback functions.
 * @return ::MQTTASYNC_SUCCESS if the subscriptions are removed.
 * An error code is returned if there was a problem removing the subscriptions.
 */
NNG_DECL int MQTTAsync_unsubscribeMany(MQTTAsync handle, int count,
    char *const *topic, MQTTAsync_responseOptions *response);

/**
 * This function attempts to publish a message to a given topic (see also
 * ::MQTTAsync_sendMessage()). An ::MQTTAsync_token is issued when
 * this function returns successfully if the QoS is greater than 0.
 * If the client application needs to
 * test for successful delivery of messages, a callback should be set
 * (see ::MQTTAsync_onSuccess() and ::MQTTAsync_deliveryComplete()).
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param destinationName The topic associated with this message.
 * @param payloadlen The length of the payload in bytes.
 * @param payload A pointer to the byte array payload of the message.
 * @param qos The @ref qos of the message.
 * @param retained The retained flag for the message.
 * @param response A pointer to an ::MQTTAsync_responseOptions structure. Used
 * to set callback functions. This is optional and can be set to NULL.
 * @return ::MQTTASYNC_SUCCESS if the message is accepted for publication.
 * An error code is returned if there was a problem accepting the message.
 */
NNG_DECL int MQTTAsync_send(MQTTAsync handle, const char *destinationName,
    int payloadlen, const void *payload, int qos, int retained,
    MQTTAsync_responseOptions *response);

/**
 * This function attempts to publish a message to a given topic (see also
 * MQTTAsync_publish()). An ::MQTTAsync_token is issued when
 * this function returns successfully if the QoS is greater than 0.
 * If the client application needs to
 * test for successful delivery of messages, a callback should be set
 * (see ::MQTTAsync_onSuccess() and ::MQTTAsync_deliveryComplete()).
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param destinationName The topic associated with this message.
 * @param msg A pointer to a valid MQTTAsync_message structure containing
 * the payload and attributes of the message to be published.
 * @param response A pointer to an ::MQTTAsync_responseOptions structure. Used
 * to set callback functions.
 * @return ::MQTTASYNC_SUCCESS if the message is accepted for publication.
 * An error code is returned if there was a problem accepting the message.
 */
NNG_DECL int MQTTAsync_sendMessage(MQTTAsync handle,
    const char *destinationName, const MQTTAsync_message *msg,
    MQTTAsync_responseOptions *response);

/**
 * Sets the MQTTAsync_connected() callback function for a client.
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param context A pointer to any application-specific context. The
 * the <i>context</i> pointer is passed to each of the callback functions to
 * provide access to the context information in the callback.
 * @param co A pointer to an MQTTAsync_connected() callback
 * function.  NULL removes the callback setting.
 * @return ::MQTTASYNC_SUCCESS if the callbacks were correctly set,
 * ::MQTTASYNC_FAILURE if an error occurred.
 */
NNG_DECL int MQTTAsync_setConnected(
    MQTTAsync handle, void *context, MQTTAsync_connected *co);

/**
 * Reconnects a client with the previously used connect options.  Connect
 * must have previously been called for this to work.
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @return ::MQTTASYNC_SUCCESS if the callbacks were correctly set,
 * ::MQTTASYNC_FAILURE if an error occurred.
 */
NNG_DECL int MQTTAsync_reconnect(MQTTAsync handle);

/**
 * This function attempts to disconnect the client from the MQTT
 * server. In order to allow the client time to complete handling of messages
 * that are in-flight when this function is called, a timeout period is
 * specified. When the timeout period has expired, the client disconnects even
 * if there are still outstanding message acknowledgements.
 * The next time the client connects to the same server, any QoS 1 or 2
 * messages which have not completed will be retried depending on the
 * cleansession settings for both the previous and the new connection (see
 * MQTTAsync_connectOptions.cleansession and MQTTAsync_connect()).
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @param options The client delays disconnection for up to this time (in
 * milliseconds) in order to allow in-flight message transfers to complete.
 * @return ::MQTTASYNC_SUCCESS if the client successfully disconnects from
 * the server. An error code is returned if the client was unable to disconnect
 * from the server
 */
NNG_DECL int MQTTAsync_disconnect(
    MQTTAsync handle, const MQTTAsync_disconnectOptions *options);

/**
 * This function allows the client application to test whether or not a
 * client is currently connected to the MQTT server.
 * @param handle A valid client handle from a successful call to
 * MQTTAsync_create().
 * @return Boolean true if the client is connected, otherwise false.
 */
NNG_DECL int MQTTAsync_isConnected(MQTTAsync handle);

/**
 * This function frees the memory allocated to an MQTT client (see
 * MQTTAsync_create()). It should be called when the client is no longer
 * required.
 * @param handle A pointer to the handle referring to the ::MQTTAsync
 * structure to be freed.
 */
NNG_DECL void MQTTAsync_destroy(MQTTAsync *handle);

/**
 * This function frees memory allocated to an MQTT message, including the
 * additional memory allocated to the message payload. The client application
 * calls this function when the message has been fully processed. <b>Important
 * note:</b> This function does not free the memory allocated to a message
 * topic string. It is the responsibility of the client application to free
 * this memory using the MQTTAsync_free() library function.
 * @param msg The address of a pointer to the ::MQTTAsync_message structure
 * to be freed.
 */
NNG_DECL void MQTTAsync_freeMessage(MQTTAsync_message **msg);

/**
 * This function frees memory allocated by the MQTT C client library,
 * especially the topic name. This is needed on Windows when the client library
 * and application program have been compiled with different versions of the C
 * compiler.  It is thus good policy to always use this function when freeing
 * any MQTT C client- allocated memory.
 * @param ptr The pointer to the client library storage to be freed.
 */
NNG_DECL void MQTTAsync_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif // NNG_MQTT_CLIENT_PAHO_H
