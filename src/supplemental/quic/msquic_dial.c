//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io>
// SPDX-License-Identifier: MIT
//
// MsQuic implementation of the nng_stream dialer interface. A connection owns
// one MQTT control stream and, optionally, four client data streams.
#include "quic_api.h"
#include <stdlib.h>
#include <string.h>

typedef struct quic_channel quic_channel;
typedef struct quic_send quic_send;
struct quic_send {
	nni_list_node node;
	quic_channel *channel;
	nni_aio *aio;
	QUIC_BUFFER buffer;
};
struct quic_channel {
	nni_quic_conn *conn;
	HQUIC handle;
	nni_list readers;
	nni_list sends;
	bool eof;
    bool start_failed;
};
struct nni_quic_conn {
	nng_stream stream;
	nni_quic_dialer *dialer;
	nni_mtx mtx;
	nni_atomic_int refs;
	nni_reap_node reap;
    nni_atomic_flag reaping;
	HQUIC handle;
	HQUIC config;
	nni_aio *aio;
	bool closed;
	bool connected;
	unsigned channels;
	int priority;
	int error;
    nni_aio reopen_aio;
    bool reopen_initialized;
    bool retry_pending;
	quic_channel channel[1 + QUIC_SUB_STREAM_NUM];
};

static void conn_release(nni_quic_conn *);
static void conn_close(void *);
static nni_reap_list conn_reap;
static QUIC_STATUS QUIC_API connection_cb(HQUIC, void *, QUIC_CONNECTION_EVENT *);
static QUIC_STATUS QUIC_API stream_cb(HQUIC, void *, QUIC_STREAM_EVENT *);

static void
dialer_destroy(nni_quic_dialer *d)
{
	if (d->registration)
		d->api->RegistrationClose(d->registration);
	if (d->api)
		MsQuicClose(d->api);
	nni_strfree(d->ca);
	nni_strfree(d->cacert);
	nni_strfree(d->key);
	nni_strfree(d->password);
	nni_mtx_fini(&d->mtx);
	NNI_FREE_STRUCT(d);
}

void
nni_msquic_quic_dialer_rele(nni_quic_dialer *d)
{
	if (nni_atomic_dec64_nv(&d->ref) == 0)
		dialer_destroy(d);
}

static void
conn_destroy(void *arg)
{
	nni_quic_conn *c = arg;
	nni_quic_dialer *d = c->dialer;
    nni_mtx_lock(&c->mtx);
    if (nni_atomic_get(&c->refs) != 0) {
        // Stream ownership was released, but MsQuic may still be completing
        // TLS/stream shutdown. Keep global reap_drain waiting for that callback.
        nni_reap(&conn_reap, c);
        nni_mtx_unlock(&c->mtx);
        return;
    }
    nni_mtx_unlock(&c->mtx);
    if (c->reopen_initialized) nni_aio_stop(&c->reopen_aio);
	/* Reaped off the MsQuic callback thread. Close waits for callbacks. */
	if (c->handle)
		d->api->ConnectionClose(c->handle);
	if (c->config)
		d->api->ConfigurationClose(c->config);
    if (c->reopen_initialized) nni_aio_fini(&c->reopen_aio);
	nni_mtx_fini(&c->mtx);
	NNI_FREE_STRUCT(c);
	nni_msquic_quic_dialer_rele(d);
}
static nni_reap_list conn_reap = {
	.rl_offset = offsetof(nni_quic_conn, reap),
	.rl_func = conn_destroy,
};
static void
conn_queue_reap(nni_quic_conn *c)
{
    if (!nni_atomic_flag_test_and_set(&c->reaping))
        nni_reap(&conn_reap, c);
}
static void
conn_release(nni_quic_conn *c)
{
    // Serialize the zero transition with the reaper. The reaper can only free
    // c after taking this mutex, so queue insertion cannot race destruction.
    nni_mtx_lock(&c->mtx);
    if (nni_atomic_dec_nv(&c->refs) == 0) conn_queue_reap(c);
    nni_mtx_unlock(&c->mtx);
}

// Called with c->mtx held; callbacks for the new stream run later on the
// connection worker. The channel context survives replacement of its handle.
static int
channel_open(quic_channel *ch)
{
    nni_quic_conn *c = ch->conn;
    const QUIC_API_TABLE *api = c->dialer->api;
    QUIC_STATUS status = api->StreamOpen(c->handle,
        QUIC_STREAM_OPEN_FLAG_NONE, stream_cb, ch, &ch->handle);
    if (QUIC_FAILED(status)) return NNG_ECONNREFUSED;
    api->StreamReceiveSetEnabled(ch->handle, FALSE);
    if (c->priority >= 0) {
        uint16_t priority = (uint16_t)c->priority;
        api->SetParam(ch->handle, QUIC_PARAM_STREAM_PRIORITY, sizeof(priority), &priority);
    }
    status = api->StreamStart(ch->handle, QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL);
    if (QUIC_FAILED(status)) {
        api->StreamClose(ch->handle);
        ch->handle = NULL;
        return NNG_ECONNREFUSED;
    }
    ch->eof = false;
    ch->start_failed = false;
    if (!nni_list_empty(&ch->readers)) api->StreamReceiveSetEnabled(ch->handle, TRUE);
    return 0;
}

// Upstream retries substream creation every five seconds independently of
// the control stream. Keep this AIO alive until connection reaping stops it.
static void
channel_retry(void *arg)
{
    nni_quic_conn *c = arg;
    bool retry = false;
    nni_mtx_lock(&c->mtx);
    c->retry_pending = false;
    if (!c->closed && nni_aio_result(&c->reopen_aio) == 0) {
        for (unsigned i = 1; i < c->channels; ++i) {
            quic_channel *ch = &c->channel[i];
            if (ch->handle == NULL && channel_open(ch) != 0) retry = true;
        }
        if (retry) {
            c->retry_pending = true;
            nni_sleep_aio(QUIC_SUB_STREAM_TIMEOUT, &c->reopen_aio);
        }
    }
    nni_mtx_unlock(&c->mtx);
}
static void
channel_retry_schedule(nni_quic_conn *c)
{
    // nni_aio_busy includes callback execution, not just a pending timer.
    // A stream-start failure can race the tail of channel_retry's callback.
    if (c->reopen_initialized && !c->retry_pending && !c->closed) {
        c->retry_pending = true;
        nni_sleep_aio(QUIC_SUB_STREAM_TIMEOUT, &c->reopen_aio);
    }
}

static void
channel_error(quic_channel *ch, int rv)
{
	nni_aio *aio;
	quic_send *send;
	while ((aio = nni_list_first(&ch->readers)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	NNI_LIST_FOREACH (&ch->sends, send) {
		if ((aio = send->aio) != NULL) {
			send->aio = NULL;
			nni_aio_finish_error(aio, rv);
		}
	}
}
static void
conn_close(void *arg)
{
	nni_quic_conn *c = arg;
	nni_mtx_lock(&c->mtx);
    bool shutdown = !c->closed;
    c->closed = true;
    if (c->reopen_initialized) nni_aio_close(&c->reopen_aio);
    for (unsigned i = 0; i < c->channels; ++i)
        channel_error(&c->channel[i], NNG_ECLOSED);
    if (shutdown && c->handle) {
        c->dialer->api->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
	nni_mtx_unlock(&c->mtx);
}
static void
conn_free(void *arg)
{
	conn_close(arg);
    conn_queue_reap(arg);
	conn_release(arg);
}
static int
stream_get(void *arg, const char *name, void *buf, size_t *sz, nni_type t)
{
	nni_quic_conn *c = arg;
	if (strcmp(name, NNG_OPT_QUIC_ENABLE_MULTISTREAM) == 0)
		return nni_copyout_bool(c->channels > 1, buf, sz, t);
	return NNG_ENOTSUP;
}
static int
stream_set(void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	NNI_ARG_UNUSED(arg); NNI_ARG_UNUSED(name); NNI_ARG_UNUSED(buf);
	NNI_ARG_UNUSED(sz); NNI_ARG_UNUSED(t);
	return NNG_ENOTSUP;
}
static quic_channel *
get_channel(nni_quic_conn *c, nni_aio *aio)
{
	int *flags = nni_aio_get_prov_data(aio);
	unsigned id = flags ? (*flags & QUIC_MULTISTREAM_FLAGS) : 0;
	return id < c->channels ? &c->channel[id] : NULL;
}
static void
recv_cancel(nni_aio *aio, void *arg, int rv)
{
	quic_channel *ch = arg;
	nni_mtx_lock(&ch->conn->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ch->conn->mtx);
}
static void
stream_recv(void *arg, nni_aio *aio)
{
	nni_quic_conn *c = arg;
	quic_channel *ch;
	int rv;
	if (nni_aio_begin(aio) != 0) return;
	nni_mtx_lock(&c->mtx);
	if ((ch = get_channel(c, aio)) == NULL) rv = NNG_EINVAL;
	else if (c->closed || (ch == &c->channel[0] && ch->eof)) rv = NNG_ECLOSED;
	else if (nni_aio_iov_count(aio) == 0) rv = NNG_EINVAL;
	else if ((rv = nni_aio_schedule(aio, recv_cancel, ch)) == 0) {
		nni_aio_list_append(&ch->readers, aio);
		if (ch->handle && !ch->eof) c->dialer->api->StreamReceiveSetEnabled(ch->handle, TRUE);
		nni_mtx_unlock(&c->mtx);
		return;
	}
	nni_mtx_unlock(&c->mtx);
	nni_aio_finish_error(aio, rv);
}
static void
send_cancel(nni_aio *aio, void *arg, int rv)
{
	quic_channel *ch = arg;
	quic_send *send;
	nni_mtx_lock(&ch->conn->mtx);
	NNI_LIST_FOREACH (&ch->sends, send) {
		if (send->aio == aio) {
			/* MsQuic still owns the copied bytes until SEND_COMPLETE. */
			send->aio = NULL;
			nni_aio_finish_error(aio, rv);
			break;
		}
	}
	nni_mtx_unlock(&ch->conn->mtx);
}
static void
stream_send(void *arg, nni_aio *aio)
{
	nni_quic_conn *c = arg;
	quic_channel *ch;
	quic_send *send = NULL;
	nni_iov *iov;
	unsigned niov;
	size_t len = 0, offset = 0;
	int rv = 0;
	if (nni_aio_begin(aio) != 0) return;
	nni_mtx_lock(&c->mtx);
	if ((ch = get_channel(c, aio)) == NULL) { rv = NNG_EINVAL; goto error; }
    if (c->closed) { rv = NNG_ECLOSED; goto error; }
    if (ch->handle == NULL || ch->eof) { rv = NNG_ECANCELED; goto error; }
    int *flags = nni_aio_get_prov_data(aio);
    if (flags && (*flags & QUIC_CLOSE_REOPEN_FLAGS)) {
        if (ch == &c->channel[0]) { rv = NNG_EINVAL; goto error; }
        ch->eof = true;
        c->dialer->api->StreamShutdown(ch->handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        rv = NNG_ECANCELED;
        goto error;
    }
	nni_aio_get_iov(aio, &niov, &iov);
	for (unsigned i = 0; i < niov; ++i) {
		if (iov[i].iov_len > UINT32_MAX - len) { rv = NNG_EMSGSIZE; goto error; }
		len += iov[i].iov_len;
	}
	if (len == 0) { rv = NNG_EINVAL; goto error; }
	if ((send = NNI_ALLOC_STRUCT(send)) == NULL) { rv = NNG_ENOMEM; goto error; }
	if ((send->buffer.Buffer = nni_alloc(len)) == NULL) { rv = NNG_ENOMEM; goto error; }
	send->buffer.Length = (uint32_t) len;
	for (unsigned i = 0; i < niov; ++i) {
        if (iov[i].iov_len != 0)
            memcpy(send->buffer.Buffer + offset, iov[i].iov_buf, iov[i].iov_len);
		offset += iov[i].iov_len;
	}
	send->aio = aio;
	send->channel = ch;
	if ((rv = nni_aio_schedule(aio, send_cancel, ch)) != 0) goto error;
	nni_list_append(&ch->sends, send);
	QUIC_STATUS status = c->dialer->api->StreamSend(
	    ch->handle, &send->buffer, 1,
        flags && (*flags & QUIC_HIGH_PRIOR_MSG) ? QUIC_SEND_FLAG_PRIORITY_WORK : QUIC_SEND_FLAG_NONE, send);
	if (QUIC_FAILED(status)) {
		nni_list_remove(&ch->sends, send);
		rv = NNG_ECONNSHUT;
		goto error;
	}
	nni_mtx_unlock(&c->mtx);
	return;
error:
	nni_mtx_unlock(&c->mtx);
	if (send) {
		nni_free(send->buffer.Buffer, len);
		NNI_FREE_STRUCT(send);
	}
	nni_aio_finish_error(aio, rv);
}

static QUIC_STATUS QUIC_API
stream_cb(HQUIC handle, void *arg, QUIC_STREAM_EVENT *event)
{
	quic_channel *ch = arg;
	nni_quic_conn *c = ch->conn;
	nni_quic_dialer *d = c->dialer;
	nni_aio *aio;
	nni_mtx_lock(&c->mtx);
	switch (event->Type) {
	case QUIC_STREAM_EVENT_SEND_COMPLETE: {
		quic_send *send = event->SEND_COMPLETE.ClientContext;
		nni_list_remove(&ch->sends, send);
		if ((aio = send->aio) != NULL) {
			if (event->SEND_COMPLETE.Canceled)
				nni_aio_finish_error(aio, NNG_ECANCELED);
			else
				nni_aio_finish(aio, 0, send->buffer.Length);
		}
		nni_free(send->buffer.Buffer, send->buffer.Length);
		NNI_FREE_STRUCT(send);
		break;
	}
	case QUIC_STREAM_EVENT_RECEIVE: {
		uint64_t consumed = 0;
		uint32_t bi = 0, bo = 0;
		while (!c->closed && bi < event->RECEIVE.BufferCount &&
		    (aio = nni_list_first(&ch->readers)) != NULL) {
			nni_iov *iov;
			unsigned niov;
			size_t copied = 0;
			nni_aio_get_iov(aio, &niov, &iov);
			for (unsigned i = 0; i < niov; ++i) {
				size_t off = 0;
				while (off < iov[i].iov_len && bi < event->RECEIVE.BufferCount) {
					const QUIC_BUFFER *b = &event->RECEIVE.Buffers[bi];
					size_t n = b->Length - bo;
					if (n > iov[i].iov_len - off) n = iov[i].iov_len - off;
					memcpy((uint8_t *)iov[i].iov_buf + off, b->Buffer + bo, n);
					off += n; copied += n; bo += (uint32_t)n;
					if (bo == b->Length) { ++bi; bo = 0; }
				}
			}
			if (copied == 0) break;
			consumed += copied;
			nni_aio_list_remove(aio);
			nni_aio_finish(aio, 0, copied);
		}
		/* Synchronous consumption of ALL buffers, including data with FIN.
		 * MsQuic retains unconsumed bytes and pauses until recv enables it. */
		event->RECEIVE.TotalBufferLength = consumed;
		break;
	}
	case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        if (ch != &c->channel[0] && !c->closed) {
            ch->eof = true;
            d->api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            break;
        }
		ch->eof = true;
		while ((aio = nni_list_first(&ch->readers)) != NULL) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		break;
	case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
	case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
		ch->eof = true;
        d->api->StreamShutdown(handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
		break;
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (QUIC_FAILED(event->START_COMPLETE.Status)) {
            ch->start_failed = true;
            if (ch == &c->channel[0]) {
                c->error = NNG_ECONNREFUSED;
                d->api->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            }
        } else if (ch == &c->channel[0] && c->aio != NULL && !c->closed && !c->error) {
            c->connected = true;
            // currcon tracks only an in-progress dial. Delivered streams own
            // a dialer reference and may outlive/free their stream dialer.
            nni_mtx_lock(&d->mtx);
            if (d->currcon == c) d->currcon = NULL;
            nni_mtx_unlock(&d->mtx);
            nni_atomic_inc(&c->refs);
            aio = c->aio; c->aio = NULL;
            nni_aio_set_output(aio, 0, &c->stream);
            nni_aio_finish(aio, 0, 0);
        }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        bool reopen = ch != &c->channel[0] && (c->connected || c->aio != NULL) && !c->closed;
        // Pending receives must discard partial MQTT frames from the old
        // stream before reading the replacement stream.
        channel_error(ch, reopen ? NNG_ECANCELED : NNG_ECLOSED);
        ch->eof = true;
        ch->handle = NULL;
        d->api->StreamClose(handle);
        if (reopen) {
            if (ch->start_failed || channel_open(ch) != 0) channel_retry_schedule(c);
        }
        break;
    }
	default: break;
	}
	nni_mtx_unlock(&c->mtx);
	return QUIC_STATUS_SUCCESS;
}
static void
dial_cancel(nni_aio *aio, void *arg, int rv)
{
	nni_quic_conn *c = arg;
	nni_mtx_lock(&c->mtx);
	if (c->aio == aio) {
		c->aio = NULL;
		c->error = rv;
        c->closed = true;
        if (c->reopen_initialized) nni_aio_close(&c->reopen_aio);
        nni_mtx_lock(&c->dialer->mtx);
        if (c->dialer->currcon == c) c->dialer->currcon = NULL;
        nni_mtx_unlock(&c->dialer->mtx);
        conn_queue_reap(c);
		nni_aio_finish_error(aio, rv);
		c->dialer->api->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
	}
	nni_mtx_unlock(&c->mtx);
}
static QUIC_STATUS QUIC_API
connection_cb(HQUIC handle, void *arg, QUIC_CONNECTION_EVENT *event)
{
	nni_quic_conn *c = arg;
	nni_quic_dialer *d = c->dialer;
	nni_aio *aio;
	NNI_ARG_UNUSED(handle);
	nni_mtx_lock(&c->mtx);
	switch (event->Type) {
	case QUIC_CONNECTION_EVENT_CONNECTED: {
		if (c->aio == NULL || c->closed) {
			d->api->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
			break;
		}
        for (unsigned i = 0; i < c->channels; ++i) {
            int err = channel_open(&c->channel[i]);
            if (err && i == 0) { c->error = err; break; }
            if (err) channel_retry_schedule(c);
        }
        if (c->error != 0) {
            d->api->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
        // As upstream, dial completes at control-stream START_COMPLETE,
        // not merely at completion of the QUIC TLS handshake.
		break;
	}
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        c->closed = true;
		c->error = event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status ==
		    QUIC_STATUS_CONNECTION_TIMEOUT ? NNG_ETIMEDOUT : NNG_ECONNREFUSED;
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        c->closed = true;
		c->error = NNG_ECONNSHUT;
		break;
	case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
		nni_mtx_lock(&d->mtx);
		if (d->enable_0rtt && event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength <= sizeof(d->rticket)) {
			d->rticket_sz = (uint16_t)event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength;
			memcpy(d->rticket, event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket, d->rticket_sz);
		}
		nni_mtx_unlock(&d->mtx);
		break;
	case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        c->closed = true;
        // Detach before completing the AIO: its callback may redial at once.
        nni_mtx_lock(&d->mtx);
        if (d->currcon == c) d->currcon = NULL;
        nni_mtx_unlock(&d->mtx);
        for (unsigned i = 0; i < c->channels; ++i)
            channel_error(&c->channel[i], NNG_ECLOSED);
        if ((aio = c->aio) != NULL) {
            c->aio = NULL;
            nni_aio_finish_error(aio, c->error ? c->error : NNG_ECLOSED);
        }
        nni_mtx_unlock(&c->mtx);
		conn_release(c); /* MsQuic's callback ownership */
		return QUIC_STATUS_SUCCESS;
	default: break;
	}
	nni_mtx_unlock(&c->mtx);
	return QUIC_STATUS_SUCCESS;
}

int
nni_quic_dialer_init(void **arg)
{
	nni_quic_dialer *d = NNI_ALLOC_STRUCT(d);
	QUIC_REGISTRATION_CONFIG registration = { "NanoSDK", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
	if (!d) return NNG_ENOMEM;
	nni_mtx_init(&d->mtx);
	nni_atomic_init64(&d->ref);
	nni_atomic_set64(&d->ref, 1);
	if (QUIC_FAILED(MsQuicOpen2(&d->api)) ||
	    QUIC_FAILED(d->api->RegistrationOpen(&registration, &d->registration))) {
		dialer_destroy(d);
		return NNG_EINTERNAL;
	}
	d->priority = -1;
	d->enable_mltstrm = false;
	d->qidle_timeout = QUIC_IDLE_TIMEOUT_DEFAULT;
	d->qkeepalive = QUIC_KEEPALIVE_DEFAULT;
	d->settings.IsSet.IdleTimeoutMs = TRUE;
	d->settings.IdleTimeoutMs = d->qidle_timeout * 1000;
	d->settings.IsSet.KeepAliveIntervalMs = TRUE;
	d->settings.KeepAliveIntervalMs = d->qkeepalive * 1000;
	*arg = d;
	return 0;
}
void
nni_quic_dialer_close(void *arg)
{
	nni_quic_dialer *d = arg;
	nni_quic_conn *c;
	nni_mtx_lock(&d->mtx);
	d->closed = true;
	c = d->currcon;
	if (c) nni_atomic_inc(&c->refs);
	nni_mtx_unlock(&d->mtx);
	if (c) {
		conn_close(c);
		conn_release(c);
	}
}
void
nni_quic_dialer_fini(nni_quic_dialer *d)
{
	nni_quic_dialer_close(d);
	nni_msquic_quic_dialer_rele(d);
}

void
nni_quic_dial(void *arg, const char *host, const char *port, nni_aio *aio)
{
	nni_quic_dialer *d = arg;
	nni_quic_conn *c = NULL;
	QUIC_STATUS status;
	QUIC_BUFFER alpn = { 4, (uint8_t *)"mqtt" };
	QUIC_CREDENTIAL_CONFIG cred = { 0 };
	QUIC_CERTIFICATE_FILE cert = { 0 };
	QUIC_CERTIFICATE_FILE_PROTECTED protected_cert = { 0 };
	char *end;
	unsigned long p = strtoul(port, &end, 10);
	int rv;
	if (nni_aio_begin(aio) != 0) return;
	if (!*port || *end || p == 0 || p > 65535 || !*host) {
		nni_aio_finish_error(aio, NNG_EADDRINVAL); return;
	}
	nni_mtx_lock(&d->mtx);
	if (d->closed) { rv = NNG_ECLOSED; goto error; }
	if (d->currcon != NULL) { rv = NNG_EBUSY; goto error; }
	if ((c = NNI_ALLOC_STRUCT(c)) == NULL) { rv = NNG_ENOMEM; goto error; }
	nni_mtx_init(&c->mtx);
	nni_atomic_init(&c->refs);
    nni_atomic_flag_reset(&c->reaping);
	nni_atomic_set(&c->refs, 1);
	c->dialer = d;
    nni_aio_init(&c->reopen_aio, channel_retry, c);
    c->reopen_initialized = true;
	nni_atomic_inc64(&d->ref);
	c->priority = d->priority;
	c->channels = d->enable_mltstrm ? 1 + QUIC_SUB_STREAM_NUM : 1;
	c->stream.s_free = conn_free; c->stream.s_close = conn_close;
	c->stream.s_send = stream_send; c->stream.s_recv = stream_recv;
	c->stream.s_get = stream_get; c->stream.s_set = stream_set;
	for (unsigned i = 0; i < c->channels; ++i) {
		c->channel[i].conn = c;
		nni_aio_list_init(&c->channel[i].readers);
		NNI_LIST_INIT(&c->channel[i].sends, quic_send, node);
	}
	status = d->api->ConfigurationOpen(d->registration, &alpn, 1,
	    &d->settings, sizeof(d->settings), NULL, &c->config);
	if (QUIC_FAILED(status)) { rv = NNG_EINVAL; goto error; }
	cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
	if (!d->verify_peer) cred.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
	if (d->ca && d->verify_peer) {
		cred.Flags |= QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
		cred.CaCertificateFile = d->ca;
	}
	if (!!d->cacert != !!d->key) { rv = NNG_EINVAL; goto error; }
	if (d->cacert) {
		if (d->password) {
			protected_cert.CertificateFile = d->cacert;
			protected_cert.PrivateKeyFile = d->key;
			protected_cert.PrivateKeyPassword = d->password;
			cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
			cred.CertificateFileProtected = &protected_cert;
		} else {
			cert.CertificateFile = d->cacert; cert.PrivateKeyFile = d->key;
			cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
			cred.CertificateFile = &cert;
		}
	}
	if (QUIC_FAILED(d->api->ConfigurationLoadCredential(c->config, &cred))) {
		rv = NNG_EPEERAUTH; goto error;
	}
	if (QUIC_FAILED(d->api->ConnectionOpen(d->registration, connection_cb, c, &c->handle))) {
		rv = NNG_ECONNREFUSED; goto error;
	}
	if (d->enable_0rtt && d->rticket_sz)
		d->api->SetParam(c->handle, QUIC_PARAM_CONN_RESUMPTION_TICKET, d->rticket_sz, d->rticket);
	nni_mtx_lock(&c->mtx);
	rv = nni_aio_schedule(aio, dial_cancel, c);
	if (rv != 0) { nni_mtx_unlock(&c->mtx); goto error; }
	c->aio = aio;
	d->currcon = c;
	status = d->api->ConnectionStart(c->handle, c->config, QUIC_ADDRESS_FAMILY_UNSPEC, host, (uint16_t)p);
	if (QUIC_FAILED(status)) {
		c->aio = NULL; d->currcon = NULL;
		nni_mtx_unlock(&c->mtx);
		rv = NNG_ECONNREFUSED; goto error;
	}
	nni_mtx_unlock(&c->mtx);
	nni_mtx_unlock(&d->mtx);
	return;
error:
	nni_mtx_unlock(&d->mtx);
	nni_aio_finish_error(aio, rv);
	if (c) conn_release(c);
}
