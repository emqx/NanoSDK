//
// Copyright 2023 NanoMQ Team, Inc. <wangwei@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// #if defined(NNG_ENABLE_QUIC) // && defined(NNG_QUIC_MSQUIC)
//
// Note.
// Quic connection is only visible in nng stream.
// Each nng stream is linked to a quic stream.
// nng dialer is linked to quic connection.
// The quic connection would be established when the first
// nng stream with same URL is created.
// The quic connection would be free if all nng streams
// closed.

#include "msquic_posix.h"
#include "quic_api.h"
#include "core/nng_impl.h"
#include "msquic.h"

#include "nng/mqtt/mqtt_client.h"
#include "nng/supplemental/nanolib/conf.h"
#include "nng/protocol/mqtt/mqtt_parser.h"
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

#include "nng/mqtt/mqtt_client.h"
#include "nng/supplemental/nanolib/conf.h"
#include "nng/protocol/mqtt/mqtt_parser.h"
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

struct nni_quic_conn {
	nng_stream      stream;
	nni_list        readq;
	nni_list        writeq;
	bool            closed;
	nni_mtx         mtx;
	nni_aio *       dial_aio;
	// nni_aio *       qstrmaio; // Link to msquic_strm_cb
	nni_quic_dialer *dialer;

	// MsQuic
	HQUIC           qstrm; // quic stream
	uint8_t         reason_code;

	nni_reap_node   reap;
};

static const QUIC_API_TABLE *MsQuic = NULL;

// Config for msquic
static const QUIC_REGISTRATION_CONFIG quic_reg_config = {
	"mqtt_listener",
	QUIC_EXECUTION_PROFILE_LOW_LATENCY
};

static const QUIC_BUFFER quic_alpn = {
	sizeof("mqtt") - 1,
	(uint8_t *) "mqtt"
};

HQUIC registration;
HQUIC configuration

static void msquic_listener_fini(HQUIC ql);
static void msquic_listener_stop(HQUIC ql);
static int  msquic_listen(HQUIC ql, const char *h, const char *p, nni_quic_listener *l);

/***************************** MsQuic Listener ******************************/

int
nni_quic_listener_init(void **argp)
{
	nni_quic_listener *l;

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}

	nni_mtx_init(&l->mtx);

	l->closed  = false;
	l->started = false;

	l->ql      = NULL;

	// nni_aio_alloc(&l->qconaio, quic_listener_cb, (void *)l);
	nni_aio_list_init(&l->acceptq);
	nni_aio_list_init(&l->incomings);
	nni_atomic_init_bool(&l->fini);
	nni_atomic_init64(&l->ref);
	nni_atomic_inc64(&l->ref);

	// 0RTT is disabled by default
	l->enable_0rtt = false;
	// multi_stream is disabled by default
	l->enable_mltstrm = false;

	memset(&l->settings, 0, sizeof(QUIC_SETTINGS));

	*argp = l;
	return 0;
}

// All the instructments here should be done within guard of lock of listener.
// And also could be done repeatly.
static void
quic_listener_doclose(nni_quic_listener *l)
{
	nni_aio *aio;

	l->closed = true;

	while ((aio = nni_list_first(&l->acceptq)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	while ((aio = nni_list_first(&l->incomings)) != NULL) {
		qconn = nni_aio_get_prov_data(aio);
		nni_aio_list_remove(aio);
		nni_aio_free(aio);
		msquic_conn_fini(qconn);
	}
	if (l->ql != NULL) {
		msquic_listener_stop(l->ql);
	}
}

void
nni_quic_listener_close(nni_quic_listener *l)
{
	nni_aio *aio;
	HQUIC qconn;

	nni_mtx_lock(&l->mtx);
	quic_listener_doclose(l);
	nni_mtx_unlock(&l->mtx);
}


int
nni_quic_listener_listen(nni_quic_listener *l, const char *h, const char *p)
{
	socklen_t               len;
	int                     rv;
	int                     fd;
	nni_posix_pfd *         pfd;

	nni_mtx_lock(&l->mtx);
	if (l->started) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ESTATE);
	}
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ECLOSED);
	}

	msquic_listen(l->ql, h, p, l);

	l->started = true;
	nni_mtx_unlock(&l->mtx);

	return (0);
}

static void
quic_listener_cancel(nni_aio *aio, void *arg, int rv)
{
	nni_quic_listener *l = arg;

	// This is dead easy, because we'll ignore the completion if there
	// isn't anything to do the accept on!
	NNI_ASSERT(rv != 0);
	nni_mtx_lock(&l->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&l->mtx);
}

static void
quic_listener_doaccept(nni_quic_listener *l)
{
	nni_aio *aio;

	while ((aio = nni_list_first(&l->acceptq)) != NULL) {
		int            newfd;
		int            fd;
		int            rv;
		int            nd;
		int            ka;
		HQUIC           qconn;
		nni_aio *       aioc;
		nni_quic_conn * c;

		// Get the connection 
		if ((aioc == nni_list_first(&l->incomings)) == NULL) {
			// No wait and return immediately
			return;
		}
		qconn = nni_aio_get_prov_data(aioc); // Must exists
		nni_aio_list_remove(aioc);
		nni_aio_free(aioc);

		// Create a nni quic connection
		if ((rv = nni_msquic_quic_alloc(&c, NULL)) != 0) {
			msquic_conn_fini(qconn);
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, rv);
			continue;
		}

		nni_aio_list_remove(aio);
		nni_aio_set_output(aio, 0, c);
		nni_aio_finish(aio, 0, 0);
	}
}


void
nni_quic_listener_accept(nni_quic_listener *l, nni_aio *aio)
{
	int rv;

	if (nni_aio_begin(aio)) {
		return;
	}
	nni_mtx_lock(&l->mtx);

	if (!l->started) {
		nni_mtx_unlock(&l->mtx);
		nni_aio_finish_error(aio, NNG_ESTATE);
		return;
	}

	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	if ((rv = nni_aio_schedule(aio, quic_listener_cancel, l)) != 0) {
		nni_mtx_unlock(&l->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_aio_list_append(&l->acceptq, aio);
	if (nni_list_first(&l->acceptq) == aio) {
		quic_listener_doaccept(l);
	}

	nni_mtx_unlock(&l->mtx);
}

void
nni_quic_listener_fini(nni_quic_listener *l)
{
	HQUIC ql;

	nni_mtx_lock(&l->mtx);
	quic_listener_doclose(l);
	ql = l->ql;
	nni_mtx_unlock(&l->mtx);

	if (ql != NULL) {
		msquic_listener_fini(ql);
	}
	nni_mtx_fini(&l->mtx);
	NNI_FREE_STRUCT(l);
}


/***************************** MsQuic Bindings *****************************/

static void
msquic_load_listener_config()
{
	return;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK) QUIC_STATUS QUIC_API
msquic_connection_cb(_In_ HQUIC Connection, _In_opt_ void *Context,
	_Inout_ QUIC_CONNECTION_EVENT *ev)
{
	nni_quic_listener *l     = Context;
	HQUIC              qconn = Connection;

	log_debug("msquic_connection_cb triggered! %d", ev->Type);
	switch (ev->Type) {
	case QUIC_CONNECTION_EVENT_CONNECTED:
		// The handshake has completed for the connection.
		// do not init any var here due to potential frequent reconnect
		log_info("[conn][%p] is Connected. Resumed Session %d", qconn,
		    ev->CONNECTED.SessionResumed);

		if (l->enable_0rtt) {
			MsQuic->ConnectionSendResumptionTicket(qconn, QUIC_SEND_RESUMPTION_FLAG_NONE, 0, NULL);
		}

		nni_aio_finish(d->qconaio, 0, 0);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
		log_warn("[conn][%p] Shutdown by transport, 0x%x, Error Code %llu\n",
		    qconn, ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
		    (unsigned long long)
		        ev->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
		// The connection was explicitly shut down by the peer.
		log_warn("[conn][%p] "
		         "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER, "
		         "0x%llu\n",
		    qconn,
		    (unsigned long long)ev->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
		log_info("[conn][%p] QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: All done\n\n", qconn);
		break;
	case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
		log_warn("[conn][%p] Resumption ticket received (%u bytes):\n",
		    Connection, ev->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
		break;
	case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
		log_info("QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED");
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
		log_warn("Unknown event type %d!", ev->Type);
		break;
	}
	return QUIC_STATUS_SUCCESS;
}


_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(QUIC_LISTENER_CALLBACK) QUIC_STATUS QUIC_API
msquic_listener_cb(_In_ HQUIC ql, _In_opt_ void *arg, _Inout_ QUIC_LISTENER_EVENT *ev)
{
	HQUIC *qconn;
	QUIC_NEW_CONNECTION_INFO *qinfo;
	QUIC_STATUS rv = QUIC_STATUS_NOT_SUPPORTED;
	nni_quic_listener *l = arg;
	nni_aio *aio;

	switch (ev->Type) {
	case QUIC_LISTENER_EVENT_NEW_CONNECTION:
		qconn = ev->NEW_CONNECTION.Connection;
		qinfo = ev->NEW_CONNECTION.Info;

		MsQuic->SetCallbackHandler(qconn, msquic_connection_cb, ql);
		rv = MsQuic->ConnectionSetConfiguration(qconn, configuration);

		nni_mtx_lock(&l->mtx);

		// Push connection to incomings
		nni_aio_alloc(&aio, NULL, NULL);
		nni_aio_set_prov_data(aio, (void *)qconn);
		nni_aio_list_append(&l->incomings, aio);

		quic_listener_doaccept(l);
		nni_mtx_unlock(&l->mtx);
		break;
	case QUIC_LISTENER_EVENT_STOP_COMPLETE:
		break;
	default:
		break;
	}

	return rv;
}

static int
msquic_listen(HQUIC ql, const char *h, const char *p, nni_quic_listener *l)
{
	HQUIC addr;
	QUIC_STATUS rv = 0;

	QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
	QuicAddrSetPort(&addr, atoi(p));

	msquic_load_listener_config();

	if (QUIC_FAILED(rv = MsQuic->ListenerOpen(registration, msquic_listener_cb, (void *)l, &ql))) {
		log_error("error in listen open %ld", rv);
		goto error;
	}

	if (QUIC_FAILED(rv = MsQuic->ListenerStart(ql, alpn, 1, &addr))) {
		log_error("error in listen start %ld", rv);
		goto error;
	}

	return rv;

error:
	if (ql != NULL) {
		msquic_listener_fini(ql);
	}
	return rv;
}

static void
msquic_listener_stop(HQUIC ql)
{
	MsQuic->ListenerStop(ql);
}

static void
msquic_listener_fini(HQUIC ql)
{
	MsQuic->ListenerClose(ql);
}

