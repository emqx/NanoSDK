//
// Copyright 2020 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/mqtt/mqtt_qos_db_api.h"


static void mqtt_sock_init(void *arg, nni_sock *sock);
static void mqtt_sock_fini(void *arg);
static void mqtt_sock_open(void *arg);
static void mqtt_sock_send(void *arg, nni_aio *aio);
static void mqtt_sock_recv(void *arg, nni_aio *aio);
static void mqtt_send_cb(void *arg);
static void mqtt_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

// A mqtt_sock_s is our per-socket protocol private structure.
struct mqtt_sock_s {
	nni_atomic_bool closed;
	nni_atomic_int  ttl;
	nni_duration    retry;
	nni_mtx         mtx;    // more fine grained mutual exclusion
	mqtt_ctx_t      master; // to which we delegate send/recv calls
	mqtt_pipe_t *   mqtt_pipe;
	nni_list        recv_queue; // ctx pending to receive
	nni_list        send_queue; // ctx pending to send
#ifdef NNG_SUPP_SQLITE
	sqlite3 *sqlite_db;
#endif
	// For Quic
	QUIC_API_TABLE* MsQuic;
	HQUIC Registration;
	HQUIC Configuration;
	HQUIC Connection;

	char *url;
};

// A mqtt_pipe_s is our per-pipe protocol private structure.
struct mqtt_pipe_s {
	nni_atomic_bool closed;
	nni_atomic_int  next_packet_id; // next packet id to use
	nni_pipe *      pipe;
	mqtt_sock_t *   mqtt_sock;
#ifdef NNG_SUPP_SQLITE
	sqlite3 *sent_unack;
#else
	nni_id_map sent_unack; // send messages unacknowledged
#endif
	nni_id_map      recv_unack;    // recv messages unacknowledged
	nni_aio         send_aio;      // send aio to the underlying transport
	nni_aio         recv_aio;      // recv aio to the underlying transport
	nni_aio         time_aio;      // timer aio to resend unack msg
	nni_lmq         recv_messages; // recv messages queue
	nni_lmq         send_messages; // send messages queue
	nni_lmq         ctx_aios;      // awaiting aio of QoS
	bool            busy;

	// For Quic
	HQUIC Stream;
	HQUIC Connection;
	QUIC_API_TABLE* MsQuic;
};


/* Configuration for MsQuic */
static uint8_t
load_quic_config(void *arg, BOOLEAN Unsecure)
{
	mqtt_sock_t *s = arg;
	QUIC_API_TABLE *MsQuic = s->MsQuic;

    QUIC_SETTINGS Settings = {0};
    // Configures the client's idle timeout.
    Settings.IdleTimeoutMs = 120000; // 120s
    Settings.IsSet.IdleTimeoutMs = TRUE;

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
	QUIC_BUFFER Alpn = { sizeof("mqtt") - 1, (uint8_t*)"mqtt" };
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(s->Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &s->Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return -1;
    }

    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(s->Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return -1;
    }

    return 0;
}

/******************************************************************************
 *                              Sock Implementation                           *
 ******************************************************************************/

static void
mqtt_quic_sock_init(void *arg, nni_sock *sock)
{
	NNI_ARG_UNUSED(sock);
	mqtt_sock_t *s = arg;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);

	nni_atomic_init(&s->ttl);
	nni_atomic_set(&s->ttl, 8);

	// this is "semi random" start for request IDs.
	s->retry = NNI_SECOND * 60;

	nni_mtx_init(&s->mtx);
	mqtt_ctx_init(&s->master, s);

#ifdef NNG_SUPP_SQLITE
	nni_qos_db_init_sqlite(s->sqlite_db, DB_NAME, false);
	nni_qos_db_reset_client_msg_pipe_id(s->sqlite_db);
#endif

	s->mqtt_pipe = NULL;
	NNI_LIST_INIT(&s->recv_queue, mqtt_ctx_t, rqnode);
	NNI_LIST_INIT(&s->send_queue, mqtt_ctx_t, sqnode);

	// For Quic Init
	QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

	if (QUIC_FAILED(Status = MsQuicOpen2(&s->MsQuic))) {
		printf("MsQuicOpen2 failed, 0x%x!\n", Status);
		goto Error;
	}

	// Create a registration for the app's connections.
	if (QUIC_FAILED(Status = s->MsQuic->RegistrationOpen(
	                    &s->RegConfig, &s->Registration))) {
		printf("RegistrationOpen failed, 0x%x!\n", Status);
		goto Error;
	}

	if (0 != (Status = load_quic_config(s, TRUE))) {
		printf("Load Quic configuration failed, 0x%x!\n", Status);
		goto Error;
	}

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

static void
mqtt_quic_sock_fini(void *arg)
{
	mqtt_sock_t *s = arg;
#ifdef NNG_SUPP_SQLITE
	nni_qos_db_fini_sqlite(s->sqlite_db);
#endif
	mqtt_ctx_fini(&s->master);
	nni_mtx_fini(&s->mtx);

	// For Quic
	if (s->MsQuic != NULL) {
		if (s->Configuration != NULL) {
			s->MsQuic->ConfigurationClose(s->Configuration);
		}
		if (s->Registration != NULL) {
			s->MsQuic->RegistrationClose(s->Registration);
		}
		MsQuicClose(s->MsQuic);
	}
}

static void
mqtt_quic_sock_open(void *arg)
{
	mqtt_sock_t *s = arg;
	QUIC_API_TABLE *MsQuic = s->MsQuic;

    QUIC_STATUS Status;
    const char* ResumptionTicketString = NULL;
    HQUIC Connection = s->Connection;

    // Allocate a new connection object.
    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(s->Registration, quic_mqtt_connect_cb, arg, &Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    printf("[conn][%p] Connecting...\n", Connection);

	char *Target;
	int UdpPort;
	quic_mqtt_url_parse(s->url, &Target, &UdpPort);

    // Start the connection to the server.
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(Connection, s->Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, Target, UdpPort))) {
        printf("ConnectionStart failed, 0x%x!\n", Status);
        goto Error;
    }

Error:

    if (QUIC_FAILED(Status) && Connection != NULL) {
        MsQuic->ConnectionClose(Connection);
    }
}

static void
mqtt_quic_sock_close(void *arg)
{
	mqtt_sock_t *s = arg;
	mqtt_ctx_t  *ctx;
	nni_aio *aio;
	nni_msg *msg;

	nni_atomic_set_bool(&s->closed, true);
	//clean ctx queue when pipe was closed.
	while ((ctx = nni_list_first(&s->send_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->send_queue, ctx);
		aio       = ctx->saio;
		ctx->saio = NULL;
		msg       = nni_aio_get_msg(aio);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		nni_msg_free(msg);
	}
	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		// there should be no msg waiting
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	// For Quic
    if (s->Connection != NULL) {
        s->MsQuic->ConnectionClose(s->Connection);
    }
}

static void
mqtt_quic_sock_send(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s = arg;
	mqtt_ctx_send(&s->master, aio);
}

static void
mqtt_quic_sock_recv(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s = arg;
	mqtt_ctx_recv(&s->master, aio);
}

/* Callback for Connection and Stream */

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
quic_mqtt_connect_cb(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
	mqtt_sock_t *s = Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        // The handshake has completed for the connection.
        printf("[conn][%p] Connected\n", Connection);

		mqtt_pipe_s *p = malloc(sizeof(mqtt_pipe_s));
		quic_mqtt_stream_init(p, NULL, s);
		quic_mqtt_stream_open(p);
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

//
// The clients's callback for stream events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
quic_mqtt_stream_cb(
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

/* Stream EQ Pipe ???? */

static void
quic_mqtt_stream_init(void *arg, void *pipe, void *s)
{
	mqtt_pipe_t *p    = arg;

	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, false);
	nni_atomic_set(&p->next_packet_id, 1);
	p->pipe      = pipe;
	p->mqtt_sock = s;
	nni_aio_init(&p->send_aio, mqtt_send_cb, p);
	nni_aio_init(&p->recv_aio, mqtt_recv_cb, p);
	nni_aio_init(&p->time_aio, mqtt_timer_cb, p);
	// Packet IDs are 16 bits
	// We start at a random point, to minimize likelihood of
	// accidental collision across restarts.
#ifdef NNG_SUPP_SQLITE
	p->sent_unack = p->mqtt_sock->sqlite_db;
#else
	nni_id_map_init(&p->sent_unack, 0x0000u, 0xffffu, true);
#endif
	nni_id_map_init(&p->recv_unack, 0x0000u, 0xffffu, true);
	nni_lmq_init(&p->recv_messages, NNG_MAX_RECV_LMQ);
	nni_lmq_init(&p->send_messages, NNG_MAX_SEND_LMQ);

	// For Quic
	mqtt_sock_t *sock = s;
	p->Connection = s->Connection;
	p->MsQuic     = s->MsQuic;

	return (0);
}

void
quic_mqtt_stream_open(void *arg)
{
	mqtt_pipe_s *p          = arg;
	HQUIC        Stream     = p->Stream;
	HQUIC        Connection = p->Connection;

	QUIC_STATUS Status;

	// Create/allocate a new bidirectional stream. The stream is just
	// allocated and no QUIC stream identifier is assigned until it's
	// started.
	if (QUIC_FAILED(Status = p->MsQuic->StreamOpen(Connection,
	                    QUIC_STREAM_OPEN_FLAG_NONE,
	                    quic_mqtt_stream_cb, NULL, &Stream))) {
		printf("StreamOpen failed, 0x%x!\n", Status);
		goto Error;
	}

	printf("[strm][%p] Starting...\n", Stream);

	// Starts the bidirectional stream. By default, the peer is not
	// notified of the stream being started until data is sent on the
	// stream.
	if (QUIC_FAILED(Status = p->MsQuic->StreamStart(
	                    Stream, QUIC_STREAM_START_FLAG_NONE))) {
		printf("StreamStart failed, 0x%x!\n", Status);
		p->MsQuic->StreamClose(Stream);
		goto Error;
	}

Error:

	if (QUIC_FAILED(Status)) {
		p->MsQuic->ConnectionShutdown(
		    Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
	}
}

void
quic_mqtt_stream_close(void *arg)
{
	mqtt_pipe_s *p = arg;
	p->MsQuic->ConnectionShutdown(
	    p->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
}

static nni_proto_sock_ops mqtt_sock_ops = {
	.sock_size    = sizeof(mqtt_sock_t),
	.sock_init    = mqtt_quic_sock_init,
	.sock_fini    = mqtt_quic_sock_fini,
	.sock_open    = mqtt_quic_sock_open,
	.sock_close   = mqtt_quic_sock_close,
	.sock_options = mqtt_quic_sock_options,
	.sock_send    = mqtt_quic_sock_send,
	.sock_recv    = mqtt_quic_sock_recv,
};

void
quic_mqtt_client_open(nng_socket *sock)
{
	// return (nni_proto_open(sock, &quic_mqtt_proto));
	// return (mqtt_quic_sock_init(sock, NULL) &&
	//		mqtt_quic_sock_open(sock));
}

