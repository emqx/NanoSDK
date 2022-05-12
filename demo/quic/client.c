//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// This is a very simple HTTP client.  It only performs HTTP GET
// operations, and does not follow HTTP redirects.  Think of it as
// a trivialized version of CURL.  It is super simple, taking the
// URL on the command line, and emitting the results to stdout.
// For clarity, we are eliding TLS support.

// It may not work on all systems, but it should work anywhere that
// both the standard C library and nng itself are available.

// We check for errors, but no effort is made to clean up resources,
// since this program just exits.  In longer running programs or libraries,
// callers should take care to clean up things that they allocate.

// Unfortunately many famous sites use redirects, so you won't see that
// emitted.

// Example usage:
//
// % export CPPFLAGS="-I /usr/local/include"
// % export LDFLAGS="-L /usr/local/lib -lnng"
// % export CC="cc"
// % ./quic_client conn url
// % ./quic_client sub  url
// % ./quic_client pub  url
//

#include <nng/nng.h>

#include "msquic.h"

#include <stdio.h>
#include <stdlib.h>

// Config for msquic
const QUIC_REGISTRATION_CONFIG RegConfig = { "mqtt", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
const QUIC_BUFFER Alpn = { sizeof("mqtt") - 1, (uint8_t*)"mqtt" };
const char* Target = "34.241.119.229";
// const char* Target = "127.0.0.1";
const uint16_t UdpPort = 14567;
const uint64_t IdleTimeoutMs = 0;
const uint32_t SendBufferLength = 100;
const QUIC_API_TABLE* MsQuic;
HQUIC Registration;
HQUIC Configuration;
HQUIC GStream;

void
fatal(int rv)
{
	fprintf(stderr, "%s\n", nng_strerror(rv));
	exit(1);
}

void
quic_open()
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        goto Error;
    }

    //
    // Create a registration for the app's connections.
    //
    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed, 0x%x!\n", Status);
        goto Error;
    }

	printf("msquic is init.\n");

	return;

Error:

    if (MsQuic != NULL) {
        if (Configuration != NULL) {
            MsQuic->ConfigurationClose(Configuration);
        }
        if (Registration != NULL) {
            MsQuic->RegistrationClose(Registration);
        }
        MsQuicClose(MsQuic);
    }
}


//
// The clients's callback for stream events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
QuicClientStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
        // free(Event->SEND_COMPLETE.ClientContext);
        printf("[strm][%p] Data sent\n", Stream);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        //
        // Data was received from the peer on the stream.
        //
        printf("[strm][%p] Data received\n", Stream);
		printf("Body is [%d][%s].\n", Event->RECEIVE.Buffers->Length, Event->RECEIVE.Buffers->Buffer);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        printf("[strm][%p] Peer aborted\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer aborted its send direction of the stream.
        //
        printf("[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        printf("[strm][%p] All done\n", Stream);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->StreamClose(Stream);
        }
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


void
QuicMqttStart(
    _In_ HQUIC Connection,
	_Out_ HQUIC *Streamp
	)
{
    HQUIC Stream = NULL;
    QUIC_STATUS Status;

    //
    // Create/allocate a new bidirectional stream. The stream is just allocated
    // and no QUIC stream identifier is assigned until it's started.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, QuicClientStreamCallback, NULL, &Stream))) {
        printf("StreamOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    printf("[strm][%p] Starting...\n", Stream);

    //
    // Starts the bidirectional stream. By default, the peer is not notified of
    // the stream being started until data is sent on the stream.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        goto Error;
    }

	*Streamp = Stream;

Error:

    if (QUIC_FAILED(Status)) {
		printf("EXITTT...........\n");
        MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
}

void
QuicMqttSend(
    _In_ HQUIC Connection,
	_In_ HQUIC Stream,
	_In_ int type
    )
{
    QUIC_STATUS Status;
    uint8_t* SendBufferRaw;
    QUIC_BUFFER* SendBuffer;

	// Mqtt connect message
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);

	if (type == 1) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);

		nng_mqtt_msg_set_connect_keep_alive(msg, 180);
		nng_mqtt_msg_set_connect_clean_session(msg, true);

		nng_mqtt_msg_set_connect_will_topic(msg, "topic");
		char *willmsg = "will \n test";
		nng_mqtt_msg_set_connect_will_msg(msg, willmsg, 12);
		nng_mqtt_msg_set_connect_keep_alive(msg, 180);
		nng_mqtt_msg_set_connect_clean_session(msg, true);
	} else if (type == 2) {
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

		int qos = 0;
		int count = 1;

		nng_mqtt_topic_qos *topics_qos =
		    nng_mqtt_topic_qos_array_create(count);
		char * topic1 = "topic1";
		struct topic * topics;
		topics = malloc(sizeof(*topics));
		topics->next = NULL;
		topics->val  = topic1;

		size_t i = 0;
		for (struct topic *tp = topics;
		     tp != NULL && i < 1;
		     tp = tp->next, i++) {
			nng_mqtt_topic_qos_array_set(
			    topics_qos, i, tp->val, qos);
		}

		nng_mqtt_msg_set_subscribe_topics(
		    msg, topics_qos, count);
	}

	nng_mqtt_msg_encode(msg);

	int header_len = nng_msg_header_len(msg);
	int body_len   = nng_msg_len(msg);
	char * header  = nng_msg_header(msg);
	char * body    = nng_msg_body(msg);
	int msg_len    = header_len + body_len;

	printf("msg_len %d header_len %d body_len %d .\n", msg_len, header_len, body_len);
	printf("header [%x%x] boyd [%x%x%x] .\n", header[0], header[1], body[0], body[1], body[2]);

    SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + msg_len);
    if (SendBufferRaw == NULL) {
        printf("SendBuffer allocation failed!\n");
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

	memcpy(SendBufferRaw+sizeof(QUIC_BUFFER), header, header_len);
	memcpy(SendBufferRaw+sizeof(QUIC_BUFFER)+header_len, body, body_len);

    SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
    SendBuffer->Buffer = SendBufferRaw+sizeof(QUIC_BUFFER);
    SendBuffer->Length = msg_len;

    printf("[strm][%p] Sending data...\n", Stream);

    //
    // Sends the buffer over the stream. Note the FIN flag is passed along with
    // the buffer. This indicates this is the last buffer on the stream and the
    // the stream is shut down (in the send direction) immediately after.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
        free(SendBufferRaw);
        goto Error;
    }

Error:

    if (QUIC_FAILED(Status)) {
		printf("EXITTT...........\n");
        MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
}


//
// Helper function to load a client configuration.
//
BOOLEAN
LoadConfiguration(
    BOOLEAN Unsecure
    )
{
    QUIC_SETTINGS Settings = {0};
    //
    // Configures the client's idle timeout.
    //
    Settings.IdleTimeoutMs = IdleTimeoutMs;
    Settings.IsSet.IdleTimeoutMs = FALSE;

    //
    // Configures a default client configuration, optionally disabling
    // server certificate validation.
    //
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (Unsecure) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    return TRUE;
}

void
quic_connect()
{
    //
    // Load the client configuration based on the "unsecure" command line option.
    //
    if (!LoadConfiguration(TRUE)) {
        return;
    }

    QUIC_STATUS Status;
    const char* ResumptionTicketString = NULL;
    HQUIC Connection = NULL;

    //
    // Allocate a new connection object.
    //
    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, QuicClientConnectionCallback, NULL, &Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    printf("[conn][%p] Connecting...\n", Connection);

    //
    // Start the connection to the server.
    //
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, Target, UdpPort))) {
        printf("ConnectionStart failed, 0x%x!\n", Status);
        goto Error;
    }

Error:

    if (QUIC_FAILED(Status) && Connection != NULL) {
        MsQuic->ConnectionClose(Connection);
    }

}


_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
QuicClientConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        printf("[conn][%p] Connected\n", Connection);
		GStream = NULL;
		QuicMqttStart(Connection, &GStream);
        // QuicMqttSend(Connection, 1);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        //
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
        //
        printf("[conn][%p] Resumption ticket received (%u bytes):\n", Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        for (uint32_t i = 0; i < Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength; i++) {
            printf("%.2X", (uint8_t)Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket[i]);
        }
        printf("\n");
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

static int
connect_cb(void * arg)
{

}

static int
recv_cb(void * arg)
{

}

int
client(const char *type, const char *url)
{
	nng_socket sock;
	int        rv;
	nng_msg *  msg;

	if ((rv = quic_open()) != 0) {
		fatal("quic_open", rv);
	}

	if ((rv = quic_connect(sock, url, NULL, 0)) != 0) {
		fatal("quic_connect", rv);
	}

/*
	if ((rv = quic_mqtt_set_connect_cb(connect_cb, NULL)) != 0) {
		fatal("set_connect_cb", rv);
	}

	if ((rv = quic_mqtt_set_recv_cb(recv_cb, NULL)) != 0) {
		fatal("set_recv_cb", rv);
	}
*/

	if ((rv = quic_mqtt_connect(sock, url) != 0) {
		fatal("mqtt_connect", rv);
	}

	nng_close(sock);

	return (0);
}

int
main(int argc, char **argv)
{
	int rc;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <conn|sub|pub> <url>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	rc = client(argv[1], argv[2]);
	exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
