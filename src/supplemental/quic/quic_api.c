//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//
#include "quic_api.h"
#include "core/nng_impl.h"
#include "msquic.h"

#include "nng/mqtt/mqtt_client.h"
#include "nng/nng.h"
#include "nng/supplemental/nanolib/conf.h"
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

struct quic_dialer {
	nng_stream_dialer ops;
	bool              closed;
	nni_mtx           mtx;
	char *            host;
	char *            port;
	nni_aio *         conaio;
	nni_list          conaios; // TODO

	void *            d; // platform dialer
};

int
nni_quic_listener_alloc(nng_stream_listener **lp, const nni_url *url)
{
	NNI_ARG_UNUSED(lp);
	NNI_ARG_UNUSED(url);

	return 0;
}

static void
quic_dial_cancel(nni_aio *aio, void *arg, int rv)
{
	quic_dialer *d = arg;

	nni_mtx_lock(&d->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);

		if (nni_list_empty(&d->conaios)) {
			nni_aio_abort(d->conaio, NNG_ECANCELED);
		}
	}
	nni_mtx_unlock(&d->mtx);
}

static void
quic_dial_con_cb(void *arg)
{
	quic_dialer *d = arg;
	nng_aio *   aio;
	int         rv;

	nni_mtx_lock(&d->mtx);
	rv = nni_aio_result(d->conaio);
	if ((d->closed) || ((aio = nni_list_first(&d->conaios)) == NULL)) {
		if (rv == 0) {
			// The type of this output is the nni_quic_conn.
			// Make sure we discard the underlying connection.
			nng_stream_free(nni_aio_get_output(d->conaio, 0));
			nni_aio_set_output(d->conaio, 0, NULL);
		}
		nni_mtx_unlock(&d->mtx);
		return;
	}
	nni_list_remove(&d->conaios, aio);
	if (rv != 0) {
		nni_aio_finish_error(aio, rv);
	} else {
		nni_aio_set_output(aio, 0, nni_aio_get_output(d->conaio, 0));
		nni_aio_set_output(aio, 1, nni_aio_get_output(d->conaio, 1));
		nni_aio_finish(aio, 0, 0);
	}

	// Start next dialer if it exists
	if (!nni_list_empty(&d->conaios))
		nni_quic_dial(d->d, d->host, d->port, d->conaio);
	nni_mtx_unlock(&d->mtx);
}

static void
quic_dialer_close(void *arg)
{
	quic_dialer *d = arg;
	nni_aio *    aio;

	nni_mtx_lock(&d->mtx);
	d->closed = true;
	while ((aio = nni_list_first(&d->conaios)) != NULL) {
		nni_list_remove(&d->conaios, aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	nni_quic_dialer_close(d->d);
	nni_mtx_unlock(&d->mtx);
}

static void
quic_dialer_dial(void *arg, nng_aio *aio)
{
	quic_dialer *d = arg;
	int          rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&d->mtx);
	if (d->closed) {
		nni_mtx_unlock(&d->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if ((rv = nni_aio_schedule(aio, quic_dial_cancel, d)) != 0) {
		nni_mtx_unlock(&d->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_list_append(&d->conaios, aio);
	if (nni_list_first(&d->conaios) == aio) {
		if (!nni_aio_busy(d->conaio)) {
			nni_quic_dial(d->d, d->host, d->port, d->conaio);
		}
	}
	nni_mtx_unlock(&d->mtx);
	log_debug("[quic dialer dial] end");
}

static int
quic_dialer_set_tls_ca(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	char *           str;
	char *           old;

	str = nng_alloc(sz + 1);
	memset(str, '\0', sz + 1);

	if (((rv = nni_copyin_str(str, buf, sz, sz, t)) != 0) || (d == NULL)) {
		nng_free(str, sz + 1);
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	old = d->ca;
	d->ca = str;
	nni_mtx_unlock(&d->mtx);
	if (old != NULL) {
		nng_free(old, 0);
	}
	return 0;
}

static int
quic_dialer_set_tls_verify(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	bool             b;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->verify_peer = b;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_tls_pwd(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	char *           str;
	char *           old;

	str = nng_alloc(sz + 1);
	memset(str, '\0', sz + 1);

	if (((rv = nni_copyin_str(str, buf, sz, sz, t)) != 0) || (d == NULL)) {
		nng_free(str, sz + 1);
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	old = d->password;
	d->password = str;
	nni_mtx_unlock(&d->mtx);
	if (old != NULL) {
		nng_free(old, 0);
	}
	return 0;
}

static int
quic_dialer_set_tls_key(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	char *           str;
	char *           old;

	str = nng_alloc(sz + 1);
	memset(str, '\0', sz + 1);

	if (((rv = nni_copyin_str(str, buf, sz, sz, t)) != 0) || (d == NULL)) {
		nng_free(str, sz + 1);
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	old = d->key;
	d->key = str;
	nni_mtx_unlock(&d->mtx);
	if (old != NULL) {
		nng_free(old, 0);
	}
	return 0;
}

// static int
// sock_set_sockname(void *s, const void *buf, size_t sz, nni_type t)
// {
// 	int rv;
// 	rv = (nni_copyin_str(
// 	    SOCK(s)->s_name, buf, sizeof(SOCK(s)->s_name), sz, t));
// #ifdef NNG_ENABLE_STATS
// 	if (rv == 0) {
// 		nni_stat_set_string(&SOCK(s)->st_name, SOCK(s)->s_name);
// 	}
// #endif
// 	return (rv);
// }

static int
quic_dialer_set_tls_cacert(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	char *           str;
	char *           old;

	str = nng_alloc(sz + 1);
	memset(str, '\0', sz + 1);

	if (((rv = nni_copyin_str(str, buf, sz, sz, t)) != 0) || (d == NULL)) {
		nng_free(str, sz + 1);
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	old = d->cacert;
	d->cacert = str;
	nni_mtx_unlock(&d->mtx);
	if (old != NULL) {
		nng_free(old, 0);
	}
	return 0;
}

static int
quic_dialer_set_congestion_ctl_cubic(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	bool             b;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->settings.IsSet.CongestionControlAlgorithm = TRUE;
	d->settings.CongestionControlAlgorithm = QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_max_ack_delay_ms(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint32_t         n;

	// range from 0 to 600000 ms
	if (((rv = nni_copyin_int((int *)&n, buf, sz, 0, 600000, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qmax_ack_delay_ms = n;
	d->settings.IsSet.MaxAckDelayMs = TRUE;
	d->settings.MaxAckDelayMs = n;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_initial_rtt_ms(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint32_t         n;

	// range from 0 to 600000 ms
	if (((rv = nni_copyin_int((int *)&n, buf, sz, 0, 600000, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qinitial_rtt_ms = n;
	d->settings.IsSet.InitialRttMs = TRUE;
	d->settings.InitialRttMs = n;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_send_idle_timeout(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint32_t         n;

	// range from 0 to 60 seconds
	if (((rv = nni_copyin_int((int *)&n, buf, sz, 0, 60, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qsend_idle_timeout = n;
	d->settings.IsSet.SendIdleTimeoutMs = TRUE;
	d->settings.SendIdleTimeoutMs = n * 1000;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_disconnect_timeout(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint32_t         n;

	// range from 0 to 600 seconds
	if (((rv = nni_copyin_int((int *)&n, buf, sz, 0, 600, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qdiscon_timeout = n;
	d->settings.IsSet.DisconnectTimeoutMs = TRUE;
	d->settings.DisconnectTimeoutMs = n * 1000;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_connect_timeout(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint64_t         n;

	if (((rv = nni_copyin_u64(&n, buf, sz, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qconnect_timeout = n;
	d->settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
	d->settings.HandshakeIdleTimeoutMs = n * 1000;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_keepalive(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint32_t         n;

	// range from 0 to 1800 seconds
	if (((rv = nni_copyin_int((int *)&n, buf, sz, 0, 1800, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qkeepalive = n;
	d->settings.IsSet.KeepAliveIntervalMs = TRUE;
	d->settings.KeepAliveIntervalMs = n * 1000;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_set_priority(void *arg, const void *buf, size_t sz, nni_type t)
{
	NNI_ARG_UNUSED(sz);
	NNI_ARG_UNUSED(t);
	nni_quic_dialer *d = arg;
	int *_buf = (int *)buf;

	nni_mtx_lock(&d->mtx);
	d->priority = *_buf;
	nni_mtx_unlock(&d->mtx);

	return 0;
}

static int
quic_dialer_set_idle_timeout(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	uint64_t         n;

	if (((rv = nni_copyin_u64(&n, buf, sz, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->qidle_timeout = n;
	d->settings.IsSet.IdleTimeoutMs = TRUE;
	d->settings.IdleTimeoutMs = n * 1000;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_get_enable_multistream(void *arg, void *buf, size_t *szp, nni_type t)
{
	bool             b;
	nni_quic_dialer *d = arg;
	nni_mtx_lock(&d->mtx);
	b = d->enable_mltstrm;
	nni_mtx_unlock(&d->mtx);
	return nni_copyout_bool(b, buf, szp, t);
}

static int
quic_dialer_set_enable_multistream(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	bool             b;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->enable_mltstrm = b;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static int
quic_dialer_get_enable_0rtt(void *arg, void *buf, size_t *szp, nni_type t)
{
	bool             b;
	nni_quic_dialer *d = arg;
	nni_mtx_lock(&d->mtx);
	b = d->enable_0rtt;
	nni_mtx_unlock(&d->mtx);
	return nni_copyout_bool(b, buf, szp, t);
}

static int
quic_dialer_set_enable_0rtt(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_quic_dialer *d = arg;
	int              rv;
	bool             b;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0) || (d == NULL)) {
		return rv;
	}

	nni_mtx_lock(&d->mtx);
	d->enable_0rtt = b;
	nni_mtx_unlock(&d->mtx);
	return 0;
}

static const nni_option quic_dialer_options[] = {
	{
		.o_name = NNG_OPT_QUIC_ENABLE_0RTT,
		.o_get = quic_dialer_get_enable_0rtt,
		.o_set = quic_dialer_set_enable_0rtt,
	},
	{
		.o_name = NNG_OPT_QUIC_ENABLE_MULTISTREAM,
		.o_get = quic_dialer_get_enable_multistream,
		.o_set = quic_dialer_set_enable_multistream,
	},
	{
		.o_name = NNG_OPT_QUIC_IDLE_TIMEOUT,
		.o_get = NULL,
		.o_set = quic_dialer_set_idle_timeout,
	},
	{
		.o_name = NNG_OPT_QUIC_KEEPALIVE,
		.o_get = NULL,
		.o_set = quic_dialer_set_keepalive,
	},
	{
		.o_name = NNG_OPT_QUIC_CONNECT_TIMEOUT,
		.o_get = NULL,
		.o_set = quic_dialer_set_connect_timeout,
	},
	{
		.o_name = NNG_OPT_QUIC_DISCONNECT_TIMEOUT,
		.o_get = NULL,
		.o_set = quic_dialer_set_disconnect_timeout,
	},
	{
		.o_name = NNG_OPT_QUIC_SEND_IDLE_TIMEOUT,
		.o_get = NULL,
		.o_set = quic_dialer_set_send_idle_timeout,
	},
	{
		.o_name = NNG_OPT_QUIC_INITIAL_RTT_MS,
		.o_get = NULL,
		.o_set = quic_dialer_set_initial_rtt_ms,
	},
	{
		.o_name = NNG_OPT_QUIC_MAX_ACK_DELAY_MS,
		.o_get = NULL,
		.o_set = quic_dialer_set_max_ack_delay_ms,
	},
	{
		.o_name = NNG_OPT_QUIC_CONGESTION_CTL_CUBIC,
		.o_get = NULL,
		.o_set = quic_dialer_set_congestion_ctl_cubic,
	},
	{
		.o_name = NNG_OPT_QUIC_TLS_CACERT_PATH,
		.o_get = NULL,
		.o_set = quic_dialer_set_tls_cacert,
	},
	{
		.o_name = NNG_OPT_QUIC_TLS_KEY_PATH,
		.o_get = NULL,
		.o_set = quic_dialer_set_tls_key,
	},
	{
		.o_name = NNG_OPT_QUIC_TLS_KEY_PASSWORD,
		.o_get = NULL,
		.o_set = quic_dialer_set_tls_pwd,
	},
	{
		.o_name = NNG_OPT_QUIC_TLS_VERIFY_PEER,
		.o_get = NULL,
		.o_set = quic_dialer_set_tls_verify,
	},
	{
		.o_name = NNG_OPT_QUIC_TLS_CA_PATH,
		.o_get = NULL,
		.o_set = quic_dialer_set_tls_ca,
	},
	{
		.o_name = NNG_OPT_QUIC_PRIORITY,
		.o_get = NULL,
		.o_set = quic_dialer_set_priority,
	},
	{
		.o_name = NULL,
	}
};

static int
quic_dialer_get(void *arg, const char *name, void *buf, size_t* szp, nni_type t)
{
	quic_dialer *d = arg;
	return(nni_getopt(quic_dialer_options, name, d->d, buf, szp, t));
}

static int
quic_dialer_set(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	quic_dialer *d = arg;
	return(nni_setopt(quic_dialer_options, name, d->d, buf, sz, t));
}

static void
quic_dialer_free(void *arg)
{
	quic_dialer *d = arg;

	if (d == NULL)
		return;

	nni_aio_stop(d->conaio);
	nni_aio_free(d->conaio);

	if (d->d != NULL) {
		nni_quic_dialer_close(d->d);
		nni_quic_dialer_fini(d->d);
	}

	nni_strfree(d->host);
	nni_strfree(d->port);

	nni_mtx_fini(&d->mtx);
	NNI_FREE_STRUCT(d);
}

static int
quic_dialer_alloc(quic_dialer **dp)
{
	int          rv;
	quic_dialer *d;

	if ((d = NNI_ALLOC_STRUCT(d)) == NULL) {
		return (NNG_ENOMEM);
	}

	nni_mtx_init(&d->mtx);
	nni_aio_list_init(&d->conaios);

	if (((rv = nni_aio_alloc(&d->conaio, quic_dial_con_cb, d)) != 0) ||
	    ((rv = nni_quic_dialer_init(&d->d)) != 0)) {
		quic_dialer_free(d);
		return (rv);
	}

	d->ops.sd_close = quic_dialer_close;
	d->ops.sd_free  = quic_dialer_free;
	d->ops.sd_dial  = quic_dialer_dial;
	d->ops.sd_get   = quic_dialer_get;
	d->ops.sd_set   = quic_dialer_set;

	*dp = d;
	return (0);
}

int
nni_quic_dialer_alloc(nng_stream_dialer **dp, const nni_url *url)
{
	quic_dialer *d;
	int          rv;
	const char  *p;

	// TODO ref inc when the connection to the `url` was established


	if ((rv = quic_dialer_alloc(&d)) != 0) {
		return (rv);
	}

	if (((p = url->u_port) == NULL) || (strlen(p) == 0)) {
		quic_dialer_free(d);
		return (NNG_EADDRINVAL);
	}

	if (strlen(url->u_hostname) == 0) {
		// Dialer needs both a destination hostname and port.
		quic_dialer_free(d);
		return (NNG_EADDRINVAL);
	}

	if (((d->host = nng_strdup(url->u_hostname)) == NULL) ||
	    ((d->port = nng_strdup(p)) == NULL)) {
		quic_dialer_free(d);
		return (NNG_ENOMEM);
	}

	*dp = (void *) d;
	return (0);
}

