#include "msquic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "core/nng_impl.h"
#include "quic_api.h"

// static void
// quic_fatal(const char *msg, int rv)
// {
// 	fprintf(stderr, "%s: %s\n", msg, nng_strerror(rv));
// }

// Config for msquic
const QUIC_REGISTRATION_CONFIG RegConfig = { "mqtt",
	QUIC_EXECUTION_PROFILE_LOW_LATENCY };
const QUIC_BUFFER Alpn   = { sizeof("mqtt") - 1, (uint8_t *) "mqtt" };
const uint64_t                 IdleTimeoutMs    = 0;
const uint32_t                 SendBufferLength = 100;
const QUIC_API_TABLE          *MsQuic;
HQUIC                          Registration;
HQUIC                          Configuration;
HQUIC                          GStream;

//
// Helper function to load a client configuration.
//
BOOLEAN
LoadConfiguration(
    BOOLEAN Unsecure
    )
{
    QUIC_SETTINGS Settings = {0};
    // Configures the client's idle timeout.
    Settings.IdleTimeoutMs = IdleTimeoutMs;
    Settings.IsSet.IdleTimeoutMs = FALSE;

    // Configures a default client configuration, optionally disabling
    // server certificate validation.
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (Unsecure) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    return TRUE;
}

void
quic_open()
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        goto Error;
    }

    // Create a registration for the app's connections.
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
            // This will block until all outstanding child objects have been
            // closed.
            MsQuic->RegistrationClose(Registration);
        }
        MsQuicClose(MsQuic);
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
        // The handshake has completed for the connection.
        printf("[conn][%p] Connected\n", Connection);
		GStream = NULL;
		// QuicMqttStart(Connection, &GStream);
        // QuicMqttSend(Connection, 1);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        // The connection was explicitly shut down by the peer.
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
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

int
quic_connect(const char *url)
{
	// Load the client configuration based on the "unsecure" command line
	// option.
	if (!LoadConfiguration(TRUE)) {
		return (-1);
	}

	QUIC_STATUS Status;
	const char *ResumptionTicketString = NULL;
	HQUIC       Connection             = NULL;

	// Allocate a new connection object.
	if (QUIC_FAILED(
	        Status = MsQuic->ConnectionOpen(Registration,
	            QuicClientConnectionCallback, NULL, &Connection))) {
		printf("ConnectionOpen failed, 0x%x!\n", Status);
		goto Error;
	}

	nng_url *url_s;

	if (url == NULL)
		url = "mqtt-quic://54.75.171.11:14567";

	nng_url_parse(&url_s, url);
	for (int i=0; i<strlen(url_s->u_host); ++i)
		if (url_s->u_host[i] == ':') {
			url_s->u_host[i] = '\0';
			break;
		}

	printf("[conn] Connecting... %s : %s\n", url_s->u_host, url_s->u_port);

	// Start the connection to the server.
	if (QUIC_FAILED(
	        Status = MsQuic->ConnectionStart(Connection, Configuration,
	            QUIC_ADDRESS_FAMILY_UNSPEC, url_s->u_host, atoi(url_s->u_port)))) {
		printf("ConnectionStart failed, 0x%x!\n", Status);
		goto Error;
	}

Error:

	if (QUIC_FAILED(Status) && Connection != NULL) {
		MsQuic->ConnectionClose(Connection);
	}

	return 0;
}

// unite init of msquic here, deal with cb of stream
static int
quic_alloc()
{
}

int
nni_msquic_dialer_alloc(nng_stream_dialer **dp, const nng_url *url)
{
}

int
nni_msquic_listener_alloc(nng_stream_listener **lp, const nng_url *url)
{
}
