#include "quic_api.h"
#include "core/nng_impl.h"
#include "msquic.h"
#include "nng/mqtt/mqtt_client.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct quic_strm_s quic_strm_t;

struct quic_strm_s {
	HQUIC    stream;
	void    *pipe;
	nni_mtx  mtx;
	nni_list sendq;
	nni_list recvq;
	nni_aio *txaio;
	nni_aio *rxaio;
	bool     closed;
	nni_lmq  recv_messages; // recv messages queue
	nni_lmq  send_messages; // send messages queue
};

// Config for msquic
const QUIC_REGISTRATION_CONFIG RegConfig = { "mqtt",
	QUIC_EXECUTION_PROFILE_LOW_LATENCY };
const QUIC_BUFFER     Alpn = { sizeof("mqtt") - 1, (uint8_t *) "mqtt" };
const uint64_t        IdleTimeoutMs    = 0;
const uint32_t        SendBufferLength = 100;
const QUIC_API_TABLE *MsQuic;
HQUIC                 Registration;
HQUIC                 Configuration;

quic_strm_t *GStream;

nni_proto *g_quic_proto;

static int quic_pipe_start(
    _In_ HQUIC Connection, _In_ void *Context, _Out_ HQUIC *Streamp);
static BOOLEAN LoadConfiguration(BOOLEAN Unsecure);
static void    quic_strm_send_cancel(nni_aio *aio, void *arg, int rv);
static void    quic_strm_send_start(quic_strm_t *qstrm);
static int     quic_strm_alloc(quic_strm_t **qstrmp);

// Helper function to load a client configuration.
static BOOLEAN
LoadConfiguration(BOOLEAN Unsecure)
{
	QUIC_SETTINGS Settings = { 0 };
	// Configures the client's idle timeout.
	Settings.IdleTimeoutMs       = IdleTimeoutMs;
	Settings.IsSet.IdleTimeoutMs = FALSE;

	// Configures a default client configuration, optionally disabling
	// server certificate validation.
	QUIC_CREDENTIAL_CONFIG CredConfig;
	memset(&CredConfig, 0, sizeof(CredConfig));
	CredConfig.Type  = QUIC_CREDENTIAL_TYPE_NONE;
	CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
	if (Unsecure) {
		CredConfig.Flags |=
		    QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
	}

	// Allocate/initialize the configuration object, with the configured
	// ALPN and settings.
	QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
	if (QUIC_FAILED(
	        Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1,
	            &Settings, sizeof(Settings), NULL, &Configuration))) {
		printf("ConfigurationOpen failed, 0x%x!\n", Status);
		return FALSE;
	}

	// Loads the TLS credential part of the configuration. This is required
	// even on client side, to indicate if a certificate is required or
	// not.
	if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(
	                    Configuration, &CredConfig))) {
		printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
		return FALSE;
	}

	return TRUE;
}

static int
quic_strm_alloc(quic_strm_t **qstrmp)
{
	quic_strm_t *qstrm;
	qstrm = malloc(sizeof(quic_strm_t));

	qstrm->closed = false;
	qstrm->rxaio  = NULL;
	qstrm->txaio  = NULL;
	nni_mtx_init(&qstrm->mtx);
	nni_aio_list_init(&qstrm->sendq);
	nni_aio_list_init(&qstrm->recvq);

	*qstrmp = qstrm;

	return 0;
}

// The clients's callback for stream events from MsQuic.
// New recv cb of quic transport
_IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_STREAM_CALLBACK) QUIC_STATUS QUIC_API
    QuicStreamCallback(_In_ HQUIC Stream, _In_opt_ void *Context,
        _Inout_ QUIC_STREAM_EVENT *Event)
{
	quic_strm_t *qstrm = Context;

	switch (Event->Type) {
	case QUIC_STREAM_EVENT_SEND_COMPLETE:
		// A previous StreamSend call has completed, and the context is
		// being returned back to the app.
		// free(Event->SEND_COMPLETE.ClientContext);
		printf("[strm][%p] Data sent\n", Stream);

		// TODO Find aio from sendq and finish (BUG here)
		// nni_aio_finish(qstrm->txaio, 0, 0);
		break;
	case QUIC_STREAM_EVENT_RECEIVE:
		// Data was received from the peer on the stream.
		printf("[strm][%p] Data received\n", Stream);
		printf("Body is [%d][%x][%x].\n",
		    Event->RECEIVE.Buffers->Length,
		    *(Event->RECEIVE.Buffers->Buffer),
		    *(Event->RECEIVE.Buffers->Buffer + 1));
		// store to lmq
		// get aio and trigger cb of protocol layer
                nni_mtx_lock(&qstrm->mtx);
		nni_aio *aio = nni_list_first(&qstrm->recvq);
                nni_mtx_unlock(&qstrm->mtx);
		if (aio != NULL) {
			nni_aio_finish_sync(aio, 0, 0);
		}

		break;
	case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
		// The peer gracefully shut down its send direction of the
		// stream.
		printf("[strm][%p] Peer aborted\n", Stream);
		break;
	case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
		// The peer aborted its send direction of the stream.
		printf("[strm][%p] Peer shut down\n", Stream);
		break;
	case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
		// Both directions of the stream have been shut down and MsQuic
		// is done with the stream. It can now be safely cleaned up.
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

_IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_CONNECTION_CALLBACK) QUIC_STATUS QUIC_API
    QuicConnectionCallback(_In_ HQUIC Connection, _In_opt_ void *Context,
        _Inout_ QUIC_CONNECTION_EVENT *Event)
{
	nni_proto_pipe_ops *pipe_ops = g_quic_proto->proto_pipe_ops;
	quic_strm_t        *qstrm    = NULL;

	switch (Event->Type) {
	case QUIC_CONNECTION_EVENT_CONNECTED:
		// The handshake has completed for the connection.
		// do not init any var here due to potential frequent reconnect
		printf("[conn][%p] Connected\n", Connection);

		// Create a pipe for quic client
		if (0 != quic_strm_alloc(&qstrm)) {
			printf("Error in alloc quic strm alloc.\n");
		}
		GStream = qstrm; // TODO Replace with getting from array

		qstrm->pipe = nng_alloc(pipe_ops->pipe_size);
		nni_lmq_init(&qstrm->recv_messages, NNG_MAX_RECV_LMQ);
		nni_lmq_init(&qstrm->send_messages, NNG_MAX_SEND_LMQ);

		pipe_ops->pipe_init(qstrm->pipe, qstrm, Context);

		if (0 != quic_pipe_start(Connection, qstrm, &qstrm->stream)) {
			printf("Error in quic pipe start.\n");
			pipe_ops->pipe_fini(qstrm->pipe);
		}

		pipe_ops->pipe_start(qstrm->pipe);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
		// The connection has been shut down by the transport.
		// Generally, this is the expected way for the connection to
		// shut down with this protocol, since we let idle timeout kill
		// the connection.
		if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status ==
		    QUIC_STATUS_CONNECTION_IDLE) {
			printf("[conn][%p] Successfully shut down on idle.\n",
			    Connection);
		} else {
			printf("[conn][%p] Shut down by transport, 0x%x\n",
			    Connection,
			    Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
		}
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
		// The connection was explicitly shut down by the peer.
		printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection,
		    (unsigned long long)
		        Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
		pipe_ops->pipe_close(GStream->pipe);
		pipe_ops->pipe_fini(GStream->pipe);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
		// The connection has completed the shutdown process and is
		// ready to be safely cleaned up.
		printf("[conn][%p] All done\n", Connection);
		if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
			MsQuic->ConnectionClose(Connection);
		}

		pipe_ops->pipe_close(GStream->pipe);
		pipe_ops->pipe_fini(GStream->pipe);
		break;
	case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
		// A resumption ticket (also called New Session Ticket or NST)
		// was received from the server.
		printf("[conn][%p] Resumption ticket received (%u bytes):\n",
		    Connection,
		    Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
		for (uint32_t i = 0; i <
		     Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength;
		     i++) {
			printf("%.2X",
			    (uint8_t) Event->RESUMPTION_TICKET_RECEIVED
			        .ResumptionTicket[i]);
		}
		printf("\n");
		break;
	default:
		break;
	}
	return QUIC_STATUS_SUCCESS;
}

/**
 * @brief
 *
 * @param Connection
 * @param Context
 * @param Streamp
 * @return int
 */
static int
quic_pipe_start(
    _In_ HQUIC Connection, _In_ void *Context, _Out_ HQUIC *Streamp)
{
	HQUIC       Stream = NULL;
	QUIC_STATUS Status;

	// Create/allocate a new bidirectional stream. The stream is just
	// allocated and no QUIC stream identifier is assigned until it's
	// started.
	if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection,
	                    QUIC_STREAM_OPEN_FLAG_NONE, QuicStreamCallback,
	                    Context, &Stream))) {
		printf("StreamOpen failed, 0x%x!\n", Status);
		goto Error;
	}

	printf("[strm][%p] Starting...\n", Stream);

	// Starts the bidirectional stream. By default, the peer is not
	// notified of the stream being started until data is sent on the
	// stream.
	if (QUIC_FAILED(Status = MsQuic->StreamStart(
	                    Stream, QUIC_STREAM_START_FLAG_NONE))) {
		printf("StreamStart failed, 0x%x!\n", Status);
		MsQuic->StreamClose(Stream);
		goto Error;
	}

	printf("[strm][%p] Done...\n", Stream);
	*Streamp = Stream;
	return 0;

Error:

	if (QUIC_FAILED(Status)) {
		MsQuic->ConnectionShutdown(
		    Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
	}
	return (-1);
}

void
quic_proto_open(nni_proto *proto)
{
	g_quic_proto = proto;
}

static void
quic_close()
{
        
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
	if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(
	                    &RegConfig, &Registration))) {
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
			// This will block until all outstanding child objects
			// have been closed.
			MsQuic->RegistrationClose(Registration);
		}
		MsQuicClose(MsQuic);
	}
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
	if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration,
	                    QuicConnectionCallback, NULL, &Connection))) {
		printf("ConnectionOpen failed, 0x%x!\n", Status);
		goto Error;
	}

	nng_url *url_s;

	if (url == NULL)
		url = "mqtt-quic://54.75.171.11:14567";

	nng_url_parse(&url_s, url);
	for (int i = 0; i < strlen(url_s->u_host); ++i)
		if (url_s->u_host[i] == ':') {
			url_s->u_host[i] = '\0';
			break;
		}

	printf("[conn] Connecting... %s : %s\n", url_s->u_host, url_s->u_port);

	// Start the connection to the server.
	if (QUIC_FAILED(Status = MsQuic->ConnectionStart(Connection,
	                    Configuration, QUIC_ADDRESS_FAMILY_UNSPEC,
	                    url_s->u_host, atoi(url_s->u_port)))) {
		printf("ConnectionStart failed, 0x%x!\n", Status);
		goto Error;
	}

Error:

	if (QUIC_FAILED(Status) && Connection != NULL) {
		MsQuic->ConnectionClose(Connection);
	}

	return 0;
}

static void
quic_strm_send_start(quic_strm_t *qstrm)
{
	nni_aio    *aio;
	nni_msg    *msg;
	QUIC_STATUS Status;

	if (qstrm->closed) {
		while ((aio = nni_list_first(&qstrm->sendq)) != NULL) {
			nni_list_remove(&qstrm->sendq, aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		return;
	}

	if ((aio = nni_list_first(&qstrm->sendq)) == NULL) {
		return;
	}

	// This runs to send the message.
	msg = nni_aio_get_msg(aio);

	QUIC_BUFFER *bufs = NULL;
	int          hl   = nni_msg_header_len(msg);
	int          bl   = nni_msg_len(msg);

	if (hl > 0 && bl > 0) {
		bufs = malloc(sizeof(QUIC_BUFFER) + bl + hl);
		memcpy((char *) (bufs + 1), nni_msg_header(msg), hl);
		memcpy((char *) (bufs + 1) + hl, nni_msg_body(msg), bl);
		bufs->Length = hl + bl;
		bufs->Buffer = bufs + 1;
	}

	if (!bufs)
		printf("error in iov.\n");

	if (QUIC_FAILED(Status = MsQuic->StreamSend(qstrm->stream, bufs, 1,
	                    QUIC_SEND_FLAG_NONE, bufs))) {
		printf("StreamSend failed, 0x%x!\n", Status);
		free(bufs);
	}

	/*
	niov  = 0;
	if (nni_msg_header_len(msg) > 0) {
	        iov[niov].iov_buf = nni_msg_header(msg);
	        iov[niov].iov_len = nni_msg_header_len(msg);
	        niov++;
	}
	if (nni_msg_len(msg) > 0) {
	        iov[niov].iov_buf = nni_msg_body(msg);
	        iov[niov].iov_len = nni_msg_len(msg);
	        niov++;
	}
	nni_aio_set_iov(txaio, niov, iov);
	nng_stream_send(p->conn, txaio);
	*/
}

static void
quic_strm_send_cancel(nni_aio *aio, void *arg, int rv)
{
	quic_strm_t *qstrm = arg;

	nni_mtx_lock(&qstrm->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&qstrm->mtx);
		return;
	}
	// If this is being sent, then cancel the pending transfer.
	// The callback on the txaio will cause the user aio to
	// be canceled too.
	if (nni_list_first(&qstrm->sendq) == aio) {
		nni_aio_abort(qstrm->txaio, rv);
		nni_mtx_unlock(&qstrm->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&qstrm->mtx);

	nni_aio_finish_error(aio, rv);
}

int
quic_strm_recv(void *arg, nni_aio *raio)
{
	quic_strm_t *qstrm = arg;
	int                rv;

	if (nni_aio_begin(raio) != 0) {
		return;
	}
	// if ((rv = nni_aio_schedule(aio, mqtt_tcptran_pipe_recv_cancel, p)) !=
	//     0) {
	// 	nni_aio_finish_error(aio, rv);
	// 	return;
	// }

	nni_list_append(&qstrm->recvq, raio);
	if (nni_list_first(&qstrm->recvq) == raio) {
	}
	return 0;
}

int
quic_strm_send(void *arg, nni_aio *aio)
{
	int          rv;
	quic_strm_t *qstrm = arg;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	// nni_mtx_lock(&qstrm->mtx);
	/*
	if ((rv = nni_aio_schedule(aio, quic_strm_send_cancel, qstrm)) != 0) {
	        nni_mtx_unlock(&qstrm->mtx);
	        nni_aio_finish_error(aio, rv);
	        return (-1);
	}
	*/
	qstrm->txaio = aio;
	nni_list_append(&qstrm->sendq, aio);
	if (nni_list_first(&qstrm->sendq) == aio) {
		quic_strm_send_start(qstrm);
	}
	// nni_mtx_unlock(&qstrm->mtx);

	return 0;
}

// unite init of msquic here, deal with cb of stream
static int
quic_alloc()
{
	return 0;
}

int
nni_msquic_dialer_alloc(nng_stream_dialer **dp, const nng_url *url)
{
	return 0;
}

int
nni_msquic_listener_alloc(nng_stream_listener **lp, const nng_url *url)
{
	return 0;
}
