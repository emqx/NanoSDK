//
// Copyright 2022 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//
#include "quic_api.h"
#include "core/nng_impl.h"
#include "msquic.h"

#include "nng/supplemental/util/platform.h"
#include "nng/mqtt/mqtt_quic.h"
#include "nng/mqtt/mqtt_client.h"
#include "supplemental/mqtt/mqtt_msg.h"

#include "openssl/pem.h"
#include "openssl/x509.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NNI_QUIC_KEEPALIVE 100
#define NNI_QUIC_TIMER 1
#define NNI_QUIC_MAX_RETRY 2

#define QUIC_API_C_DEBUG 0


#if QUIC_API_C_DEBUG
#define qdebug(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_debug(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_info(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_warn(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_error(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#else
#define qdebug(fmt, ...) do {} while(0)
#define log_debug(fmt, ...) do {} while(0)
#define log_info(fmt, ...) do {} while(0)
#define log_warn(fmt, ...) do {} while(0)
#define log_error(fmt, ...) do {} while(0)
#endif

typedef struct quic_sock_s quic_sock_t;

struct quic_sock_s {
	HQUIC     qconn; // QUIC connection
	nni_sock *sock;
	bool      closed;
	void     *pipe; //main mqtt_pipe

	nni_mtx  mtx; // for reconnect
	nni_aio  close_aio;

	uint8_t  rticket[4096];
	uint16_t rticket_sz;
	nng_url *url_s;

	char    *cacert;
	bool     enable_0rtt;
	uint8_t  reason_code; // reason code in transport layer
};

typedef struct quic_strm_s quic_strm_t;

struct quic_strm_s {
	HQUIC    stream;
	nni_mtx  mtx;
	nni_list sendq;
	nni_list recvq;

	quic_sock_t *sock; // QUIC socket
	void        *pipe; // Stream pipe if multi-stream is enabled

	bool     inrr;          // is rraio in run_q
	bool     closed;
	nni_lmq  recv_messages; // recv messages queue
	nni_lmq  send_messages; // send messages queue
	nni_msg *rxmsg;         // nng_msg for received

	nni_aio  rraio;
	uint8_t *rrbuf; // Buffer for remaining packet
	uint32_t rrlen; // Length of rrbuf
	uint32_t rrpos; // Start position of rrbuf
	uint32_t rrcap; // Start position of rrbuf
	uint32_t rxlen; // Length received
	uint32_t rwlen; // Length wanted
	uint8_t  rxbuf[5];
};

const QUIC_API_TABLE *MsQuic;

// Config for msquic
const QUIC_REGISTRATION_CONFIG quic_reg_config = {
	"mqtt",
	QUIC_EXECUTION_PROFILE_LOW_LATENCY
};

const QUIC_BUFFER quic_alpn = {
	sizeof("mqtt") - 1,
	(uint8_t *) "mqtt"
};

HQUIC registration;
HQUIC configuration;

static conf_quic conf_node;
nni_proto *g_quic_proto;

static BOOLEAN quic_load_sdk_config(BOOLEAN Unsecure);

static void    quic_pipe_send_cancel(nni_aio *aio, void *arg, int rv);
static void    quic_pipe_recv_cancel(nni_aio *aio, void *arg, int rv);
static void    quic_pipe_recv_cb(void *arg);
static void    quic_pipe_send_start(quic_strm_t *qstrm);
static void    quic_pipe_recv_start(void *arg);

static int  quic_sock_reconnect(quic_sock_t *qsock);
static void quic_sock_close_cb(void *arg);
static void quic_sock_init(quic_sock_t *qsock);
static void quic_sock_fini(quic_sock_t *qsock);

static void quic_strm_init(quic_strm_t *qstrm, quic_sock_t *qsock);
static void quic_strm_fini(quic_strm_t *qstrm);

static QUIC_STATUS verify_peer_cert_tls(
    QUIC_CERTIFICATE *cert, QUIC_CERTIFICATE *chain, char *cacert);

static QUIC_STATUS
verify_peer_cert_tls(QUIC_CERTIFICATE* cert, QUIC_CERTIFICATE* chain, char *cacert)
{
	// local ca
	X509_LOOKUP *lookup = NULL;
	X509_STORE *trusted = NULL;
	trusted = X509_STORE_new();
	if (trusted == NULL) {
		return QUIC_STATUS_ABORTED;
	}
	lookup = X509_STORE_add_lookup(trusted, X509_LOOKUP_file());
	if (lookup == NULL) {
		X509_STORE_free(trusted);
		trusted = NULL;
		return QUIC_STATUS_ABORTED;
	}

	// if (!X509_LOOKUP_load_file(lookup, cacertfile, X509_FILETYPE_PEM)) {
	if (!X509_LOOKUP_load_file(lookup, cacert, X509_FILETYPE_PEM)) {
		log_warn("No load cacertfile be found");
		X509_STORE_free(trusted);
		trusted = NULL;
	}
	if (trusted == NULL) {
		return QUIC_STATUS_ABORTED;
	}

	// @TODO peer_certificate_received
	// Only with QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
	// assert(QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED == Event->Type);

	// Validate against CA certificates using OpenSSL API
	X509 *crt = (X509 *) cert;
	X509_STORE_CTX *x509_ctx = (X509_STORE_CTX *) chain;
	STACK_OF(X509) *untrusted = X509_STORE_CTX_get0_untrusted(x509_ctx);

	if (crt == NULL) {
		log_error("NULL Cert!");
		return QUIC_STATUS_BAD_CERTIFICATE;
	}
	X509_STORE_CTX *ctx = X509_STORE_CTX_new();
	X509_STORE_CTX_init(ctx, trusted, crt, untrusted);
	int res = X509_verify_cert(ctx);
	X509_STORE_CTX_free(ctx);

	if (res <= 0) {
		log_error("X509 verify error: %d: %s", res, X509_verify_cert_error_string(ctx));
		return QUIC_STATUS_BAD_CERTIFICATE;
	} else
		return QUIC_STATUS_SUCCESS;

	/* @TODO validate SNI */
}

// Helper function to load a client configuration.
static BOOLEAN
quic_load_sdk_config(BOOLEAN Unsecure)
{
	NNI_ARG_UNUSED(Unsecure);
	QUIC_SETTINGS          Settings = { 0 };
	QUIC_CREDENTIAL_CONFIG CredConfig;

	conf_quic *node = &conf_node;

	if (!node) {
		Settings.IsSet.IdleTimeoutMs       = TRUE;
		Settings.IdleTimeoutMs             = 90 * 1000;
		Settings.IsSet.KeepAliveIntervalMs = TRUE;
		Settings.KeepAliveIntervalMs       = 60 * 1000;
		goto there;
	}
	// Configures the client's idle timeout.
	if (node->qidle_timeout == 0) {
		Settings.IsSet.IdleTimeoutMs = FALSE;
	} else {
		Settings.IsSet.IdleTimeoutMs = TRUE;
		Settings.IdleTimeoutMs       = node->qidle_timeout * 1000;
	}
	if (node->qconnect_timeout != 0) {
		Settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
		Settings.HandshakeIdleTimeoutMs =
		    node->qconnect_timeout * 1000;
	}
	if (node->qdiscon_timeout != 0) {
		Settings.IsSet.DisconnectTimeoutMs = TRUE;
		Settings.DisconnectTimeoutMs       = node->qdiscon_timeout * 1000;
	}

	Settings.IsSet.KeepAliveIntervalMs = TRUE;
	Settings.KeepAliveIntervalMs       = node->qkeepalive * 1000;
	switch (node->qcongestion_control)
	{
	case QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC:
		Settings.IsSet.CongestionControlAlgorithm = TRUE;
		Settings.CongestionControlAlgorithm       = QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC;
		break;

	default:
		log_warn("unsupport congestion control algorithm, use default cubic!");
		break;
	}

there:

	// Configures a default client configuration, optionally disabling
	// server certificate validation.
	memset(&CredConfig, 0, sizeof(CredConfig));
	// Unsecure by default
	CredConfig.Type  = QUIC_CREDENTIAL_TYPE_NONE;
	// CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
	CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;

	if (node->tls.enable) {
		char *cert_path = node->tls.certfile;
		char *key_path  = node->tls.keyfile;
		char *password  = node->tls.key_password;

		if (password) {
			QUIC_CERTIFICATE_FILE_PROTECTED *CertFile =
			    (QUIC_CERTIFICATE_FILE_PROTECTED *) malloc(sizeof(QUIC_CERTIFICATE_FILE_PROTECTED));
			CertFile->CertificateFile           = cert_path;
			CertFile->PrivateKeyFile            = key_path;
			CertFile->PrivateKeyPassword        = password;
			CredConfig.CertificateFileProtected = CertFile;
			CredConfig.Type =
			    QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
		} else {
			QUIC_CERTIFICATE_FILE *CertFile =
			    (QUIC_CERTIFICATE_FILE_PROTECTED *) malloc(sizeof(QUIC_CERTIFICATE_FILE_PROTECTED));
			CertFile->CertificateFile  = cert_path;
			CertFile->PrivateKeyFile   = key_path;
			CredConfig.CertificateFile = CertFile;
			CredConfig.Type =
			    QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
		}

		BOOLEAN verify = (node->tls.verify_peer == true ? 1 : 0);
		BOOLEAN has_ca_cert = (node->tls.cafile != NULL ? 1 : 0);
		if (!verify) {
			CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
		} else if (has_ca_cert) {
			// Do own validation instead against provided ca certs in cacertfile
			CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED;
			CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
		}

		CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
		CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED;
	} else {
		CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
		log_warn("No quic TLS/SSL credentials was specified.");
	}

	// Allocate/initialize the configuration object, with the configured
	// ALPN and settings.
	QUIC_STATUS rv = QUIC_STATUS_SUCCESS;
	if (QUIC_FAILED(rv = MsQuic->ConfigurationOpen(registration,
	    &quic_alpn, 1, &Settings, sizeof(Settings), NULL, &configuration))) {
		log_error("ConfigurationOpen failed, 0x%x!\n", rv);
		return FALSE;
	}

	// Loads the TLS credential part of the configuration. This is required
	// even on client side, to indicate if a certificate is required or
	// not.
	if (QUIC_FAILED(rv = MsQuic->ConfigurationLoadCredential(
	                    configuration, &CredConfig))) {
		qdebug("Configuration Load Credential failed, 0x%x!\n", rv);
		return FALSE;
	}

	return TRUE;
}

static void
quic_sock_init(quic_sock_t *qsock)
{
	qsock->qconn  = NULL;
	qsock->sock   = NULL;
	qsock->pipe   = NULL;
	qsock->closed = 0;

	nni_mtx_init(&qsock->mtx);
	nni_aio_init(&qsock->close_aio, quic_sock_close_cb, qsock);

	qsock->url_s       = NULL;
	qsock->rticket_sz  = 0;
	qsock->cacert      = NULL;
	qsock->enable_0rtt = false;
	qsock->closed      = false;
	qsock->reason_code = 0;
}

void
quic_sock_close(void *qsock)
{
	log_debug("actively disclose the QUIC Socket");
	quic_sock_t *qs    = qsock;

	nni_mtx_lock(&qs->mtx);
	qs->closed = true;
	nni_mtx_unlock(&qs->mtx);
}

static void
quic_sock_fini(quic_sock_t *qsock)
{
	nni_mtx_fini(&qsock->mtx);
	if (qsock->url_s)
		nng_url_free(qsock->url_s);
	nni_aio_stop(&qsock->close_aio);
	nni_aio_fini(&qsock->close_aio);
}

static void
quic_strm_init(quic_strm_t *qstrm, quic_sock_t *qsock)
{
	qstrm->stream = NULL;

	nni_mtx_init(&qstrm->mtx);
	nni_aio_list_init(&qstrm->sendq);
	nni_aio_list_init(&qstrm->recvq);

	qstrm->sock = qsock;
	qstrm->closed = false;
	qstrm->inrr   = false;

	nni_lmq_init(&qstrm->recv_messages, NNG_MAX_RECV_LMQ);
	nni_lmq_init(&qstrm->send_messages, NNG_MAX_SEND_LMQ);

	nni_aio_init(&qstrm->rraio, quic_pipe_recv_cb, qstrm);

	qstrm->rxlen = 0;
	qstrm->rxmsg = NULL;

	qstrm->rrbuf = NULL;
	qstrm->rrlen = 0;
	qstrm->rrpos = 0;
	qstrm->rrcap = 0;
}

static void
quic_strm_fini(quic_strm_t *qstrm)
{
	if (qstrm == NULL)
		return;

	nni_aio_stop(&qstrm->rraio);
	nni_aio_close(&qstrm->rraio);
	nni_aio_fini(&qstrm->rraio);

	if (qstrm->rxmsg)
		free(qstrm->rxmsg);
	if (qstrm->rrbuf)
		free(qstrm->rrbuf);
	log_debug("%p quic_strm_fini", qstrm->stream);

	nni_lmq_fini(&qstrm->recv_messages);
	nni_lmq_fini(&qstrm->send_messages);
	nni_mtx_fini(&qstrm->mtx);
}

// The clients's callback for stream events from MsQuic.
// New recv cb of quic transport
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK) QUIC_STATUS QUIC_API
quic_strm_cb(_In_ HQUIC stream, _In_opt_ void *Context,
	_Inout_ QUIC_STREAM_EVENT *Event)
{
	quic_strm_t *qstrm = Context;
	quic_sock_t *qsock = qstrm->sock;
	uint32_t rlen;
	uint8_t *rbuf;
	nni_msg *smsg;
	nni_aio *aio;

	log_debug("quic_strm_cb triggered! %d", Event->Type);
	switch (Event->Type) {
	case QUIC_STREAM_EVENT_SEND_COMPLETE:
		// A previous StreamSend call has completed, and the context is
		// being returned back to the app.
		log_debug("QUIC_STREAM_EVENT_SEND_COMPLETE!");
		if (Event->SEND_COMPLETE.Canceled) {
			log_warn("[strm][%p] Data sent Canceled: %d",
					 stream, Event->SEND_COMPLETE.Canceled);
		}
		// Get aio from sendq and finish
		nni_mtx_lock(&qstrm->mtx);
		aio = Event->SEND_COMPLETE.ClientContext;
		if (aio != NULL) {
			// QoS messages send_cb
			nni_aio_list_remove(aio);
			// free the buf
			QUIC_BUFFER *buf = nni_aio_get_input(aio, 0);
			free(buf);
			smsg = nni_aio_get_msg(aio);
			nni_mtx_unlock(&qstrm->mtx);
			nni_msg_free(smsg);
			nni_aio_set_msg(aio, NULL);
			//Process QoS ACK in Protocol layer
			nni_aio_finish_sync(aio, 0, 0);
			break;
		}
		if ((aio = nni_list_first(&qstrm->sendq)) != NULL) {
			// this is the send_aio in protocol layer
			nni_aio_list_remove(aio);
			QUIC_BUFFER *buf = nni_aio_get_input(aio, 0);
			free(buf);
			quic_pipe_send_start(qstrm);
			nni_mtx_unlock(&qstrm->mtx);
			smsg = nni_aio_get_msg(aio);
			nni_msg_free(smsg);
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish_sync(aio, 0, 0);
			break;
		} else {
			log_error("AIO missing, potential msg leaking!");
		}
		nni_mtx_unlock(&qstrm->mtx);
		break;
	case QUIC_STREAM_EVENT_RECEIVE:
		// Data was received from the peer on the stream.
		rbuf = Event->RECEIVE.Buffers->Buffer;
		rlen = Event->RECEIVE.Buffers->Length;
		uint8_t count = Event->RECEIVE.BufferCount;

		log_debug("[strm][%p] Data received Flag: %d", stream, Event->RECEIVE.Flags);

		if (Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) {
			if (qsock->reason_code == 0)
				qsock->reason_code = CLIENT_IDENTIFIER_NOT_VALID;
			log_warn("FIN received in QUIC stream");
			break;
		}

		nni_mtx_lock(&qstrm->mtx);
		// Get all the buffers in quic stream
		if (count == 0 || rlen <= 0) {
			nni_mtx_unlock(&qstrm->mtx);
			return QUIC_STATUS_PENDING;
		}
		log_debug("Body is [%d]-[0x%x 0x%x].", rlen, *(rbuf), *(rbuf + 1));

		if (rlen > qstrm->rrcap - qstrm->rrlen - qstrm->rrpos) {
			qstrm->rrbuf = realloc(qstrm->rrbuf, rlen + qstrm->rrlen + qstrm->rrpos);
			qstrm->rrcap = rlen + qstrm->rrlen + qstrm->rrpos;
		}
		// Copy data from quic stream to rrbuf
		memcpy(qstrm->rrbuf + qstrm->rrpos + (int)qstrm->rrlen, rbuf, rlen);
		qstrm->rrlen += rlen;
		MsQuic->StreamReceiveComplete(qstrm->stream, rlen);
		if (!nni_list_empty(&qstrm->recvq) && qstrm->inrr == false) {
			// We should not do executing now, Or circle calling occurs
			// nng_aio_wait(&qstrm->rraio);
			qstrm->inrr = true;
			nni_aio_finish(&qstrm->rraio, 0, 0);
		}
		nni_mtx_unlock(&qstrm->mtx);
		log_debug("stream cb over\n");

		return QUIC_STATUS_PENDING;
	case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
		// The peer gracefully shut down its send direction of the
		// stream.
		log_warn("[strm][%p] Peer SEND aborted\n", stream);
		log_info("PEER_SEND_ABORTED Error Code: %llu",
		    (unsigned long long) Event->PEER_SEND_ABORTED.ErrorCode);
		if (qsock->reason_code == 0)
			qsock->reason_code = SERVER_SHUTTING_DOWN;
		break;
	case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
		// The peer aborted its send direction of the stream.
		log_warn("[strm][%p] Peer send shut down\n", stream);
		MsQuic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
		break;
	case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
		// fall through to close the stream
		log_warn("[strm][%p] QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE.", stream);
		break;
	case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
		// Both directions of the stream have been shut down and MsQuic
		// is done with the stream. It can now be safely cleaned up.
		log_warn("[strm][%p] QUIC_STREAM_EVENT shutdown: All done.",
		    stream);
		log_info("close stream with Error Code: %llu",
		    (unsigned long long)
		        Event->SHUTDOWN_COMPLETE.ConnectionErrorCode);
		if (qstrm->sock->pipe != qstrm->pipe) {
			// close data stream only
			const nni_proto_pipe_ops *pipe_ops =
			    g_quic_proto->proto_pipe_ops;
			log_warn("close the data stream [%p]!", stream);
			pipe_ops->pipe_stop(qstrm->pipe);
			if (qstrm->closed != true)
				MsQuic->StreamClose(stream);
			break;
		}
		if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
			// only server close the main stream gonna trigger this
			log_warn("close the main stream [%p]!", stream);
			if (qstrm->closed != true) {
				MsQuic->ConnectionShutdown(stream,
				    QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
				MsQuic->StreamClose(stream);
			}
			qstrm->stream = NULL;
			// close stream here if in multi-stream mode?
			// Conflic with quic_pipe_close
			// qstrm->closed = true;
		}
		break;
	case QUIC_STREAM_EVENT_START_COMPLETE:
		log_info(
		    "QUIC_STREAM_EVENT_START_COMPLETE [%p] ID: %ld Status: %d",
		    stream, Event->START_COMPLETE.ID,
		    Event->START_COMPLETE.Status);
		if (!Event->START_COMPLETE.PeerAccepted)
			log_warn("Peer refused");
		break;
	case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
		log_info("QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE");
		break;
	case QUIC_STREAM_EVENT_PEER_ACCEPTED:
		log_info("QUIC_STREAM_EVENT_PEER_ACCEPTED");
		break;
	case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
		// The peer has requested that we stop sending. Close abortively.
		log_warn("[strm][%p] Peer RECEIVE aborted\n", stream);
		log_warn("QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED Error Code: %llu",
				 (unsigned long long) Event->PEER_RECEIVE_ABORTED.ErrorCode);
		break;

	default:
		log_warn("Unknown Event Type %d", Event->Type);
		break;
	}
	return QUIC_STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK) QUIC_STATUS QUIC_API
quic_connection_cb(_In_ HQUIC Connection, _In_opt_ void *Context,
	_Inout_ QUIC_CONNECTION_EVENT *Event)
{
	const nni_proto_pipe_ops *pipe_ops = g_quic_proto->proto_pipe_ops;

	quic_sock_t *qsock = Context;
	void        *mqtt_sock;
	HQUIC        qconn = Connection;
	QUIC_STATUS  rv;

	log_debug("quic_connection_cb triggered! %d", Event->Type);
	switch (Event->Type) {
	case QUIC_CONNECTION_EVENT_CONNECTED:
		// The handshake has completed for the connection.
		// do not init any var here due to potential frequent reconnect
		log_info("[conn][%p] is Connected. Resumed Session %d", qconn,
		    Event->CONNECTED.SessionResumed);

		// only create main stream/pipe it there is none.
		if (qsock->pipe == NULL) {
			// First time to establish QUIC pipe
			if ((qsock->pipe = nng_alloc(pipe_ops->pipe_size)) == NULL) {
				log_error("Failed in allocating pipe.");
				break;
			}
			mqtt_sock = nni_sock_proto_data(qsock->sock);
			pipe_ops->pipe_init(
			    qsock->pipe, (nni_pipe *) qsock, mqtt_sock);
		}
		pipe_ops->pipe_start(qsock->pipe);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
		// The connection has been shut down by the transport.
		// Generally, this is the expected way for the connection to
		// shut down with this protocol, since we let idle timeout kill
		// the connection.
		if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status ==
		    QUIC_STATUS_CONNECTION_IDLE) {
			log_warn(
			    "[conn][%p] Successfully shut down connection on idle.\n",
			    qconn);
		} else if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status ==
		    QUIC_STATUS_CONNECTION_TIMEOUT) {
			log_warn("[conn][%p] Successfully shut down on "
			         "CONNECTION_TIMEOUT.\n",
			    qconn);
		}
		log_warn("[conn][%p] Shut down by transport, 0x%x, Error Code "
		         "%llu\n",
		    qconn, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
		    (unsigned long long)
		        Event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
		if (qsock->reason_code == 0)
            	qsock->reason_code = SERVER_UNAVAILABLE;
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
		// The connection was explicitly shut down by the peer.
		log_warn("[conn][%p] "
		         "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER, "
		         "0x%llu\n",
		    qconn,
		    (unsigned long long)
		        Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
		if (qsock->reason_code == 0)
			qsock->reason_code = SERVER_SHUTTING_DOWN;
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
		// The connection has completed the shutdown process and is
		// ready to be safely cleaned up.
		log_info("[conn][%p] QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: All done\n\n", qconn);
		nni_mtx_lock(&qsock->mtx);
		if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
			// explicitly shutdon on protocol layer.
			MsQuic->ConnectionClose(qconn);
		}

		// Close and finite nng pipe ONCE disconnect
		if (qsock->pipe) {
			log_warn("Quic reconnect failed or disconnected!");
			pipe_ops->pipe_stop(qsock->pipe);
			pipe_ops->pipe_close(qsock->pipe);
			pipe_ops->pipe_fini(qsock->pipe);
			qsock->pipe = NULL;
		}

		nni_mtx_unlock(&qsock->mtx);
		if (qsock->closed == true) {
			nni_sock_rele(qsock->sock);
		} else {
			nni_aio_finish(&qsock->close_aio, 0, 0);
		}
		/*
		if (qstrm->rtt0_enable) {
			// No rticket
			log_warn("reconnect failed due to no resumption ticket.\n");
			quic_strm_fini(qstrm);
			nng_free(qstrm, sizeof(quic_strm_t));
		}
		*/

		break;
	case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
		// A resumption ticket (also called New Session Ticket or NST)
		// was received from the server.
		log_warn("[conn][%p] Resumption ticket received (%u bytes):\n",
		    Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
		if (qsock->enable_0rtt == false) {
			log_warn("[conn][%p] Ignore ticket due to turn off the 0RTT");
			break;
		}
		qsock->rticket_sz = Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength;
		memcpy(qsock->rticket, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket,
		        Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
		break;
	case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
		log_info("QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED");

		// TODO Using mbedtls APIs to verify
		/*
		 * TODO
		 * Does MsQuic ensure the connected event will happen after
		 * peer_certificate_received event.?
		 */
		if (QUIC_FAILED(rv = verify_peer_cert_tls(
				Event->PEER_CERTIFICATE_RECEIVED.Certificate,
				Event->PEER_CERTIFICATE_RECEIVED.Chain, qsock->cacert))) {
			log_error("[conn][%p] Invalid certificate file received from the peer");
			return rv;
		}
		break;
	case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
		log_info("QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED");
		break;
	case QUIC_CONNECTION_EVENT_STREAMS_AVAILABLE:
		log_info("QUIC_CONNECTION_EVENT_STREAMS_AVAILABLE");
		break;
	case QUIC_CONNECTION_EVENT_IDEAL_PROCESSOR_CHANGED:
		log_info("QUIC_CONNECTION_EVENT_IDEAL_PROCESSOR_CHANGED");
		break;
	default:
		log_warn("Unknown event type %d!", Event->Type);
		break;
	}
	return QUIC_STATUS_SUCCESS;
}

int
quic_disconnect(void *qsock, void *qpipe)
{
	log_debug("actively disclose the QUIC stream");
	quic_sock_t *qs    = qsock;
	quic_strm_t *qstrm = qpipe;

	if (!qsock) {
		return -1;
	}
	// unable to close a closed pipe
	if (qstrm != NULL) {
		if (qstrm->closed == true || qstrm->stream == NULL)
			return -2;
		MsQuic->StreamShutdown(
		    qstrm->stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, NNG_ECONNSHUT);
	}
	qstrm->closed = true;
	nni_mtx_lock(&qs->mtx);
	MsQuic->ConnectionShutdown(
	    qs->qconn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, NNG_ECLOSED);

	nni_mtx_unlock(&qs->mtx);
	nni_aio_wait(&qs->close_aio);
	nni_aio_wait(&qstrm->rraio);

	return 0;
}

int
quic_connect_ipv4(const char *url, nni_sock *sock, uint32_t *index, void **qsockp)
{
	// Load the client configuration
	if (!quic_load_sdk_config(TRUE)) {
		log_error("Failed in load quic configuration");
		return (-1);
	}

	QUIC_STATUS  rv;
	HQUIC        conn = NULL;
	nng_url     *url_s;
	quic_sock_t *qsock;

	if ((qsock = malloc(sizeof(quic_sock_t))) == NULL) {
		return -2;
	}
	// never free the sock in bridging mode
	quic_sock_init(qsock);
	// CACert
	if (conf_node.tls.enable)
		qsock->cacert = conf_node.tls.cafile;

	// Allocate a new connection object.
	if (QUIC_FAILED(rv = MsQuic->ConnectionOpen(registration,
	        quic_connection_cb, (void *)qsock, &conn))) {
		log_error("Failed in Quic ConnectionOpen, 0x%x!", rv);
		goto error;
	}

	// TODO: Windows compatible
	QUIC_ADDR Address = { 0 };
	// Address.Ip.sa_family = QUIC_ADDRESS_FAMILY_UNSPEC;
	Address.Ip.sa_family = QUIC_ADDRESS_FAMILY_INET;
	Address.Ipv4.sin_port = htons(0);
	// QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
	// QuicAddrSetPort(&Address, 0);
	if (QUIC_FAILED(rv = MsQuic->SetParam(conn, QUIC_PARAM_CONN_LOCAL_ADDRESS,
	    sizeof(QUIC_ADDR), &Address))) {
		log_error("Failed in address setting, 0x%x!\n", rv);
		goto error;
	}
	// TODO get index from address
	if (index)
		if (QUIC_FAILED(rv = MsQuic->SetParam(conn,
		                    QUIC_PARAM_CONN_LOCAL_INTERFACE,
		                    sizeof(uint32_t), index))) {
			log_error(
			    "Failed in local address binding, 0x%x!\n", rv);
			goto error;
		}

	nng_url_parse(&url_s, url);
	// TODO maybe something wrong happened
	for (size_t i = 0; i < strlen(url_s->u_host); ++i)
		if (url_s->u_host[i] == ':') {
			url_s->u_host[i] = '\0';
			break;
		}

	qsock->url_s = url_s;
	qsock->sock  = sock;
	nni_sock_hold(sock);

	log_info("Quic connecting... %s:%s", url_s->u_host, url_s->u_port);

	// Start the connection to the server.
	if (QUIC_FAILED(rv = MsQuic->ConnectionStart(conn, configuration,
	        QUIC_ADDRESS_FAMILY_UNSPEC, url_s->u_host, atoi(url_s->u_port)))) {
		log_error("Failed in ConnectionStart, 0x%x!", rv);
		goto error;
	}
	// Successfully creating quic connection then assign to qsock
	// Here mutex should be unnecessary.
	qsock->qconn = conn;

	*qsockp = qsock;
	return 0;

error:

	if (QUIC_FAILED(rv) && conn != NULL) {
		MsQuic->ConnectionClose(conn);
	}
	return rv;
}

static int
quic_sock_reconnect(quic_sock_t *qsock)
{
	// Load the client configuration.
	if (!quic_load_sdk_config(TRUE)) {
		log_error("Failed in load quic configuration");
		return (-1);
	}

	QUIC_STATUS rv;
	HQUIC       conn = NULL;

	// Allocate a new connection object.
	if (QUIC_FAILED(rv = MsQuic->ConnectionOpen(registration,
	        quic_connection_cb, (void *)qsock, &conn))) {
		log_error("Failed in Quic ConnectionOpen, 0x%x!", rv);
		goto error;
	}

	if (qsock->rticket_sz != 0) {
		log_info("QUIC connection reconnect with 0RTT enabled");
		if (QUIC_FAILED(rv = MsQuic->SetParam(conn,
		    QUIC_PARAM_CONN_RESUMPTION_TICKET, qsock->rticket_sz, qsock->rticket))) {
			log_error("Failed in setting resumption ticket, 0x%x!", rv);
			goto error;
		}
	}

	nng_url *url_s = qsock->url_s;
	log_info("Quic reconnecting... %s:%s", url_s->u_host, url_s->u_port);

	// Start the connection to the server.
	if (QUIC_FAILED(rv = MsQuic->ConnectionStart(conn, configuration,
	        QUIC_ADDRESS_FAMILY_UNSPEC, url_s->u_host, atoi(url_s->u_port)))) {
		log_error("Failed in ConnectionStart, 0x%x!", rv);
		goto error;
	}
	// Successfully creating quic connection then assign to qsock
	nni_mtx_lock(&qsock->mtx);
	qsock->qconn = conn;
	nni_mtx_unlock(&qsock->mtx);

	return 0;

error:

	if (QUIC_FAILED(rv) && conn != NULL) {
		MsQuic->ConnectionClose(conn);
	}
	log_error("reconnect failed");
	return 0;
}

static void
quic_sock_close_cb(void *arg)
{
	quic_sock_t *qsock = arg;

	log_warn("Try to do quic stream reconnect!");
	nng_msleep(NNI_QUIC_TIMER * 1000);
	quic_sock_reconnect(qsock);
}


// only for qos 1/2
int
quic_aio_send(void *arg, nni_aio *aio)
{
	quic_strm_t *qstrm = arg;
	int          rv;
	nni_msg     *msg;
	QUIC_STATUS Status;

	nni_mtx_lock(&qstrm->mtx);
	msg = nni_aio_get_msg(aio);
	if ((rv = nni_aio_schedule(aio, quic_pipe_send_cancel, qstrm)) != 0) {
		nni_mtx_unlock(&qstrm->mtx);
		nni_msg_free(msg);
		nni_aio_finish_error(aio, rv);
		return (-1);
	}

	if (qstrm->closed) {
		qdebug("Sending msg on a closed pipe");
		nni_msg_free(msg);
		nni_mtx_unlock(&qstrm->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return 0;
	}

	QUIC_BUFFER *buf=(QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER)*2);
	int          hl   = nni_msg_header_len(msg);
	int          bl   = nni_msg_len(msg);

	if (hl > 0) {
		QUIC_BUFFER *buf1 = &buf[0];
		buf1->Length = hl;
		buf1->Buffer = nni_msg_header(msg);
	}

	if (bl > 0) {
		QUIC_BUFFER *buf2 = &buf[1];
		buf2->Length = bl;
		buf2->Buffer = nni_msg_body(msg);
	}

	log_debug("type is 0x%x %x.",
	    ((((uint8_t *) nni_msg_header(msg))[0] & 0xf0) >> 4),
	    ((uint8_t *) nni_msg_header(msg))[0]);
	log_debug("body len: %d header len: %d", buf[1].Length, buf[0].Length);
	nni_aio_set_input(aio, 0, buf);

	if (QUIC_FAILED(Status = MsQuic->StreamSend(qstrm->stream, buf, bl > 0 ? 2:1,
	                    QUIC_SEND_FLAG_NONE, aio))) {
		log_error("Failed in StreamSend, 0x%x!", Status);
		nni_aio_list_remove(aio);
		nni_mtx_unlock(&qstrm->mtx);
		free(buf);
		nni_msg_free(msg);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return NNG_ECANCELED;
	}

	nni_mtx_unlock(&qstrm->mtx);

	return 0;
}

static void
quic_pipe_send_start(quic_strm_t *qstrm)
{
	nni_aio    *aio;
	nni_msg    *msg;
	QUIC_STATUS rv;

	if ((aio = nni_list_first(&qstrm->sendq)) == NULL) {
		return;
	}

	if (qstrm->closed) {
		while ((aio = nni_list_first(&qstrm->sendq)) != NULL) {
			nni_list_remove(&qstrm->sendq, aio);
			msg = nni_aio_get_msg(aio);
			nni_msg_free(msg);
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		return;
	}

	// This runs to send the message.
	msg = nni_aio_get_msg(aio);

	QUIC_BUFFER *buf = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER)*2);
	int          hl  = nni_msg_header_len(msg);
	int          bl  = nni_msg_len(msg);

	if (hl > 0) {
		QUIC_BUFFER *buf1 = &buf[0];
		buf1->Length      = hl;
		buf1->Buffer      = nni_msg_header(msg);
	}

	if (bl > 0) {
		QUIC_BUFFER *buf2 = &buf[1];
		buf2->Length      = bl;
		buf2->Buffer      = nni_msg_body(msg);
	}

	log_debug("type is 0x%x %x.",
	    ((((uint8_t *) nni_msg_header(msg))[0] & 0xf0) >> 4),
	    ((uint8_t *) nni_msg_header(msg))[0]);
	log_debug("body len: %d header len: %d", buf[1].Length, buf[0].Length);
	nni_aio_set_input(aio, 0, buf);
	// send QoS 0 msg with NULL context

	if (QUIC_FAILED(rv = MsQuic->StreamSend(qstrm->stream, buf, bl > 0 ? 2:1,
	                    QUIC_SEND_FLAG_NONE, NULL))) {
		log_debug("Failed in StreamSend, 0x%x!", rv);
		free(buf);
	}
	// Directly finish AIO here instead of waiting send_complete cb?
}

static void
quic_pipe_recv_start(void *arg)
{
	qdebug("quic_strm_recv_start.\n");
	quic_strm_t *qstrm = arg;
	nni_aio *aio = NULL;

	if (qstrm->closed) {
		while ((aio = nni_list_first(&qstrm->recvq)) != NULL) {
			nni_list_remove(&qstrm->recvq, aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		return;
	}
	if (nni_list_empty(&qstrm->recvq)) {
		return;
	}
	if (qstrm->rrlen > qstrm->rwlen && qstrm->rrlen > 0 && qstrm->inrr == false) {
		qstrm->inrr = true;
		nni_aio_finish(&qstrm->rraio, 0, 0);
		return;
	}

	// Wait MsQuic take back data
	MsQuic->StreamReceiveSetEnabled(qstrm->stream, TRUE);
}

static void
quic_pipe_recv_cb(void *arg)
{
	quic_strm_t *qstrm = arg;
	nni_aio *aio = NULL;

	qdebug("before rxlen %d rwlen %d.\n", qstrm->rxlen, qstrm->rwlen);
	// qdebug("rrpos %d rrlen %d rrbuf %x %x.\n", qstrm->rrpos, qstrm->rrlen,
	// qstrm->rrbuf[qstrm->rrpos], qstrm->rrbuf[qstrm->rrpos + 1]);
	uint8_t  usedbytes;
	uint8_t *rbuf;
	uint32_t rlen, n, remain_len;
	if (nni_aio_result(&qstrm->rraio) != 0) {
		qdebug("QUIC aio receving error!");
		return;
	}
	nni_mtx_lock(&qstrm->mtx);
	rbuf = qstrm->rrbuf + qstrm->rrpos;
	rlen = qstrm->rrlen; // Wait MsQuic take back data
	if (rlen < qstrm->rwlen - qstrm->rxlen) {
		qdebug("Data is not enough and rrpos %d rrlen %d.\n", qstrm->rrpos, qstrm->rrlen);
		if (rlen > 0 && qstrm->rrpos > 0) {
			memmove(qstrm->rrbuf, qstrm->rrbuf+qstrm->rrpos, qstrm->rrlen);
			qstrm->rrpos = 0;
		}
		MsQuic->StreamReceiveSetEnabled(qstrm->stream, TRUE);
		nni_mtx_unlock(&qstrm->mtx);
		nni_aio_finish(&qstrm->rraio, 0, 0);
		return;
	}
	// We get enough data

	// Already get 2 Bytes
	if (qstrm->rxlen == 0) {
		n = 2; // new
		qdebug("msg type !!: %x\n", *rbuf);
		memcpy(qstrm->rxbuf, rbuf, n);
		qstrm->rxlen = 0 + n;
		qstrm->rrpos += n;
		qstrm->rrlen -= n;
		if (qstrm->rxbuf[1] == 0) {
			// 0 remaining length could be
			// PINGRESP/DISCONNECT
			if (0 != nng_msg_alloc(&qstrm->rxmsg, 0)) {
				qdebug("error in msg allocated.\n");
			}
			nni_msg_header_append(
			    qstrm->rxmsg, qstrm->rxbuf, 2);
			goto upload;
		}
		if (qstrm->rxbuf[1] == 2)
			qstrm->rwlen = n + 2; // Only this case exclude
		else
			qstrm->rwlen = n + 3;

		// Re-schedule
		if (!nni_list_empty(&qstrm->recvq)) {
			nni_aio_finish(&qstrm->rraio, 0, 0);
		}
		qdebug("1after  rxlen %d rwlen %d rrlen %d.\n", qstrm->rxlen, qstrm->rwlen, qstrm->rrlen);
		nni_mtx_unlock(&qstrm->mtx);
		return;
	}

	// Already get 4 Bytes
	if (qstrm->rxbuf[1] == 2 && qstrm->rwlen == 4) {
		// Handle 4 bytes msg
		n = 2; // new
		memcpy(qstrm->rxbuf + 2, rbuf, n);
		qstrm->rxlen += n;
		qstrm->rrpos += n;
		qstrm->rrlen -= n;
		qdebug("4bytes byte1 !!!!!!!: %x\n", *rbuf);

		// Compose msg
		if (0 != nng_msg_alloc(&qstrm->rxmsg, 4)) {
			qdebug("error in msg allocated.\n");
		}

		nni_msg_header_clear(qstrm->rxmsg);
		nni_msg_clear(qstrm->rxmsg);
		// Copy Header
		nni_msg_header_append(qstrm->rxmsg, qstrm->rxbuf, 2);
		// Copy Body
		nni_msg_append(qstrm->rxmsg, qstrm->rxbuf + 2, 2);

		// Done
		qdebug("2after  rxlen %d rwlen %d.\n", qstrm->rxlen, qstrm->rwlen);
	}

	// Already get 5 Bytes
	if (qstrm->rxbuf[1] > 0x02 && qstrm->rwlen == 5) {
		n = 3; // new
		memcpy(qstrm->rxbuf + 2, rbuf, n);
		qstrm->rxlen += n;
		qstrm->rrpos += n;
		qstrm->rrlen -= n;

		usedbytes = 0;
		if (0 != mqtt_get_remaining_length(qstrm->rxbuf, qstrm->rxlen, &remain_len, &usedbytes)) {
			qdebug("error in get remain_len.\n");
		}
		if (0 != nng_msg_alloc(&qstrm->rxmsg, 1 + usedbytes + remain_len)) {
			qdebug("error in msg allocated.\n");
		}
		qstrm->rwlen = remain_len + usedbytes + 1;

		if (qstrm->rxbuf[1] == 0x03) {
			nni_msg_header_clear(qstrm->rxmsg);
			nni_msg_clear(qstrm->rxmsg);
			// Copy Header
			nni_msg_header_append(qstrm->rxmsg, qstrm->rxbuf, 2);
			// Copy Body
			nni_msg_append(qstrm->rxmsg, qstrm->rxbuf + 2, 3);
		} else {
			// Wait to be re-schedule
			if (!nni_list_empty(&qstrm->recvq)) {
				nni_aio_finish(&qstrm->rraio, 0, 0);
			}
			qdebug("3after  rxlen %d rwlen %d.\n", qstrm->rxlen, qstrm->rwlen);
			nni_mtx_unlock(&qstrm->mtx);
			return;
		}
	}

	// Already get remain_len Bytes
	if (qstrm->rwlen > 0x05 && qstrm->rxmsg != NULL) {
		usedbytes = 0;
		if (0 != mqtt_get_remaining_length(qstrm->rxbuf, qstrm->rxlen, &remain_len, &usedbytes)) {
			qdebug("error in get remain_len.\n");
		}
		n = 1 + usedbytes + remain_len - 5; // new

		nni_msg_header_clear(qstrm->rxmsg);
		nni_msg_clear(qstrm->rxmsg);
		// Copy Header
		nni_msg_header_append(qstrm->rxmsg, qstrm->rxbuf, 1 + usedbytes);
		// Copy Body
		nni_msg_append(qstrm->rxmsg,
			qstrm->rxbuf + (1 + usedbytes), 5 - (1 + usedbytes));
		nni_msg_append(qstrm->rxmsg, rbuf, n);

		qstrm->rxlen += n;
		qstrm->rrpos += n;
		qstrm->rrlen -= n;
	}
	qdebug("4after  rxlen %d rwlen %d rrlen %d.\n", qstrm->rxlen, qstrm->rwlen, qstrm->rrlen);

	if (qstrm->rrlen > 0 && qstrm->rrpos > 0) {
		memmove(qstrm->rrbuf, qstrm->rrbuf+qstrm->rrpos, qstrm->rrlen);
		qstrm->rrpos = 0;
	}

upload:
	qstrm->inrr = false;

	// get aio and trigger cb of protocol layer
	aio = nni_list_first(&qstrm->recvq);

	if (aio != NULL) {
		nni_list_remove(&qstrm->recvq, aio);
		// Set msg and remove from list and finish
		nni_aio_set_msg(aio, qstrm->rxmsg);
		qstrm->rxmsg = NULL;
		qdebug("AIO FINISH\n");

		if (qstrm->rrlen > 0) {
			if (!nni_list_empty(&qstrm->recvq) && qstrm->inrr == false) {
				qdebug("inrr false\n");
				qstrm->rxlen = 0;
				qstrm->rwlen = 2;
				qstrm->inrr = true;
				nni_aio_finish(&qstrm->rraio, 0, 0);
			}
		}

		nni_mtx_unlock(&qstrm->mtx);
		nni_aio_finish_sync(aio, 0, 0);
	} else {
		if (nni_lmq_full(&qstrm->recv_messages)) {
			if (0 != nni_lmq_resize(&qstrm->recv_messages,
				2 * nni_lmq_cap(&qstrm->recv_messages))) {
				// memory error
				nni_msg_free(qstrm->rxmsg);
				nni_println("msg dropped due to no more memory!\n");
			}
		}
		nni_lmq_put(&qstrm->recv_messages, qstrm->rxmsg);
		qstrm->rxmsg = NULL;
		qstrm->inrr = false;
		nni_mtx_unlock(&qstrm->mtx);
	}

	/*
	if (qstrm->rrlen > 0)
		if (!nni_list_empty(&qstrm->recvq))
			nni_aio_finish(&qstrm->rraio, 0, 0);

	memmove(qstrm->rrbuf, qstrm->rrbuf+qstrm->rrpos, qstrm->rrlen);
	qstrm->rrpos = 0;
	*/

	qdebug("over\n");
}

static void
quic_pipe_send_cancel(nni_aio *aio, void *arg, int rv)
{
	quic_strm_t *qstrm = arg;

	nni_mtx_lock(&qstrm->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&qstrm->mtx);
		return;
	}
	if (nni_list_first(&qstrm->sendq) == aio) {
		nni_mtx_unlock(&qstrm->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&qstrm->mtx);

	nni_aio_finish_error(aio, rv);
}

static void
quic_pipe_recv_cancel(nni_aio *aio, void *arg, int rv)
{
	quic_strm_t *p = arg;

	nni_mtx_lock(&p->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&p->mtx);
		return;
	}
	if (nni_list_first(&p->recvq) == aio) {
		nni_aio_list_remove(aio);
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&p->mtx);
	nni_aio_finish_error(aio, rv);
}

int
quic_pipe_recv(void *qpipe, nni_aio *raio)
{
	int          rv;
	quic_strm_t *qstrm = qpipe;
	nng_msg     *msg;

	if (nni_aio_begin(raio) != 0) {
		return -1;
	}

	nni_mtx_lock(&qstrm->mtx);
	if ((rv = nni_aio_schedule(raio, quic_pipe_recv_cancel, qstrm)) !=
	    0) {
		nni_mtx_unlock(&qstrm->mtx);
		nni_aio_finish_error(raio, rv);
		return 0;
	}
	// Get msg from cache
	if (!nni_lmq_empty(&qstrm->recv_messages)) {
		nni_lmq_get(&qstrm->recv_messages, &msg);
		nni_aio_set_msg(raio, msg);
		nni_mtx_unlock(&qstrm->mtx);

		nni_aio_finish(raio, 0, 0);
		return 0;
	}

	nni_list_append(&qstrm->recvq, raio);
	if (nni_list_first(&qstrm->recvq) == raio) {
		//TODO set different init length for different packet.
		qstrm->rxlen = 0;
		qstrm->rwlen = 2; // Minimal RX length
		quic_pipe_recv_start(qstrm);
	}
	nni_mtx_unlock(&qstrm->mtx);
	return 0;
}

int
quic_pipe_send(void *qpipe, nni_aio *aio)
{
	quic_strm_t *qstrm = qpipe;
	int          rv;

	if ((rv = nni_aio_begin(aio)) != 0) {
		return rv;
	}

	if (qstrm->closed) {
		nni_msg *msg = nni_aio_get_msg(aio);
		nni_msg_free(msg);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return -1;
	}
	nni_mtx_lock(&qstrm->mtx);
	if ((rv = nni_aio_schedule(aio, quic_pipe_send_cancel, qstrm)) != 0) {
		nni_mtx_unlock(&qstrm->mtx);
		nni_aio_finish_error(aio, rv);
		return (-1);
	}
	nni_list_append(&qstrm->sendq, aio);
	if (nni_list_first(&qstrm->sendq) == aio) {
		quic_pipe_send_start(qstrm);
	}
	nni_mtx_unlock(&qstrm->mtx);

	return 0;
}

// create pipe & stream
int
quic_pipe_open(void *qsock, void **qpipe, void *mqtt_pipe)
{
	HQUIC strm = NULL;
	QUIC_STATUS  rv;
	quic_sock_t *qs = qsock;

	quic_strm_t *qstrm = nng_alloc(sizeof(quic_strm_t));
	if (qstrm == NULL)
		return -1;
	quic_strm_init(qstrm, qsock);

	// Allocate a new bidirectional stream.
	// The stream is just allocated and no QUIC stream identifier
	// is assigned until it's started.
	nni_mtx_lock(&qs->mtx);
	rv = MsQuic->StreamOpen(qs->qconn, QUIC_STREAM_OPEN_FLAG_NONE,
	        quic_strm_cb, (void *)qstrm, &strm);
	nni_mtx_unlock(&qs->mtx);
	if (QUIC_FAILED(rv)) {
		log_error("StreamOpen failed, 0x%x!\n", rv);
		goto error;
	}
	log_debug("[strm][%p] Starting...", strm);

	// Starts the bidirectional stream.
	// By default, the peer is not notified of the stream being started
	// until data is sent on the stream.
	if (QUIC_FAILED(rv = MsQuic->StreamStart(strm, QUIC_STREAM_START_FLAG_NONE))) {
		log_error("quic stream start failed, 0x%x!\n", rv);
		MsQuic->StreamClose(strm);
		goto error;
	}

	// Not ready for receiving
	MsQuic->StreamReceiveSetEnabled(qstrm->stream, FALSE);
	qstrm->closed = false;

	log_debug("[strm][%p] Done...\n", strm);

	qstrm->stream = strm;
	qstrm->pipe   = mqtt_pipe;

	*qpipe = qstrm;
	return 0;

error:
	nng_free(qstrm, sizeof(quic_strm_t));

	return (-2);
}

// return value 0 : normal close -1 : not even started
int
quic_pipe_close(void *qpipe, uint8_t *code)
{
	if (!qpipe)
		return -1;
	quic_strm_t *qstrm = qpipe;
	nni_aio     *aio;

	if (qstrm->closed != true) {
		qstrm->closed = true;
		log_warn("closing the QUIC stream!");
		MsQuic->StreamClose(qstrm->stream);
	} else {
		return -1;
	}

	log_info("Protocol layer is closing QUIC pipe!");
	// take care of aios
	while ((aio = nni_list_first(&qstrm->sendq)) != NULL) {
		nni_list_remove(&qstrm->sendq, aio);
		nni_msg *msg = nni_aio_get_msg(aio);
		nni_msg_free(msg);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	while ((aio = nni_list_first(&qstrm->recvq)) != NULL) {
		nni_list_remove(&qstrm->recvq, aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	quic_strm_fini(qstrm);
	nng_free(qstrm, sizeof(quic_strm_t));

	return 0;
}

// only call this when main stream is closed!
uint8_t
quic_sock_disconnect_code(void *arg)
{
       quic_sock_t *qsock = arg;
       return qsock->reason_code;
}

void
quic_open()
{
	QUIC_STATUS rv = QUIC_STATUS_SUCCESS;
	// only Open MsQUIC lib once, otherwise cause memleak
	if (MsQuic == NULL)
		if (QUIC_FAILED(rv = MsQuicOpen2(&MsQuic))) {
			log_error("MsQuicOpen2 failed, 0x%x!\n", rv);
			goto error;
		}

	// Create a registration for the app's connections.
	if (QUIC_FAILED(rv = MsQuic->RegistrationOpen(
	                    &quic_reg_config, &registration))) {
		log_error("RegistrationOpen failed, 0x%x!\n", rv);
		goto error;
	}

	log_info("Msquic is enabled");
	return;

error:
	quic_close();
}

void
quic_close()
{
	if (MsQuic != NULL) {
		if (configuration != NULL) {
			MsQuic->ConfigurationClose(configuration);
		}
		if (registration != NULL) {
			// This will block until all outstanding child objects
			// have been closed.
			MsQuic->RegistrationClose(registration);
		}
		MsQuicClose(MsQuic);
	}
}

void
quic_proto_open(nni_proto *proto)
{
	g_quic_proto = proto;
}

void
quic_proto_close()
{
	g_quic_proto = NULL;
}

void
quic_proto_set_sdk_config(void *config)
{
	memcpy(&conf_node, config, sizeof(conf_quic));
}
