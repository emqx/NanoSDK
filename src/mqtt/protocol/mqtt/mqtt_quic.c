//
// Copyright 2020 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "supplemental/quic/quic_api.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/mqtt/mqtt_qos_db_api.h"

#define NNG_MQTT_SELF 0
#define NNG_MQTT_SELF_NAME "mqtt-client"
#define NNG_MQTT_PEER 0
#define NNG_MQTT_PEER_NAME "mqtt-server"
typedef struct mqtt_sock_s mqtt_sock_t;

static void mqtt_quic_sock_init(void *arg, nni_sock *sock);
static void mqtt_quic_sock_fini(void *arg);
static void mqtt_quic_sock_open(void *arg);
static void mqtt_quic_sock_send(void *arg, nni_aio *aio);
static void mqtt_quic_sock_recv(void *arg, nni_aio *aio);
static void mqtt_send_cb(void *arg);
static void mqtt_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

// A mqtt_sock_s is our per-socket protocol private structure.
struct mqtt_sock_s {
};

// A mqtt_pipe_s is our per-pipe protocol private structure.
struct mqtt_pipe_s {
};

/******************************************************************************
 *                              Sock Implementation                           *
 ******************************************************************************/

static void
mqtt_quic_sock_fini(void *arg)
{
}

static void
mqtt_quic_sock_send(void *arg, nni_aio *aio)
{
}

static void
mqtt_quic_sock_recv(void *arg, nni_aio *aio)
{
}

static void mqtt_quic_sock_init(void *arg, nni_sock *sock)
{
	NNI_ARG_UNUSED(arg);
}

/* Stream EQ Pipe ???? */

static void
quic_mqtt_stream_init(void *arg, void *pipe, void *s)
{
}

void
quic_mqtt_stream_open(void *arg)
{
}

void
quic_mqtt_stream_close(void *arg)
{
}

static void
mqtt_quic_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}


static void
mqtt_quic_sock_close(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static nni_proto_pipe_ops mqtt_quic_pipe_ops = {
	// .pipe_size  = sizeof(mqtt_pipe_t),
	// .pipe_init  = mqtt_pipe_init,
	// .pipe_fini  = mqtt_pipe_fini,
	// .pipe_start = mqtt_pipe_start,
	// .pipe_close = mqtt_pipe_close,
	// .pipe_stop  = mqtt_pipe_stop,
};

static nni_option mqtt_quic_ctx_options[] = {
	{
	    .o_name = NULL,
	},
};

static nni_proto_ctx_ops mqtt_quic_ctx_ops = {
	// .ctx_size    = sizeof(mqtt_ctx_t),
	// .ctx_init    = mqtt_ctx_init,
	// .ctx_fini    = mqtt_ctx_fini,
	// .ctx_recv    = mqtt_ctx_recv,
	// .ctx_send    = mqtt_ctx_send,
	// .ctx_options = mqtt_ctx_options,
};

static nni_option mqtt_quic_sock_options[] = {
	// terminate list
	{
	    .o_name = NULL,
	},
};

static nni_proto_sock_ops mqtt_quic_sock_ops = {
	.sock_size    = sizeof(mqtt_sock_t),
	.sock_init    = mqtt_quic_sock_init,
	.sock_fini    = mqtt_quic_sock_fini,
	.sock_open    = mqtt_quic_sock_open,
	.sock_close   = mqtt_quic_sock_close,
	.sock_options = mqtt_quic_sock_options,
	// .sock_send    = mqtt_quic_sock_send,
	// .sock_recv    = mqtt_quic_sock_recv,
};

static nni_proto mqtt_msquic_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MQTT_SELF, NNG_MQTT_SELF_NAME },
	.proto_peer     = { NNG_MQTT_PEER, NNG_MQTT_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mqtt_quic_sock_ops,
	.proto_pipe_ops = &mqtt_quic_pipe_ops,
	.proto_ctx_ops  = &mqtt_quic_ctx_ops,
};

// As taking msquic as tranport, we exclude the dialer for now.
int
nng_mqtt_quic_client_open(nng_socket *sock, const char *url)
{

	int rv = 0;
	// Quic settings
	if ((rv = nni_proto_open(sock, &mqtt_msquic_proto)) == 0) {
		// quic open
		quic_open();
		quic_connect(url);
	}

	return rv;
}
