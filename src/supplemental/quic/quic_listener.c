//
// Copyright 2023 NanoMQ Team, Inc. <wangwei@emqx.io>
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

struct quic_listener {
	nng_stream_listener ops;
	const char *        host;
	const char *        port;
	void *              l; // platform listener
};

static void
quic_listener_free(void *arg)
{
	quic_listener *l = arg;
	nni_quic_listener_fini(l->l);
	NNI_FREE_STRUCT(l);
}

static void
quic_listener_close(void *arg)
{
	quic_listener *l = arg;
	nni_quic_listener_close(l->l);
}

static int
quic_listener_listen(void *arg)
{
	quic_listener *l = arg;
	return (nni_quic_listener_listen(l->l, &l->sa));
}

static void
quic_listener_accept(void *arg, nng_aio *aio)
{
	quic_listener *l = arg;
	nni_quic_listener_accept(l->l, aio);
}

/*
static int
quic_listener_get(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	quic_listener *l = arg;
	if (strcmp(name, NNG_OPT_TCP_BOUND_PORT) == 0) {
		return (quic_listener_get_port(l, buf, szp, t));
	}
	return (nni_quic_listener_get(l->l, name, buf, szp, t));
}

static int
quic_listener_set(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	quic_listener *l = arg;
	return (nni_quic_listener_set(l->l, name, buf, sz, t));
}
*/

static int
quic_listener_alloc_addr(nng_stream_listener **lp, const char *h, const char *p)
{
	quic_listener *l;
	int           rv;

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((rv = nni_quic_listener_init(&l->l)) != 0) {
		NNI_FREE_STRUCT(l);
		return (rv);
	}
	l->host = h;
	l->port = p;

	l->ops.sl_free   = quic_listener_free;
	l->ops.sl_close  = quic_listener_close;
	l->ops.sl_listen = quic_listener_listen;
	l->ops.sl_accept = quic_listener_accept;
	l->ops.sl_get    = quic_listener_get;
	l->ops.sl_set    = quic_listener_set;

	*lp = (void *) l;
	return (0);
}

int
nni_quic_listener_alloc(nng_stream_listener **lp, const nni_url *url)
{
	int          rv;
	const char * h, *p;

	if ((rv = nni_init()) != 0) {
		return (rv);
	}

	h = url->u_hostname;
	// Wildcard special case, which means bind to INADDR_ANY.
	if ((h != NULL) && ((strcmp(h, "*") == 0) || (strcmp(h, "") == 0))) {
		h = NULL;
	}

	p = url->u_port;
	if (p == NULL) {
		return NNG_EADDRINVAL;
	}

	return (quic_listener_alloc_addr(lp, h, p));
}

