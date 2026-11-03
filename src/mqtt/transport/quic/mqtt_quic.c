//
// Copyright 2024 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#define NANO_MAX_PACKET_SIZE 268435455u
#define NNI_NANO_MAX_HEADER_SIZE 5
#define NANO_MAX_QOS_PACKET 65535u
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "nng/supplemental/nanolib/log.h"
#include "core/sockimpl.h"
#include "nng/mqtt/mqtt_client.h"
#include "nng/mqtt/mqtt_quic_client.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/quic/quic_api.h"

// QUIC transport.   Platform specific QUIC operations must be
// supplied as well.

typedef struct mqtt_quictran_pipe mqtt_quictran_pipe;
typedef struct mqtt_quictran_ep   mqtt_quictran_ep;

typedef struct mqtt_quic_sub_stream   quic_substream;

struct mqtt_quic_sub_stream {
	mqtt_quictran_pipe  *pipe;		// refer to main pipe
	nni_aio         	 saio;		//for substream
	nni_aio         	 raio;
	nni_aio         	 qaio;		// qsaio
	nni_msg             *rxmsg;
	nni_msg             *pending_rx;
	nni_list             sendq;
	uint16_t			 level;
	int id;	// To command stream layer
    int send_flags;
	uint8_t         	 txlen[sizeof(uint64_t)];
	uint8_t          	 rxlen[sizeof(uint64_t)]; // fixed header
	nni_lmq              rslmq;
	bool                 busy;
	nni_mtx          	 mtx;

	size_t           gottxhead;
	size_t           gotrxhead;
	size_t           wanttxhead;
	size_t           wantrxhead;
};
struct mqtt_quictran_pipe {
	quic_substream   substreams[QUIC_SUB_STREAM_NUM];
	nng_stream *     conn;
	nni_msg *connack;
	bool multi_stream;
	nni_id_map inflight; // MQTT 5 credit, keyed by stream and packet ID
    int send_flags;
	nni_pipe *       npipe;
	nni_list_node    node;
	mqtt_quictran_ep *ep;
	nni_atomic_flag  reaped;
	nni_reap_node    reap;
	uint32_t         packmax; // MQTT Maximum Packet Size (Max length)
	uint16_t         peer;    // broker info
	uint16_t         proto;   // MQTT version
	nni_duration     keepalive;
	uint16_t         sndmax;  // MQTT Receive Maximum (QoS 1/2 packet)
	uint8_t          qosmax;
	uint8_t          txlen[sizeof(uint64_t)];
	uint8_t          rxlen[sizeof(uint64_t)]; // fixed header
	size_t           rcvmax;
	size_t           gottxhead;
	size_t           gotrxhead;
	size_t           wanttxhead;
	size_t           wantrxhead;
	nni_list         recvq;
	nni_list         sendq;
	nni_aio         *txaio;
	nni_aio         *rxaio;
	nni_aio         *qsaio; // aio for qos/pingreq
	nni_aio         *negoaio;
	nni_aio         *rpaio;
	nni_msg         *rxmsg;
	nni_msg         *pending_rx;
	nni_lmq          rslmq;	// resend lmq
	nni_lmq          rxlmq; // recv lmq, shared by all streams
	nni_mtx          mtx;
	nni_atomic_bool  closed;
	bool             busy;
};

struct mqtt_quictran_ep {
	nni_mtx              mtx;
	uint16_t             proto; //socket's 16-bit protocol number
	nni_duration         backoff;
	nni_duration         backoff_max;
	bool                 fini;
	bool                 started;
	bool                 closed;
	bool                 no_local_v4;
	nng_url *            url;
	const char *         host; // for dialers
	nng_sockaddr         src;
	int                  refcnt; // active pipes
	reason_code          reason_code;
	nni_aio *            useraio;
	nni_aio *            connaio;
	nni_aio *            timeaio;
	nni_sock *           nsock;
	nni_list             busypipes; // busy pipes -- ones passed to socket
	nni_list             waitpipes; // pipes waiting to match to socket
	nni_list             negopipes; // pipes busy negotiating
	nni_reap_node        reap;
	nng_stream_dialer *  dialer;
	nng_stream_listener *listener;
	nni_dialer *         ndialer;
	void *               property;  // property
	void *               connmsg;
};

static void mqtt_share_pipe_send_cb(void *, nni_aio *, quic_substream *);
static void mqtt_share_pipe_recv_cb(void *, nni_aio *, quic_substream *, nni_msg **);
static void mqtt_quictran_pipe_send_start(mqtt_quictran_pipe *, quic_substream *);
static void mqtt_quictran_subpipe_send_cb(void *);
static void mqtt_quictran_subpipe_recv_cb(void *);
static void mqtt_quictran_pipe_send_cb(void *);
static void mqtt_quictran_pipe_recv_cb(void *);
static void mqtt_quictran_subpipe_qos_cb(void *);
static void mqtt_quictran_pipe_qos_send_cb(void *);
static void mqtt_quictran_share_qos_send_cb(void *, nni_aio *, quic_substream *);

static void mqtt_quictran_pipe_rp_send_cb(void *);
static void mqtt_quictran_pipe_nego_cb(void *);
static void mqtt_quictran_ep_fini(void *);
static void mqtt_quictran_pipe_fini(void *);

static nni_reap_list quictran_ep_reap_list = {
	.rl_offset = offsetof(mqtt_quictran_ep, reap),
	.rl_func   = mqtt_quictran_ep_fini,
};

static nni_reap_list quictran_pipe_reap_list = {
	.rl_offset = offsetof(mqtt_quictran_pipe, reap),
	.rl_func   = mqtt_quictran_pipe_fini,
};

static void
mqtt_quictran_init(void)
{
}

static void
mqtt_quictran_fini(void)
{
}

// Complete every upper-layer operation before stopping the stream AIOs.
// The stream callbacks may still run, but no longer own an entry in these queues.
static void
mqtt_quictran_pipe_close(void *arg)
{
	mqtt_quictran_pipe *p = arg;
	nni_aio *aio;
	nni_mtx_lock(&p->mtx);
	nni_atomic_set_bool(&p->closed, true);
	while ((aio = nni_list_first(&p->recvq)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	for (unsigned i = 0; i <= QUIC_SUB_STREAM_NUM; i++) {
		nni_list *q = i ? &p->substreams[i - 1].sendq : &p->sendq;
		while ((aio = nni_list_first(q)) != NULL) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
	}
	nni_mtx_unlock(&p->mtx);
	nng_stream_close(p->conn);
	for (unsigned i = 0; i < QUIC_SUB_STREAM_NUM; i++) {
		quic_substream *stream = &p->substreams[i];
		nni_aio_close(&stream->raio);
		nni_aio_close(&stream->saio);
		nni_aio_close(&stream->qaio);
	}
	nni_aio_close(p->rxaio);
	nni_aio_close(p->qsaio);
	nni_aio_close(p->txaio);
	nni_aio_close(p->negoaio);
	nni_aio_close(p->rpaio);
}

static void
mqtt_quictran_pipe_stop(void *arg)
{
	mqtt_quictran_pipe *p = arg;

	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		quic_substream *stream = &p->substreams[i];
		nni_aio_stop(&stream->raio);
		nni_aio_stop(&stream->saio);
		nni_aio_stop(&stream->qaio);
	}
	nni_aio_stop(p->rxaio);
	nni_aio_stop(p->qsaio);
	nni_aio_stop(p->txaio);
	nni_aio_stop(p->negoaio);
	nni_aio_stop(p->rpaio);
}

static int
mqtt_quictran_pipe_init(void *arg, nni_pipe *npipe)
{
	mqtt_quictran_pipe *p = arg;
	p->npipe = npipe;
	return 0;
}

static void
mqtt_quictran_pipe_fini(void *arg)
{
	mqtt_quictran_pipe *p = arg;
	mqtt_quictran_ep *  ep;

	// Cautious, do we need to stop pipe here?
	log_debug(" ##### pipe %p finit ##### ", p);
	mqtt_quictran_pipe_stop(p);
	if ((ep = p->ep) != NULL) {
		nni_mtx_lock(&ep->mtx);
		nni_list_node_remove(&p->node);
		ep->refcnt--;
		nni_mtx_unlock(&ep->mtx);
	}

	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		quic_substream *stream = &p->substreams[i];
		nni_msg_free(stream->rxmsg);
		nni_msg_free(stream->pending_rx);
		nni_msg_free(nni_aio_get_msg(&stream->qaio));
		nni_aio_fini(&stream->raio);
		nni_aio_fini(&stream->saio);
		nni_aio_fini(&stream->qaio);
		nni_lmq_flush(&stream->rslmq);
		nni_lmq_fini(&stream->rslmq);
		nni_mtx_fini(&stream->mtx);
	}
	nng_stream_free(p->conn);
	nni_msg_free(p->pending_rx);
	nni_id_map_fini(&p->inflight);
	nni_aio_free(p->rxaio);
	nni_aio_free(p->txaio);
	nni_msg_free(nni_aio_get_msg(p->qsaio));
	nni_aio_free(p->qsaio);
	nni_aio_free(p->negoaio);
	nni_aio_free(p->rpaio);
	nni_msg_free(p->rxmsg);
	nni_msg_free(p->connack);
	nni_lmq_flush(&p->rxlmq);
	nni_lmq_fini(&p->rxlmq);
	nni_lmq_flush(&p->rslmq);
	nni_lmq_fini(&p->rslmq);
	nni_mtx_fini(&p->mtx);
	NNI_FREE_STRUCT(p);
}

static void
mqtt_quictran_pipe_reap(mqtt_quictran_pipe *p)
{
	if (!nni_atomic_flag_test_and_set(&p->reaped)) {
		if (p->conn != NULL) {
			nng_stream_close(p->conn);
		}
		nni_reap(&quictran_pipe_reap_list, p);
	}
}

static int
mqtt_quictran_pipe_alloc(mqtt_quictran_pipe **pipep)
{
	mqtt_quictran_pipe *p;
	int                rv;

	if ((p = NNI_ALLOC_STRUCT(p)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&p->mtx);
	nni_aio_list_init(&p->recvq);
	nni_aio_list_init(&p->sendq);
	nni_id_map_init(&p->inflight, 1, 0x4ffff, false);

	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		quic_substream *stream = &p->substreams[i];
		stream->pipe = p;
		nni_aio_list_init(&stream->sendq);
		stream->id = (i + 1);
		nni_mtx_init(&stream->mtx);
		nni_lmq_init(&stream->rslmq, 512);
		nni_aio_init(&stream->raio, mqtt_quictran_subpipe_recv_cb, stream);
		nni_aio_init(&stream->saio, mqtt_quictran_subpipe_send_cb, stream);
		nni_aio_init(&stream->qaio, mqtt_quictran_subpipe_qos_cb, stream);
		nni_aio_set_prov_data(&stream->qaio, &stream->id);
		nni_aio_set_prov_data(&stream->raio, &stream->id);
		nni_aio_set_prov_data(&stream->saio, &stream->id);
	}

	nni_lmq_init(&p->rslmq, NNG_MAX_SEND_LMQ);
	nni_lmq_init(&p->rxlmq, NNG_MAX_RECV_LMQ);
	nni_atomic_init_bool(&p->closed);
	p->packmax = NANO_MAX_PACKET_SIZE;
	p->qosmax = 2;
	if (((rv = nni_aio_alloc(&p->txaio, mqtt_quictran_pipe_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->rxaio, mqtt_quictran_pipe_recv_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->qsaio, mqtt_quictran_pipe_qos_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->rpaio, mqtt_quictran_pipe_rp_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->negoaio, mqtt_quictran_pipe_nego_cb, p)) != 0)) {
		mqtt_quictran_pipe_fini(p);
		return (rv);
	}

	nni_aio_list_init(&p->recvq);
	nni_aio_list_init(&p->sendq);
	nni_atomic_flag_reset(&p->reaped);

	*pipep = p;

	return (0);
}

static void
mqtt_quictran_ep_match(mqtt_quictran_ep *ep)
{
	nni_aio *          aio;
	mqtt_quictran_pipe *p;

	if (((aio = ep->useraio) == NULL) ||
	    ((p = nni_list_first(&ep->waitpipes)) == NULL)) {
		return;
	}
	nni_list_remove(&ep->waitpipes, p);
	nni_list_append(&ep->busypipes, p);
	ep->useraio = NULL;
	nni_aio_set_output(aio, 0, p);
	nni_aio_finish(aio, 0, 0);
}

static void
mqtt_quictran_pipe_nego_cb(void *arg)
{
	mqtt_quictran_pipe *p   = arg;
	mqtt_quictran_ep *  ep  = p->ep;
	nni_aio *          aio = p->negoaio;
	nni_aio *          uaio;
	int                rv;
	int                var_int;
	uint8_t            pos = 0;

	nni_mtx_lock(&ep->mtx);

	if ((rv = nni_aio_result(aio)) != 0) {
		log_info("aio result %s", nng_strerror(rv));
		rv = SERVER_UNAVAILABLE;
		goto error;
	}

	if (p->gottxhead < p->wanttxhead) {
		size_t n = nni_aio_count(aio);
		p->gottxhead += n;
		nni_aio_iov_advance(aio, n);
		if (nni_aio_iov_count(aio)) {
			nng_stream_send(p->conn, aio);
			nni_mtx_unlock(&ep->mtx);
			return;
		}
	} else {
		p->gotrxhead += nni_aio_count(aio);
	}
	// receving fixed header
	if (p->gotrxhead < 2 ||
	    (p->gotrxhead <= 5 && p->rxlen[p->gotrxhead - 1] > 0x7f &&
	        p->rxmsg == NULL)) {
		nni_iov iov;
		if (p->gotrxhead >= 5) { rv = NNG_EPROTO; goto error; }
		iov.iov_buf = &p->rxlen[p->gotrxhead];
		if (p->gotrxhead < 2) {
			iov.iov_len = p->wantrxhead - p->gotrxhead;
		} else {
			iov.iov_len = 1;
		}
		nni_aio_set_iov(aio, 1, &iov);
		nng_stream_recv(p->conn, aio);
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	// only accept CONNACK msg
	if ((p->rxlen[0] != CMD_CONNACK) &&
	    (p->rxlen[0] != CMD_AUTH_V5)) {
		log_error("Invalid type received %x %x", p->rxlen[0], p->rxlen[1]);
		rv = PROTOCOL_ERROR;
		goto error;
	}
	// finish recevied fixed header
	if (p->rxmsg == NULL) {
		pos = 0;
		if ((rv = mqtt_get_remaining_length(p->rxlen, p->gotrxhead,
		         (uint32_t *) &var_int, &pos)) != 0) {
			rv = PAYLOAD_FORMAT_INVALID;
			goto error;
		}
		size_t rcvmax = p->rcvmax > 0 ? p->rcvmax : NANO_MAX_PACKET_SIZE;
		if ((size_t) var_int + pos + 1 > rcvmax) {
			log_error("Packet too large in negotiation: %zu > %zu", (size_t)var_int, rcvmax);
			rv = PACKET_TOO_LARGE;
			goto error;
		}
		if ((rv = nni_mqtt_msg_alloc(&p->rxmsg, var_int)) != 0) {
			rv = NNG_ENOMEM;
			goto error;
		}

		nni_msg_header_append(p->rxmsg, p->rxlen, pos + 1);

		p->wantrxhead = var_int + 1 + pos;
		if (p->proto == MQTT_PROTOCOL_VERSION_v311 &&
		    ((rv = (p->wantrxhead <= 4) ? 0 : NNG_EPROTO) != 0)) {
			// Broker send a invalid CONNACK!
			rv = PROTOCOL_ERROR;
			goto error;
		}
	}
	// got remaining length
	if (p->gotrxhead < p->wantrxhead) {
		nni_iov iov;
		iov.iov_len = p->wantrxhead - p->gotrxhead;
		iov.iov_buf = (uint8_t *)nni_msg_body(p->rxmsg) + p->gotrxhead - nni_msg_header_len(p->rxmsg);
		nni_aio_set_iov(aio, 1, &iov);
		nng_stream_recv(p->conn, aio);
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	// Connack
	if (p->gotrxhead >= p->wantrxhead) {
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			rv = nni_mqttv5_msg_decode(p->rxmsg);
			ep->reason_code = rv;
			if (rv != 0)
				goto mqtt_error;
			property_free(ep->property);
			ep->property = NULL;
			property *prop = (void *)nni_mqtt_msg_get_connack_property(p->rxmsg);
			if (prop && (rv = property_dup((property **) &ep->property, prop)) != 0)
				goto mqtt_error;
			if (ep->property != NULL) {
				property_data *data;
				data = property_get_value(ep->property, RECEIVE_MAXIMUM);
				if (data) {
					if (data->p_value.u16 == 0) {
						rv = MQTT_ERR_PROTOCOL;
						ep->reason_code = rv;
						goto mqtt_error;
					} else {
						p->sndmax = data->p_value.u16;
					}
				}
				data = property_get_value(ep->property, MAXIMUM_PACKET_SIZE);
				if (data) {
					if (data->p_value.u32 == 0) {
						rv = MQTT_ERR_PROTOCOL;
						ep->reason_code = rv;
						goto mqtt_error;
					} else {
						p->packmax = data->p_value.u32;
						log_info("Set max packet size as %ld", p->packmax);
					}
				}
				data = property_get_value(ep->property, PUBLISH_MAXIMUM_QOS);
				if (data) {
					p->qosmax = data->p_value.u8;
				}
				data = property_get_value(ep->property, SERVER_KEEP_ALIVE);
				if (data) {
					p->keepalive = data->p_value.u16 * NNI_SECOND;
				}
			}
		} else {
			if ((rv = nni_mqtt_msg_decode(p->rxmsg)) != MQTT_SUCCESS) {
				ep->reason_code = rv;
				goto mqtt_error;
			}
			ep->property = NULL;
		}
		ep->reason_code = nni_mqtt_msg_get_connack_return_code(p->rxmsg);
		if (ep->reason_code != MQTT_SUCCESS) {
			log_warn("Broker rejected CONNECT with reason code %d",
			    ep->reason_code);
			rv = NNG_ECONNREFUSED;
			goto error;
		}
	}


mqtt_error:
	if (rv != MQTT_SUCCESS) goto error;
	nni_list_remove(&ep->negopipes, p);
	nni_list_append(&ep->waitpipes, p);
	p->connack = p->rxmsg;
	p->rxmsg = NULL;
	/* All stream reads start only after MQTT negotiation and queue setup. */
	for (unsigned i = 0; i <= (p->multi_stream ? QUIC_SUB_STREAM_NUM : 0); ++i) {
		nni_aio *raio = i ? &p->substreams[i-1].raio : p->rxaio;
		nni_iov iov = { i ? p->substreams[i-1].rxlen : p->rxlen, 2 };
		if (i) p->substreams[i-1].gotrxhead = 0;
		else p->gotrxhead = 0;
		nni_aio_set_iov(raio, 1, &iov);
		nng_stream_recv(p->conn, raio);
	}
	mqtt_quictran_ep_match(ep);
	nni_mtx_unlock(&ep->mtx);
	return;

error:
	// If the connection is closed, we need to pass back a different
	// error code.  This is necessary to avoid a problem where the
	// closed status is confused with the accept file descriptor
	// being closed.
	if (rv == NNG_ECLOSED) {
		rv = SERVER_SHUTTING_DOWN;
	}
	nng_stream_close(p->conn);

	if (p->rxmsg != NULL) {
		nni_msg_free(p->rxmsg);
		p->rxmsg = NULL;
	}

	if ((uaio = ep->useraio) != NULL) {
		ep->useraio = NULL;
		nni_aio_finish_error(uaio, rv);
	}
	nni_mtx_unlock(&ep->mtx);
	mqtt_quictran_pipe_reap(p);
}

static void
mqtt_quictran_subpipe_qos_cb(void *arg)
{
	quic_substream *stream = arg;
	mqtt_quictran_pipe *p = stream->pipe;
	mqtt_quictran_share_qos_send_cb(p, &stream->qaio, stream);
}

static void
mqtt_quictran_pipe_qos_send_cb(void *arg)
{
	mqtt_quictran_pipe *p = arg;
	mqtt_quictran_share_qos_send_cb(p, p->qsaio, NULL);
}

static void
mqtt_quictran_share_qos_send_cb(void *arg, nni_aio *aio, quic_substream *stream)
{
	mqtt_quictran_pipe *p = arg;
	bool *busy = stream ? &stream->busy : &p->busy;
	nni_lmq *queue = stream ? &stream->rslmq : &p->rslmq;
	nni_msg *msg;
	nni_mtx_lock(&p->mtx);
	msg = nni_aio_get_msg(aio);
	if (nni_aio_result(aio) || nni_atomic_get_bool(&p->closed)) {
		nni_aio_set_msg(aio, NULL);
		nni_msg_free(msg);
		*busy = false;
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_aio_iov_advance(aio, nni_aio_count(aio));
	if (nni_aio_iov_count(aio)) {
		nng_stream_send(p->conn, aio);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_aio_set_msg(aio, NULL);
	nni_msg_free(msg);
	*busy = false;
	if (nni_lmq_get(queue, &msg) == 0) {
		nni_iov iov[2] = {
			{ nni_msg_header(msg), nni_msg_header_len(msg) },
			{ nni_msg_body(msg), nni_msg_len(msg) }
		};
		*busy = true;
		nni_aio_set_msg(aio, msg);
		nni_aio_set_iov(aio, 2, iov);
		nng_stream_send(p->conn, aio);
	}
	nni_mtx_unlock(&p->mtx);
}

//Only for reply DISCONNECT while negotiating
static void
mqtt_quictran_pipe_rp_send_cb(void *arg)
{
	mqtt_quictran_pipe *p = arg;
	nni_aio      *rpaio = p->rpaio;
	size_t        n;
	int           rv;

	if ((rv = nni_aio_result(rpaio)) != 0) {
		log_warn(" send aio error %s", nng_strerror(rv));
		// pipe is reaped in nego_cb
		return;
	}

	nni_mtx_lock(&p->mtx);
	n = nni_aio_count(rpaio);
	nni_aio_iov_advance(rpaio, n);

	// more bytes to send
	if (nni_aio_iov_count(rpaio) > 0) {
		nng_stream_send(p->conn, rpaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	p->busy = false;
	nni_mtx_unlock(&p->mtx);
	return;
}

// only for main stream
static void
mqtt_quictran_pipe_send_cb(void *arg)
{
	mqtt_quictran_pipe *p = arg;
	nni_aio *          txaio = p->txaio;

	mqtt_share_pipe_send_cb(p, txaio, NULL);
}

// only for sub stream
static void
mqtt_quictran_subpipe_send_cb(void *arg)
{
	quic_substream 	   *stream = arg;
	mqtt_quictran_pipe *pipe   = stream->pipe;
	nni_aio            *txaio  = &stream->saio;

	mqtt_share_pipe_send_cb(pipe, txaio, stream);
}

// shared by sub stream and main stream
static void
mqtt_share_pipe_send_cb(void *arg, nni_aio *txaio, quic_substream *stream)
{
	mqtt_quictran_pipe *p = arg;
	int                rv;
	nni_aio *          aio;
	size_t             n;
	nni_msg *          msg;
	nni_list *queue = stream ? &stream->sendq : &p->sendq;

	nni_mtx_lock(&p->mtx);
	aio = nni_list_first(queue);
	if (aio == NULL) { nni_mtx_unlock(&p->mtx); return; }

	log_trace(" ############ mqtt_quictran_pipe_send_cb [%p] ############ ", p);
	if ((rv = nni_aio_result(txaio)) != 0) {
		log_warn(" send aio error %s", nng_strerror(rv));
		nni_aio_set_msg(txaio, NULL);
		nni_aio_list_remove(aio);
		mqtt_quictran_pipe_send_start(p, stream);
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}

	n = nni_aio_count(txaio);
	nni_aio_iov_advance(txaio, n);
	if (nni_aio_iov_count(txaio) > 0) {
		nng_stream_send(p->conn, txaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	nni_aio_set_msg(txaio, NULL);
	nni_aio_list_remove(aio);
	mqtt_quictran_pipe_send_start(p, stream);
	msg = nni_aio_get_msg(aio);

	if (msg != NULL) {
		n = nni_msg_len(msg);
#ifdef NNG_ENABLE_STATS
		// nni_pipe_bump_tx(p->npipe, n);
		if (p->ep != NULL) nni_sock_bump_tx(p->ep->nsock, n);
#endif
		nni_msg_free(msg);
	}
	nni_mtx_unlock(&p->mtx);

	nni_aio_set_msg(aio, NULL);
	nni_aio_finish(aio, rv, n);
}

static void
mqtt_quictran_pipe_recv_cb(void *arg)
{
	mqtt_quictran_pipe *pipe   = arg;
	nni_aio            *rxaio  = pipe->rxaio;

	mqtt_share_pipe_recv_cb(pipe, rxaio, NULL, &pipe->rxmsg);
}

// only for sub stream
static void
mqtt_quictran_subpipe_recv_cb(void *arg)
{
	quic_substream 	   *stream = arg;
	mqtt_quictran_pipe *pipe   = stream->pipe;
	nni_aio            *rxaio  = &stream->raio;

	mqtt_share_pipe_recv_cb(pipe, rxaio, stream, &stream->rxmsg);
}

static void
mqtt_share_pipe_recv_cb(void *arg, nni_aio *rxaio, quic_substream *stream, nni_msg **rxmsg)
{
	mqtt_quictran_pipe *p = arg;
	nni_aio *aio;
	nni_msg *msg = NULL;
	nni_iov iov[2];
	uint8_t *header = stream ? stream->rxlen : p->rxlen;
	size_t *got = stream ? &stream->gotrxhead : &p->gotrxhead;
	uint32_t len;
	uint8_t pos, type, flags;
	bool ack = false;
	property *prop = NULL;
	int rv = nni_aio_result(rxaio);

	nni_mtx_lock(&p->mtx);
    if (rv == NNG_ECANCELED && stream != NULL && !nni_atomic_get_bool(&p->closed)) {
        nni_msg_free(*rxmsg); *rxmsg = NULL; *got = 0;
        iov[0].iov_buf = header; iov[0].iov_len = 2;
        nni_aio_set_iov(rxaio, 1, iov);
        nng_stream_recv(p->conn, rxaio);
        nni_mtx_unlock(&p->mtx);
        return;
    }
	if (rv != 0 || nni_atomic_get_bool(&p->closed)) goto error;
	*got += nni_aio_count(rxaio);
	nni_aio_iov_advance(rxaio, nni_aio_count(rxaio));
	if (nni_aio_iov_count(rxaio)) {
		nng_stream_recv(p->conn, rxaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	if (*rxmsg == NULL) {
		if (header[*got - 1] & 0x80) {
			if (*got >= 5) { rv = NNG_EPROTO; goto error; }
			iov[0].iov_buf = header + *got;
			iov[0].iov_len = 1;
			nni_aio_set_iov(rxaio, 1, iov);
			nng_stream_recv(p->conn, rxaio);
			nni_mtx_unlock(&p->mtx);
			return;
		}
		if (mqtt_get_remaining_length(header, *got, &len, &pos) != 0) {
			rv = NNG_EPROTO; goto error;
		}
		if ((size_t)len + *got > p->rcvmax) { rv = NNG_EMSGSIZE; goto error; }
		if ((rv = nni_msg_alloc(rxmsg, len)) != 0) goto error;
		if ((rv = nni_msg_header_append(*rxmsg, header, *got)) != 0) goto error;
		if (len) {
			iov[0].iov_buf = nni_msg_body(*rxmsg);
			iov[0].iov_len = len;
			nni_aio_set_iov(rxaio, 1, iov);
			nng_stream_recv(p->conn, rxaio);
			nni_mtx_unlock(&p->mtx);
			return;
		}
	}
	msg = *rxmsg;
	*rxmsg = NULL;
    if ((rv = nni_mqtt_msg_proto_data_alloc(msg)) != 0) goto error;
    ((nni_mqtt_proto_data *)nni_msg_get_proto_data(msg))->quic_stream = stream ? stream->id : 0;
	type = header[0] & 0xf0;
	flags = header[0] & 0x0f;
	uint8_t   qos_pac;
	uint16_t  packet_id   = 0;
	uint8_t   reason_code = 0;
	uint8_t   ack_cmd     = 0;
	switch (type) {
	case CMD_PUBLISH:
		qos_pac = nni_msg_get_pub_qos(msg);
		if (qos_pac > 0) {
			if (qos_pac == 1) {
				ack_cmd = CMD_PUBACK;
			} else if (qos_pac == 2) {
				ack_cmd = CMD_PUBREC;
			} else {
				rv = PROTOCOL_ERROR;
				goto error;
			}
			if ((packet_id = nni_msg_get_pub_pid(msg)) == 0) {
				rv = PROTOCOL_ERROR;
				goto error;
			}
			ack = true;
		}
		break;
	case CMD_PUBREC:
		if (nni_mqtt_pubres_decode(msg, &packet_id, &reason_code, &prop,
		        p->proto) != 0) {
			rv = PROTOCOL_ERROR;
			goto error;
		}
        // A negative MQTT 5 PUBREC terminates the QoS 2 exchange.
        // Preserve it for the protocol layer and do not send PUBREL.
        if (p->proto == MQTT_PROTOCOL_VERSION_v5 && reason_code >= 0x80) {
            uint32_t key = packet_id | ((uint32_t) (stream ? stream->id : 0) << 16);
            if (nni_id_get(&p->inflight, key) != NULL) {
                nni_id_remove(&p->inflight, key);
                p->sndmax++;
            }
            break;
        }
		ack_cmd = CMD_PUBREL;
		ack     = true;
		break;
	case CMD_PUBREL:
		if (flags == 0x02) {
			if (nni_mqtt_pubres_decode(msg, &packet_id, &reason_code,
			        &prop, p->proto) != 0) {
				rv = PROTOCOL_ERROR;
				goto error;
			}
			ack_cmd = CMD_PUBCOMP;
			ack     = true;
			break;
		} else {
			rv = PROTOCOL_ERROR;
			goto error;
		}
	case CMD_PUBACK:
	case CMD_PUBCOMP:
		if (nni_mqtt_pubres_decode(
		        msg, &packet_id, &reason_code, &prop, p->proto) != 0) {
			rv = PROTOCOL_ERROR;
			goto error;
		}
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			uint32_t key = packet_id | ((uint32_t) (stream ? stream->id : 0) << 16);
            if (nni_id_get(&p->inflight, key) != NULL) {
                nni_id_remove(&p->inflight, key);
                p->sndmax++;
            }
		}
		break;
	case CMD_PINGRESP:
		//free here?
		break;
	case CMD_DISCONNECT:
		break;
	case CMD_SUBACK:
		break;
	default:
		break;
	}

	if (ack == true) {
		nni_aio *qsaio = NULL;
		bool busy;
		nni_lmq *rlmq;
		// alloc a msg here costs memory. However we must do it for the
		// sake of compatibility with nng.
		nni_msg *qmsg;
		if ((rv = nni_msg_alloc(&qmsg, 0)) != 0) {
			ack = false;
			rv  = UNSPECIFIED_ERROR;
			log_error("Memory error");
			goto error;
		}
		// TODO set reason code or property here if necessary
        // Response packets carry our success status, not the peer's reason
        // code or its diagnostic properties (e.g. PUBREC 0x10 is not a
        // valid reason for PUBREL).
		nni_mqtt_msgack_encode(qmsg, packet_id, 0, NULL, p->proto);
		nni_mqtt_pubres_header_encode(qmsg, ack_cmd);
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			property_free(prop);
			prop = NULL;
		}
		if (stream != NULL) {
			qsaio = &stream->qaio;
			rlmq = &stream->rslmq;
			busy  = stream->busy;
		} else {
			qsaio = p->qsaio;
			busy  = p->busy;
			rlmq = &p->rslmq;
		}
		if (!busy) {
		// if (!nni_aio_busy(qsaio)) {
			iov[0].iov_len = nni_msg_header_len(qmsg);
			iov[0].iov_buf = nni_msg_header(qmsg);
			iov[1].iov_len = nni_msg_len(qmsg);
			iov[1].iov_buf = nni_msg_body(qmsg);
			nni_aio_set_msg(qsaio, qmsg);
			// send ACK down...
			nni_aio_set_iov(qsaio, 2, iov);
			if (stream != NULL) {
				stream->busy = true;
			} else {
				p->busy      = true;
			}
			nng_stream_send(p->conn, qsaio);
		} else {
			if (nni_lmq_full(rlmq)) {
				// Make space for the new message.
				if (nni_lmq_cap(rlmq) <= NANO_MAX_QOS_PACKET) {
					if ((rv = nni_lmq_resize(rlmq,
					         nni_lmq_cap(rlmq) * 2)) == 0) {
						nni_lmq_put(rlmq, qmsg);
					} else {
						// memory error.
						nni_msg_free(qmsg);
						log_warn("ACK msg lost due to full lmq");
					}
				} else {
					nni_msg *old;
					(void) nni_lmq_get(rlmq, &old);
					nni_msg_free(old);
					if (nni_lmq_put(rlmq, qmsg) != 0) {
						nni_msg_free(qmsg);
						log_warn("ACK msg lost due to full lmq");
					}
				}
			} else {
				if (nni_lmq_put(rlmq, qmsg) != 0) {
					nni_msg_free(qmsg);
					log_warn("ACK msg lost due to full lmq");
				}
			}
		}
		ack = false;
	}

	mqtt_quictran_pipe_send_start(p, NULL);
	for (unsigned i = 0; i < QUIC_SUB_STREAM_NUM; i++)
		mqtt_quictran_pipe_send_start(p, &p->substreams[i]);
	property_free(prop);
	prop = NULL;
	if ((aio = nni_list_first(&p->recvq)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_set_msg(aio, msg);
		nni_aio_finish(aio, 0, nni_msg_len(msg));
	} else if (nni_lmq_put(&p->rxlmq, msg) != 0) {
		// Do not ACK and discard messages when the application is slow.
		// Stop reading this stream until pipe_recv makes room.
		*(stream ? &stream->pending_rx : &p->pending_rx) = msg;
		nni_mtx_unlock(&p->mtx);
		return;
	}
	*got = 0;
	iov[0].iov_buf = header;
	iov[0].iov_len = 2;
	nni_aio_set_iov(rxaio, 1, iov);
	nng_stream_recv(p->conn, rxaio);
	nni_mtx_unlock(&p->mtx);
	return;
error:
	property_free(prop);
	nni_msg_free(msg);
	nni_msg_free(*rxmsg);
	*rxmsg = NULL;
	nni_atomic_set_bool(&p->closed, true);
	while ((aio = nni_list_first(&p->recvq)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv ? rv : NNG_ECLOSED);
	}
	nni_mtx_unlock(&p->mtx);
	mqtt_quictran_pipe_close(p);
}


// Routing belongs to the transport, not the MQTT protocol. Keep publications
// on one topic ordered, and keep SUBSCRIBE/UNSUBSCRIBE on the same stream.
static quic_substream *
mqtt_quictran_route(mqtt_quictran_pipe *p, nni_msg *msg)
{
	unsigned selected = nng_mqtt_quic_msg_get_stream(msg);
	uint8_t type = *(uint8_t *) nni_msg_header(msg) & 0xf0;
	if (!p->multi_stream) return NULL;
	if (selected != 0) return &p->substreams[selected - 1];
	nni_mqtt_proto_data *routing = nni_msg_get_proto_data(msg);
	// NanoMQ's qos_first path uses the control stream to bypass data queues.
	// Explicit SDK stream selection takes precedence over this default route.
	if (routing != NULL && routing->quic_priority) return NULL;
	if (type == CMD_PUBLISH) {
		uint32_t len, hash = 2166136261u;
		const char *topic = nni_mqtt_msg_get_publish_topic(msg, &len);
		for (uint32_t i = 0; i < len; i++) hash = (hash ^ (uint8_t) topic[i]) * 16777619u;
		return &p->substreams[hash % 2 + 2];
	}
	if (type == CMD_SUBSCRIBE || type == CMD_UNSUBSCRIBE) return &p->substreams[0];
	return NULL;
}

static void
mqtt_quictran_pipe_send_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtt_quictran_pipe *p = arg;
	nni_mtx_lock(&p->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&p->mtx);
		return;
	}
	for (unsigned i = 0; i <= QUIC_SUB_STREAM_NUM; i++) {
		nni_list *q = i ? &p->substreams[i - 1].sendq : &p->sendq;
		nni_aio *tx = i ? &p->substreams[i - 1].saio : p->txaio;
		// The head may be waiting for MQTT Receive Maximum credit.
		if (nni_list_first(q) == aio && nni_aio_get_msg(tx) != NULL) {
			nni_aio_abort(tx, rv);
			nni_mtx_unlock(&p->mtx);
			return;
		}
	}
	quic_substream *stream = mqtt_quictran_route(p, nni_aio_get_msg(aio));
	nni_aio_list_remove(aio);
	mqtt_quictran_pipe_send_start(p, stream);
	nni_mtx_unlock(&p->mtx);
	nni_aio_finish_error(aio, rv);
}

// Each stream has an independent FIFO and internal AIO. A blocked stream
// never prevents another stream (including MQTT keepalive) from making progress.
static void
mqtt_quictran_pipe_send_start(mqtt_quictran_pipe *p, quic_substream *stream)
{
	nni_list *q = stream ? &stream->sendq : &p->sendq;
	nni_aio *tx = stream ? &stream->saio : p->txaio;
	nni_aio *aio;
	if (nni_atomic_get_bool(&p->closed) || nni_aio_get_msg(tx) != NULL) return;
	while ((aio = nni_list_first(q)) != NULL) {
		nni_msg *msg = nni_aio_get_msg(aio);
		uint8_t type = *(uint8_t *) nni_msg_header(msg) & 0xf0;
		if (p->proto == MQTT_PROTOCOL_VERSION_v5 && type == CMD_PUBLISH &&
		    nni_mqtt_msg_get_publish_qos(msg) != 0) {
			uint32_t key = nni_mqtt_msg_get_packet_id(msg) |
			    ((uint32_t) (stream ? stream->id : 0) << 16);
			if (nni_id_get(&p->inflight, key) == NULL) {
				if (p->sndmax == 0) return;
				int rv = nni_id_set(&p->inflight, key, p);
				if (rv != 0) {
					nni_aio_list_remove(aio);
					nni_aio_finish_error(aio, rv);
					continue;
				}
				p->sndmax--;
			}
		}
		int *flags = stream ? &stream->send_flags : &p->send_flags;
		*flags = stream ? stream->id : 0;
		nni_mqtt_proto_data *routing = nni_msg_get_proto_data(msg);
		if (routing != NULL && routing->quic_priority) *flags |= QUIC_HIGH_PRIOR_MSG;
		nni_iov iov[2] = {
			{ nni_msg_header(msg), nni_msg_header_len(msg) },
			{ nni_msg_body(msg), nni_msg_len(msg) }
		};
		nni_aio_set_prov_data(tx, flags);
		// Non-owning marker: the upper AIO retains the message until completion.
		nni_aio_set_msg(tx, msg);
		nni_aio_set_iov(tx, 2, iov);
		nng_stream_send(p->conn, tx);
		return;
	}
}

static void
mqtt_quictran_pipe_send(void *arg, nni_aio *aio)
{
	mqtt_quictran_pipe *p = arg;
	nni_msg *msg = nni_aio_get_msg(aio);
	int rv = 0;
	if (nni_aio_begin(aio) != 0) return;
	nni_mtx_lock(&p->mtx);
	if (nni_atomic_get_bool(&p->closed)) rv = NNG_ECLOSED;
	else if (msg == NULL || nni_msg_header_len(msg) == 0) rv = NNG_EINVAL;
	else if (nng_mqtt_quic_msg_get_stream(msg) > QUIC_SUB_STREAM_NUM) rv = NNG_EINVAL;
	else if (!p->multi_stream && nng_mqtt_quic_msg_get_stream(msg) != 0) rv = NNG_ENOTSUP;
	else if (nni_msg_header_len(msg) + nni_msg_len(msg) > p->packmax) rv = NNG_EMSGSIZE;
	else if ((*(uint8_t *) nni_msg_header(msg) & 0xf0) == CMD_PUBLISH &&
	    nni_mqtt_msg_get_publish_qos(msg) > p->qosmax) rv = NNG_EPROTO;
	if (rv == 0) rv = nni_aio_schedule(aio, mqtt_quictran_pipe_send_cancel, p);
	if (rv != 0) {
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	quic_substream *stream = mqtt_quictran_route(p, msg);
	nni_list *q = stream ? &stream->sendq : &p->sendq;
	nni_aio_list_append(q, aio);
	mqtt_quictran_pipe_send_start(p, stream);
	nni_mtx_unlock(&p->mtx);
}

static void
mqtt_quictran_pipe_recv_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtt_quictran_pipe *p = arg;
	nni_mtx_lock(&p->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&p->mtx);
}


static void
mqtt_quictran_pipe_recv(void *arg, nni_aio *aio)
{
	mqtt_quictran_pipe *p = arg;
	int                rv;
	nni_msg *msg = NULL;
	if (nni_aio_begin(aio) != 0) {
		log_error("recv canceld due to misused aio");
		return;
	}
	nni_mtx_lock(&p->mtx);
	if (nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	if (p->connack) {
		msg = p->connack;
		p->connack = NULL;
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}
	if (nni_lmq_get(&p->rxlmq, &msg) == 0) {
		for (unsigned i = 0; i <= QUIC_SUB_STREAM_NUM; i++) {
			quic_substream *stream = i ? &p->substreams[i - 1] : NULL;
			nni_msg **pending = stream ? &stream->pending_rx : &p->pending_rx;
			if (*pending == NULL) continue;
			int putrv = nni_lmq_put(&p->rxlmq, *pending);
			NNI_ASSERT(putrv == 0);
			NNI_ARG_UNUSED(putrv);
			*pending = NULL;
			nni_aio *rx = stream ? &stream->raio : p->rxaio;
			*(stream ? &stream->gotrxhead : &p->gotrxhead) = 0;
			nni_iov iov = { stream ? stream->rxlen : p->rxlen, 2 };
			nni_aio_set_iov(rx, 1, &iov);
			nng_stream_recv(p->conn, rx);
			break;
		}
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}
	if ((rv = nni_aio_schedule(aio, mqtt_quictran_pipe_recv_cancel, p)) != 0) {
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_list_append(&p->recvq, aio);

	nni_mtx_unlock(&p->mtx);
}

static uint16_t
mqtt_quictran_pipe_peer(void *arg)
{
	mqtt_quictran_pipe *p = arg;

	return (p->peer);
}

static int
mqtt_quictran_pipe_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	mqtt_quictran_pipe *p = arg;
	if (strcmp(name, NNG_OPT_MQTT_KEEPALIVE_INTERVAL) == 0)
		return nni_copyout_ms(p->keepalive, buf, szp, t);
	return (nni_stream_get(p->conn, name, buf, szp, t));
}

static void
mqtt_quictran_pipe_start(
    mqtt_quictran_pipe *p, nng_stream *conn, mqtt_quictran_ep *ep)
{
	nni_iov  iov[2];
	nni_msg *connmsg = NULL;
	uint8_t mqtt_version;
	int      niov = 0;
	int      rv;

	ep->refcnt++;

	p->conn   = conn;
	(void)nng_stream_get_bool(conn, NNG_OPT_QUIC_ENABLE_MULTISTREAM, &p->multi_stream);
	p->ep     = ep;
	p->rcvmax = NANO_MAX_PACKET_SIZE;
	p->sndmax = 65535;

	connmsg = ep->connmsg;
	if (connmsg == NULL) {
		if ((rv = nni_mqtt_msg_alloc(&connmsg, 0)) != 0) goto start_error;
		nni_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
		nni_mqtt_msg_set_connect_proto_version(connmsg, 4);
		nni_mqtt_msg_set_connect_keep_alive(connmsg, 60);
		nni_mqtt_msg_set_connect_clean_session(connmsg, true);
		ep->connmsg = connmsg;
	}
	mqtt_version = nni_mqtt_msg_get_connect_proto_version(connmsg);
	if (mqtt_version == 4) rv = nni_mqtt_msg_encode(connmsg);
	else if (mqtt_version == 5) {
		property_data *data = property_get_value(nni_mqtt_msg_get_connect_property(connmsg), MAXIMUM_PACKET_SIZE);
		if (data) p->rcvmax = data->p_value.u32;
		rv = nni_mqttv5_msg_encode(connmsg);
	} else rv = NNG_EPROTO;
	if (rv) goto start_error;
	p->keepalive = nni_mqtt_msg_get_connect_keep_alive(connmsg) * NNI_SECOND;
	p->gotrxhead  = 0;
	p->gottxhead  = 0;
	p->wantrxhead = 2;
	p->wanttxhead = nni_msg_header_len(connmsg) + nni_msg_len(connmsg);
	p->rxmsg      = NULL;
	// p->keepalive  = nni_mqtt_msg_get_connect_keep_alive(connmsg) * 1000;
	p->proto      = mqtt_version;

	if (nni_msg_header_len(connmsg) > 0) {
		iov[niov].iov_buf = nni_msg_header(connmsg);
		iov[niov].iov_len = nni_msg_header_len(connmsg);
		niov++;
	}
	if (nni_msg_len(connmsg) > 0) {
		iov[niov].iov_buf = nni_msg_body(connmsg);
		iov[niov].iov_len = nni_msg_len(connmsg);
		niov++;
	}
	nni_aio_set_iov(p->negoaio, niov, iov);
	nni_list_append(&ep->negopipes, p);

	nni_aio_set_timeout(p->negoaio, 10000); // 10 sec timeout to negotiate
	nng_stream_send(p->conn, p->negoaio);
	return;
start_error:
	if (ep->useraio) {
		nni_aio_finish_error(ep->useraio, rv);
		ep->useraio = NULL;
	}
	mqtt_quictran_pipe_reap(p);
}

static void
mqtt_quictran_ep_fini(void *arg)
{
	mqtt_quictran_ep *ep = arg;

	nni_mtx_lock(&ep->mtx);
	ep->fini = true;
	if (ep->refcnt != 0) {
		// Keep nng_fini's reap drain aware of negotiating pipes as well.
		nni_reap(&quictran_ep_reap_list, ep);
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	nni_mtx_unlock(&ep->mtx);
	// Free connmsg once
	if (ep->connmsg)
		nni_msg_free(ep->connmsg);

	nni_aio_stop(ep->timeaio);
	nni_aio_stop(ep->connaio);
	nng_stream_dialer_free(ep->dialer);
	nng_stream_listener_free(ep->listener);
	nni_aio_free(ep->timeaio);
	nni_aio_free(ep->connaio);
	property_free(ep->property);

	nni_mtx_fini(&ep->mtx);
	NNI_FREE_STRUCT(ep);
}

static void
mqtt_quictran_ep_close(void *arg)
{
	mqtt_quictran_ep *  ep = arg;
	mqtt_quictran_pipe *p;

	nni_mtx_lock(&ep->mtx);

	ep->closed = true;
	nni_aio_close(ep->timeaio);
	if (ep->dialer != NULL) {
		nng_stream_dialer_close(ep->dialer);
	}
	if (ep->listener != NULL) {
		nng_stream_listener_close(ep->listener);
	}
	NNI_LIST_FOREACH (&ep->negopipes, p) {
		mqtt_quictran_pipe_close(p);
	}
	NNI_LIST_FOREACH (&ep->waitpipes, p) {
		mqtt_quictran_pipe_close(p);
	}
	NNI_LIST_FOREACH (&ep->busypipes, p) {
		mqtt_quictran_pipe_close(p);
	}
	if (ep->useraio != NULL) {
		nni_aio_finish_error(ep->useraio, NNG_ECLOSED);
		ep->useraio = NULL;
	}

	nni_mtx_unlock(&ep->mtx);
}

static void
mqtt_quictran_dial_cb(void *arg)
{
	mqtt_quictran_ep *  ep  = arg;
	nni_aio *           aio = ep->connaio;
	mqtt_quictran_pipe *p;
	int                 rv;
	nng_stream *        conn;

	if ((rv = nni_aio_result(aio)) != 0) {
		log_error("Dial failed %d", rv);
		goto error;
	}

	conn = nni_aio_get_output(aio, 0);
	if ((rv = mqtt_quictran_pipe_alloc(&p)) != 0) {
		nng_stream_free(conn);
		goto error;
	}
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		mqtt_quictran_pipe_fini(p);
		nng_stream_free(conn);
		rv = NNG_ECLOSED;
		nni_mtx_unlock(&ep->mtx);
		goto error;
	} else {
		mqtt_quictran_pipe_start(p, conn, ep);
	}
	nni_mtx_unlock(&ep->mtx);
	return;

error:
	// Error connecting.  We need to pass this straight back
	// to the user.
	nni_mtx_lock(&ep->mtx);
	if ((aio = ep->useraio) != NULL) {
		ep->useraio = NULL;
		log_error("Dialer useraio cb rv%d", rv);
		nni_aio_finish_error(aio, rv);
	} else {
		log_error("Null useraio in ep");
	}
	nni_mtx_unlock(&ep->mtx);
}

static int
mqtt_quictran_ep_init(mqtt_quictran_ep **epp, nng_url *url, nni_sock *sock)
{
	mqtt_quictran_ep *ep;

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&ep->mtx);
	NNI_LIST_INIT(&ep->busypipes, mqtt_quictran_pipe, node);
	NNI_LIST_INIT(&ep->waitpipes, mqtt_quictran_pipe, node);
	NNI_LIST_INIT(&ep->negopipes, mqtt_quictran_pipe, node);

	ep->proto       = nni_sock_proto_id(sock);
	ep->nsock       = sock;
	ep->url         = url;
	ep->connmsg     = NULL;
	ep->reason_code = 0;
	ep->property    = NULL;
	ep->backoff     = 0;

	*epp = ep;
	return (0);
}

static int
mqtt_quictran_dialer_init(void **dp, nng_url *url, nni_dialer *ndialer)
{
	mqtt_quictran_ep *ep;
	int              rv;
	// nng_sockaddr     srcsa;
	nni_sock *       sock = nni_dialer_sock(ndialer);

	// Check for invalid URL components. only one dialer is allowed
	if ((strlen(url->u_path) != 0) && (strcmp(url->u_path, "/") != 0)) {
		return (NNG_EADDRINVAL);
	}
	if ((url->u_fragment != NULL) || (url->u_userinfo != NULL) ||
	    (url->u_query != NULL) || (strlen(url->u_hostname) == 0) ||
	    (strlen(url->u_port) == 0)) {
		return (NNG_EADDRINVAL);
	}

	if ((rv = mqtt_quictran_ep_init(&ep, url, sock)) != 0) {
		return (rv);
	}
	ep->ndialer = ndialer;

	if ((rv != 0) ||
	    ((rv = nni_aio_alloc(&ep->connaio, mqtt_quictran_dial_cb, ep)) !=
	        0) ||
	    ((rv = nng_stream_dialer_alloc_url(&ep->dialer, url)) != 0)) {
		mqtt_quictran_ep_fini(ep);
		return (rv);
	}
	// TODO srcsa will be got from nni quic dialer option. Which is not supported yet.
	//if ((srcsa.s_family != NNG_AF_UNSPEC) &&
	//    ((rv = nni_stream_dialer_set(ep->dialer, NNG_OPT_LOCADDR, &srcsa,
	//          sizeof(srcsa), NNI_TYPE_SOCKADDR)) != 0)) {
	//	mqtt_quictran_ep_fini(ep);
	//	return (rv);
	//}
	*dp = ep;
	return (0);
}

static void
mqtt_quictran_ep_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtt_quictran_ep *ep = arg;
	nni_mtx_lock(&ep->mtx);
	if (ep->useraio == aio) {
		ep->useraio = NULL;
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ep->mtx);
}

// called by dialer
static void
mqtt_quictran_ep_connect(void *arg, nni_aio *aio)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;


	if ((rv = nni_aio_begin(aio)) != 0) {
		log_error("ep connect rv %d", rv);
		return;
	}
	if (ep->closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		nni_mtx_unlock(&ep->mtx);
		log_error("ep clsoed");
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->useraio != NULL) {
		log_error("ep busy");
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_EBUSY);
		return;
	}
	if ((rv = nni_aio_schedule(aio, mqtt_quictran_ep_cancel, ep)) != 0) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	ep->useraio = aio;

	nng_stream_dialer_dial(ep->dialer, ep->connaio);
	nni_mtx_unlock(&ep->mtx);
}

static int
mqtt_quictran_ep_get_url(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	char *           s;
	int              rv;
	int              port = 0;

	if (ep->listener != NULL) {
		(void) nng_stream_listener_get_int(
		    ep->listener, NNG_OPT_TCP_BOUND_PORT, &port);
	}

	if ((rv = nni_url_asprintf_port(&s, ep->url, port)) == 0) {
		rv = nni_copyout_str(s, v, szp, t);
		nni_strfree(s);
	}
	return (rv);
}

static int
mqtt_quictran_ep_get_reasoncode(void *arg, void *v, size_t *sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
	mqtt_quictran_ep *ep = arg;
	int              rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_copyout_int(ep->reason_code, v, sz, t);
	nni_mtx_unlock(&ep->mtx);
	return (rv);
}

static int
mqtt_quictran_ep_get_connmsg(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	rv = nni_copyout_ptr(ep->connmsg, v, szp, t);

	return (rv);
}

static int
mqtt_quictran_ep_get_property(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	rv = nni_copyout_ptr(ep->property, v, szp, t);
	return (rv);
}

static int
mqtt_quictran_ep_set_connmsg(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	nni_msg *msg;
	int rv = nni_copyin_ptr((void **)&msg, v, sz, t);
	if (rv) return rv;
	nni_mtx_lock(&ep->mtx);
	if (ep->refcnt || ep->useraio) rv = NNG_ESTATE;
	else if (msg != ep->connmsg) {
		nni_msg_free(ep->connmsg);
		ep->connmsg = msg;
	}
	nni_mtx_unlock(&ep->mtx);
	return rv;
}

// NanoSDK use exponential backoff strategy as default
// Backoff for random time that exponentially curving
static int
mqtt_quictran_ep_set_reconnect_backoff(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	nni_duration    tmp;
	int rv;

	// max backoff time cannot exceed 10min

	if ((rv = nni_copyin_ms(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&ep->mtx);
		// only allow 360s as max value
		ep->backoff_max = tmp > 600000 ? 360000 : tmp;
		nni_mtx_unlock(&ep->mtx);
	}
	return (rv);
}


static int
mqtt_quictran_ep_set_priority(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	return nni_stream_dialer_set(ep->dialer, NNG_OPT_QUIC_PRIORITY, v, sz, t);
}


static nni_sp_pipe_ops mqtt_quictran_pipe_ops = {
	.p_init   = mqtt_quictran_pipe_init,
	.p_fini   = mqtt_quictran_pipe_fini,
	.p_stop   = mqtt_quictran_pipe_stop,
	.p_send   = mqtt_quictran_pipe_send,
	.p_recv   = mqtt_quictran_pipe_recv,
	.p_close  = mqtt_quictran_pipe_close,
	.p_peer   = mqtt_quictran_pipe_peer,
	.p_getopt = mqtt_quictran_pipe_getopt,
};

static const nni_option mqtt_quictran_ep_opts[] = {
	{
	    .o_name = NNG_OPT_MQTT_CONNECT_REASON,
	    .o_get  = mqtt_quictran_ep_get_reasoncode,
	},
	{
	    .o_name = NNG_OPT_MQTT_CONNECT_PROPERTY,
	    .o_get  = mqtt_quictran_ep_get_property,
	},
	{
	    .o_name = NNG_OPT_MQTT_CONNMSG,
	    .o_get  = mqtt_quictran_ep_get_connmsg,
	    .o_set  = mqtt_quictran_ep_set_connmsg,
	},
	{
	    .o_name = NNG_OPT_MQTT_RECONNECT_BACKOFF_MAX,
	    .o_set  = mqtt_quictran_ep_set_reconnect_backoff,
	},
	{
	    .o_name = NNG_OPT_URL,
	    .o_get  = mqtt_quictran_ep_get_url,
	},
	{
	    .o_name = NNG_OPT_MQTT_QUIC_PRIORITY,
	    .o_set  = mqtt_quictran_ep_set_priority,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static int
mqtt_quictran_dialer_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	rv = nni_stream_dialer_get(ep->dialer, name, buf, szp, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_getopt(mqtt_quictran_ep_opts, name, ep, buf, szp, t);
	}
	return (rv);
}

static int
mqtt_quictran_dialer_setopt(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	rv = nni_stream_dialer_set(ep->dialer, name, buf, sz, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_setopt(mqtt_quictran_ep_opts, name, ep, buf, sz, t);
	}
	return (rv);
}

static nni_sp_dialer_ops mqtt_quictran_dialer_ops = {
	.d_init    = mqtt_quictran_dialer_init,
	.d_fini    = mqtt_quictran_ep_fini,
	.d_connect = mqtt_quictran_ep_connect,
	.d_close   = mqtt_quictran_ep_close,
	.d_getopt  = mqtt_quictran_dialer_getopt,
	.d_setopt  = mqtt_quictran_dialer_setopt,
};


static nni_sp_tran mqtt_quic_tran = {
	.tran_scheme   = "mqtt-quic",
	.tran_dialer   = &mqtt_quictran_dialer_ops,
	.tran_listener = NULL,
	// .tran_listener = &mqtt_quictran_listener_ops,
	.tran_pipe     = &mqtt_quictran_pipe_ops,
	.tran_init     = mqtt_quictran_init,
	.tran_fini     = mqtt_quictran_fini,
};

#ifndef NNG_ELIDE_DEPRECATED
int
nng_mqtt_quic_register(void)
{
	return (nni_init());
}
#endif

void
nni_mqtt_quic_register(void)
{
	nni_mqtt_tran_register(&mqtt_quic_tran);
}
