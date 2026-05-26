//
// Copyright 2024 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "core/sockimpl.h"
#include "nng/mqtt/mqtt_client.h"
#include "supplemental/mqtt/mqtt_msg.h"
// #include "nng/protocol/mqtt/mqtt_parser.h"
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
	uint16_t			 level;
	uint16_t			 id;	// To command stream layer
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
	nni_pipe *       npipe;
	nni_list_node    node;
	mqtt_quictran_ep *ep;
	nni_atomic_flag  reaped;
	nni_reap_node    reap;
	uint32_t         packmax; // MQTT Maximum Packet Size (Max length)
	uint16_t         peer;    // broker info
	uint16_t         proto;   // MQTT version
	uint16_t         keepalive;
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
	nni_lmq          rslmq;	// resend lmq
	nni_lmq          rxlmq; // recv lmq, shared by all streams
	nni_mtx          mtx;
	nni_atomic_bool  closed;
	bool             busy;
#ifdef NNG_HAVE_MQTT_BROKER
	nni_msg         *connack;
	conn_param *     cparam;
#endif
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
static void mqtt_quictran_pipe_send_start(mqtt_quictran_pipe *);
static void mqtt_quictran_subpipe_send_cb(void *);
static void mqtt_quictran_subpipe_recv_cb(void *);
static void mqtt_quictran_pipe_send_cb(void *);
static void mqtt_quictran_pipe_recv_cb(void *);
static void mqtt_quictran_subpipe_qos_cb(void *);
static void mqtt_quictran_pipe_qos_send_cb(void *);
static void mqtt_quictran_share_qos_send_cb(void *, nni_aio *, quic_substream *);

static void mqtt_quictran_pipe_recv_start(mqtt_quictran_pipe *, nni_aio *);
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

static void
mqtt_quictran_pipe_close(void *arg)
{
	mqtt_quictran_pipe *p = arg;

	nng_stream_close(p->conn);
	nni_atomic_set_bool(&p->closed, true);
	nni_mtx_lock(&p->mtx);
	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		quic_substream *stream = &p->substreams[i];
		nni_aio_close(&stream->raio);
		nni_aio_close(&stream->saio);
		nni_aio_close(&stream->qaio);
	}
	nni_mtx_unlock(&p->mtx);

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
	nni_lmq_init(&p->rslmq, NNG_MAX_SEND_LMQ);
	nni_lmq_init(&p->rxlmq, NNG_MAX_RECV_LMQ);
	p->busy = false;
	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, false);
	// set max value by default
	p->packmax == 0 ? p->packmax = (uint32_t)0xFFFFFFFF : p->packmax;
	p->qosmax  == 0 ? p->qosmax  = 2 : p->qosmax;
	p->keepalive = 0;
	return (0);
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
		if (ep->fini && (ep->refcnt == 0)) {
			nni_reap(&quictran_ep_reap_list, ep);
		}
		nni_mtx_unlock(&ep->mtx);
	}

	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		quic_substream *stream = &p->substreams[i];
		nni_lmq_flush(&stream->rslmq);
		nni_lmq_fini(&stream->rslmq);
		nni_mtx_fini(&stream->mtx);
	}
	nng_stream_free(p->conn);
	nni_aio_free(p->rxaio);
	nni_aio_free(p->txaio);
	nni_aio_free(p->qsaio);
	nni_aio_free(p->negoaio);
	nni_aio_free(p->rpaio);
	nni_msg_free(p->rxmsg);
	nni_lmq_flush(&p->rxlmq);
	nni_lmq_fini(&p->rxlmq);
	nni_lmq_flush(&p->rslmq);
	nni_lmq_fini(&p->rslmq);
	nni_mtx_fini(&p->mtx);
#ifdef NNG_HAVE_MQTT_BROKER
	conn_param_free(p->cparam);
#endif
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
	if (((rv = nni_aio_alloc(&p->txaio, mqtt_quictran_pipe_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->rxaio, mqtt_quictran_pipe_recv_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->qsaio, mqtt_quictran_pipe_qos_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->rpaio, mqtt_quictran_pipe_rp_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->negoaio, mqtt_quictran_pipe_nego_cb, p)) != 0)) {
		mqtt_quictran_pipe_fini(p);
		return (rv);
	}

	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		quic_substream *stream = &p->substreams[i];
		stream->pipe = p;
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
#ifdef NNG_HAVE_MQTT_BROKER
	if (p->cparam == NULL) {
		p->cparam = nni_get_conn_param_from_msg(ep->connmsg);
		if (p->keepalive != 0)
			p->cparam->keepalive_mqtt = p->keepalive;
		nni_msg_set_conn_param(ep->connmsg, p->cparam);
	}
#endif
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
	// We start transmitting before we receive.
	if (p->gottxhead < p->wanttxhead) {
		p->gottxhead += nni_aio_count(aio);
	} else if (p->gotrxhead < p->wantrxhead) {
		p->gotrxhead += nni_aio_count(aio);
	}

	if (p->gottxhead < p->wanttxhead) {
		nni_iov iov;
		iov.iov_len = p->wanttxhead - p->gottxhead;
		iov.iov_buf = &p->txlen[p->gottxhead];
		// send it down...
		nni_aio_set_iov(aio, 1, &iov);
		nng_stream_send(p->conn, aio);	// only main pipe
		nni_mtx_unlock(&ep->mtx);
		return;
	}

	// receving fixed header
	if (p->gotrxhead == 0 ||
	    (p->gotrxhead <= 5 && p->rxlen[p->gotrxhead - 1] > 0x7f &&
	        p->rxmsg == NULL)) {
		nni_iov iov;
		iov.iov_buf = &p->rxlen[p->gotrxhead];
		if (p->gotrxhead == 0) {
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
	// TODO MQTT V5 Auth
	if ((p->rxlen[0] & CMD_CONNACK) != CMD_CONNACK) {
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
		iov.iov_buf = nni_msg_body(p->rxmsg);
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
			if (property_dup((property **) &ep->property, prop) != 0)
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
					p->keepalive = data->p_value.u16;
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
	}
#ifdef NNG_HAVE_MQTT_BROKER
	nni_msg_clone(p->rxmsg);
	p->connack = p->rxmsg;
#endif


mqtt_error:
	// We are ready now.  We put this in the wait list, and
	// then try to run the matcher.
	nni_list_remove(&ep->negopipes, p);
	nni_list_append(&ep->waitpipes, p);

	nni_msg_free(p->rxmsg);
	p->rxmsg = NULL;

	if (rv == MQTT_SUCCESS) {
		// TODO pass reason code of CONNACK to upper layer
		mqtt_quictran_ep_match(ep);
	} else {
		// Fail but still match to let user know ack has arrived
		mqtt_quictran_ep_match(ep);
		// send DISCONNECT
		nni_iov iov;
		p->txlen[0] = CMD_DISCONNECT;
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			p->txlen[1] = 0x02;
			p->txlen[2] = ep->reason_code;
			p->txlen[3] = 0; // length of property
			iov.iov_len = 4;
		} else {
			p->txlen[1] = 0x00;
			iov.iov_len = 2;
		}
		iov.iov_buf = p->txlen;
		nni_aio_set_iov(p->rpaio, 1, &iov);
		nng_stream_send(p->conn, p->rpaio);
	}
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
mqtt_quictran_share_qos_send_cb(void *arg, nni_aio *qsaio, quic_substream *stream)
{
	mqtt_quictran_pipe *p = arg;
	nni_mtx            *mtx;
	nni_msg            *msg;
	size_t              n;
	int                 rv;

	msg = nni_aio_get_msg(qsaio);
	if (nni_atomic_get_bool(&p->closed)) {
		if (msg != NULL)
			nni_msg_free(msg);
		return;
	}
	stream == NULL ? (mtx = &p->mtx) : (mtx = &stream->mtx);
	if ((rv = nni_aio_result(qsaio)) != 0) {
		log_warn("qos send aio error %s", nng_strerror(rv));
		if (msg != NULL)
			nni_msg_free(msg);
		nni_aio_set_msg(qsaio, NULL);
		if (stream == NULL) {
			
		} else {
			log_info("Send ACK on sub stream failed %d", rv);
			stream->busy = false;
		}
		return;
	}

	nni_mtx_lock(mtx);
	n = nni_aio_count(qsaio);
	nni_aio_iov_advance(qsaio, n);

	// more bytes to send
	if (nni_aio_iov_count(qsaio) > 0) {
		nng_stream_send(p->conn, qsaio);
		nni_mtx_unlock(mtx);
		return;
	}
	if (msg != NULL) {
		nni_msg_free(msg);
	} else {
		log_warn("NULL msg detected in send_cb");
		if (stream == NULL) {
			nni_mtx_unlock(mtx);
			nng_stream_close(p->conn);
		} else {
			log_info("Send NULL msg on sub stream");
			stream->busy = false;
			nni_mtx_unlock(mtx);
		}
		return;
	}
	nni_aio_set_msg(qsaio, NULL);
	nni_lmq *lmq;
	stream == NULL? (lmq = &p->rslmq) : (lmq = &stream->rslmq);
	if (nni_lmq_get(lmq, &msg) == 0) {
		nni_iov iov[2];
		iov[0].iov_len = nni_msg_header_len(msg);
		iov[0].iov_buf = nni_msg_header(msg);
		iov[1].iov_len = nni_msg_len(msg);
		iov[1].iov_buf = nni_msg_body(msg);
		p->busy        = true;
		nni_aio_set_msg(qsaio, msg);
		// send ACK down...
		nni_aio_set_iov(qsaio, 2, iov);
		nng_stream_send(p->conn, qsaio);
		nni_mtx_unlock(mtx);
		return;
	}
	if (stream != NULL)
		stream->busy = false;
	else
		p->busy = false;
	nni_aio_set_msg(qsaio, NULL);
	nni_mtx_unlock(mtx);
	return;
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
	NNI_ARG_UNUSED(stream);

	nni_mtx_lock(&p->mtx);
	aio = nni_list_first(&p->sendq);

	log_trace(" ############ mqtt_quictran_pipe_send_cb [%p] ############ ", p);
	if ((rv = nni_aio_result(txaio)) != 0) {
		log_warn(" send aio error %s", nng_strerror(rv));
		nni_aio_list_remove(aio);
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

	nni_aio_list_remove(aio);
	mqtt_quictran_pipe_send_start(p);
	msg = nni_aio_get_msg(aio);

	if (msg != NULL) {
		n = nni_msg_len(msg);
#ifdef NNG_ENABLE_STATS
		// nni_pipe_bump_tx(p->npipe, n);
		nni_sock_bump_tx(p->ep->nsock, n);
#endif
		nni_msg_free(msg);
	}
	nni_mtx_unlock(&p->mtx);

	nni_aio_set_msg(aio, NULL);
	if (stream)
		stream->busy = false;
	else
		p->busy = false;
	nni_aio_finish_sync(aio, rv, n);
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
	nni_aio *          aio;
	nni_iov            iov[2];
	uint8_t            type, pos, flags;
	uint32_t           len = 0, rv;
	size_t             n;
	nni_msg *          msg;
	mqtt_quictran_pipe *p    = arg;
	bool               ack   = false;
	uint8_t            *rxlen;
	size_t			   *gotrxhead, *wantrxhead;

	nni_mtx_lock(&p->mtx);
	log_debug(" ###### recv cb of %p stream %p ###### ", rxaio, stream);
	aio = nni_list_first(&p->recvq);
	if ((rv = nni_aio_result(rxaio)) != 0) {
		log_info("aio error result %s stream %p", nng_strerror(rv), stream);
		if (stream == NULL) {
			// set close flag to prevent infinit stream recv
			nni_atomic_set_bool(&p->closed, true);
			// NNI_ASSERT(rv == NNG_ECANCELED);
			rv = NNG_ECONNABORTED;
		}
		goto recv_error;
	}
	if (nni_atomic_get_bool(&p->closed)) {
		goto recv_error;
	}
	uint16_t *id;
	if (stream != NULL) {
		rxlen = stream->rxlen;
		gotrxhead  = &stream->gotrxhead;
		wantrxhead = &stream->wantrxhead;
		id = (uint16_t *)nni_aio_get_prov_data(rxaio);
		if (*id != (stream->id))
			log_error("Unexpected Stream IO Received!");
	} else {
		rxlen = p->rxlen;
		gotrxhead  = &p->gotrxhead;
		wantrxhead = &p->wantrxhead;
	}
	n = nni_aio_count(rxaio);
	*gotrxhead += n;

	nni_aio_iov_advance(rxaio, n);
	if (nni_aio_iov_count(rxaio) > 0) {
		nng_stream_recv(p->conn, rxaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	rv = mqtt_get_remaining_length(rxlen, *gotrxhead, &len, &pos);
	*wantrxhead = len + 1 + pos;
	if (*gotrxhead <= 5 && rxlen[*gotrxhead - 1] > 0x7f) {
		if (*gotrxhead == NNI_NANO_MAX_HEADER_SIZE) {
			rv = PACKET_TOO_LARGE;
			goto recv_error;
		}
		// same packet, continue receving next byte of remaining length
		iov[0].iov_buf = &rxlen[*gotrxhead];
		iov[0].iov_len = 1;
		nni_aio_set_iov(rxaio, 1, iov);
		nng_stream_recv(p->conn, rxaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	// fixed header finished
	if (NULL == *rxmsg) {
		// Make sure the message payload is not too big.  If it is
		// the caller will shut down the pipe.
		if ((len > p->rcvmax) && (p->rcvmax > 0)) {
			rv = PACKET_TOO_LARGE;
			goto recv_error;
		}

		if ((rv = nni_msg_alloc(rxmsg, (size_t) len)) != 0) {
			rv = UNSPECIFIED_ERROR;
			goto recv_error;
		}

		// Submit the rest of the data for a read -- seperate Fixed
		// header with variable header and so on
		//  we want to read the entire message now.
		if (len != 0) {
			iov[0].iov_buf = nni_msg_body(*rxmsg);
			iov[0].iov_len = (size_t) len;

			nni_aio_set_iov(rxaio, 1, iov);
			// second recv action
			nng_stream_recv(p->conn, rxaio);
			nni_mtx_unlock(&p->mtx);
			return;
		}
	}
	// We read a message completely.  Let the user know the good news. use
	// as application message callback of users
	if (aio)
		nni_aio_list_remove(aio);
	nni_msg_header_append(*rxmsg, rxlen, pos + 1);
	msg      = *rxmsg;
	*rxmsg = NULL;
	n        = nni_msg_len(msg);
	type     = rxlen[0] & 0xf0;
	flags    = rxlen[0] & 0x0f;

	// set the payload pointer of msg according to packet_type
	uint8_t   qos_pac;
	uint16_t  packet_id   = 0;
	uint8_t   reason_code = 0;
	property *prop        = NULL;
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
				goto recv_error;
			}
			if ((packet_id = nni_msg_get_pub_pid(msg)) == 0) {
				rv = PROTOCOL_ERROR;
				goto recv_error;
			}
			ack = true;
		}
		break;
	case CMD_PUBREC:
		if (nni_mqtt_pubres_decode(msg, &packet_id, &reason_code, &prop,
		        p->proto) != 0) {
			rv = PROTOCOL_ERROR;
			goto recv_error;
		}
		ack_cmd = CMD_PUBREL;
		ack     = true;
		break;
	case CMD_PUBREL:
		if (flags == 0x02) {
			if (nni_mqtt_pubres_decode(msg, &packet_id, &reason_code,
			        &prop, p->proto) != 0) {
				rv = PROTOCOL_ERROR;
				goto recv_error;
			}
			ack_cmd = CMD_PUBCOMP;
			ack     = true;
			break;
		} else {
			rv = PROTOCOL_ERROR;
			goto recv_error;
		}
	case CMD_PUBACK:
	case CMD_PUBCOMP:
		if (nni_mqtt_pubres_decode(
		        msg, &packet_id, &reason_code, &prop, p->proto) != 0) {
			rv = PROTOCOL_ERROR;
			goto recv_error;
		}
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			p->sndmax++;
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
			goto recv_error;
		}
		// TODO set reason code or property here if necessary
		nni_mqtt_msgack_encode(
		    qmsg, packet_id, reason_code, prop, p->proto);
		nni_mqtt_pubres_header_encode(qmsg, ack_cmd);
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			property_free(prop);
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

	if (stream == NULL) {
		// only main stream keep connection & Schedule next receive at here
		if (!nni_list_empty(&p->recvq)) {
			mqtt_quictran_pipe_recv_start(p, aio);
		}
	}
#ifdef NNG_HAVE_MQTT_BROKER
	nni_msg_set_conn_param(msg, p->cparam);
#endif
	if (aio)
		nni_aio_set_msg(aio, msg);
	if (aio) {
		log_trace("return msg type %d", type);
		if (stream != NULL) {
			nni_aio_set_prov_data(aio, &stream->id);
		} else {
			nni_aio_set_prov_data(aio, NULL);
		}
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_sync(aio, 0, n);
#ifdef NNG_ENABLE_STATS
		// nni_pipe_bump_rx(p->npipe, n);
		nni_sock_bump_rx(p->ep->nsock, n);
#endif
		return;
	} else if (msg != NULL) {
		// protocol layer is not ready yet.
		if (nni_lmq_full(&p->rxlmq)) {
			nni_msg_free(msg);
			// lost counter in stats msg_recv_drop
			log_warn("msg type %d from substream lost!", type);
		} else {
			log_debug("cache msg from substream first");
			if (nni_lmq_put(&p->rxlmq, msg) != 0) {
				nni_msg_free(msg);
			}
		}
		nni_iov sub_iov;
		if (stream) {
			stream->gotrxhead  = 0;
			stream->wantrxhead = 2;
			sub_iov.iov_buf    = stream->rxlen;
			memset(stream->rxlen, '\0',
			    sizeof(uint64_t) * sizeof(stream->rxlen[0]));
		} else {
			p->gotrxhead    = 0;
			p->wantrxhead   = 2;
			sub_iov.iov_buf = p->rxlen;
			memset(p->rxlen, '\0',
			    sizeof(uint64_t) * sizeof(p->rxlen[0]));
		}
		sub_iov.iov_len    = 2;
		nni_aio_set_iov(rxaio, 1, &sub_iov);
		log_debug("start recv on aio %p", rxaio);
		nng_stream_recv(p->conn, rxaio);
	} else {
		log_error("NULL msg received?");
	}
	nni_mtx_unlock(&p->mtx);
	return;

recv_error:
	if (aio)
		nni_aio_list_remove(aio);
	else if (!nni_atomic_get_bool(&p->closed)) {
		nni_iov sub_iov;
		if (stream) {
			// nni_msleep(500);
			stream->gotrxhead  = 0;
			stream->wantrxhead = 2;
			sub_iov.iov_buf    = stream->rxlen;
			sub_iov.iov_len = 2;
			nni_aio_set_iov(rxaio, 1, &sub_iov);

			log_info("sub stream %d canceled, keep receving on aio %p",
					  stream->id, rxaio);
			nng_stream_recv(p->conn, rxaio);
		}
	}
	msg      = *rxmsg;
	*rxmsg = NULL;
	nni_mtx_unlock(&p->mtx);

	nni_msg_free(msg);
	if (aio)
		nni_aio_finish_error(aio, rv);
	else if (stream == NULL) {
		log_info("Main stream aborted! Connection closed!");
		mqtt_quictran_pipe_close(p);
	}
}

static void
mqtt_quictran_pipe_send_prior(mqtt_quictran_pipe *p, nni_aio *aio)
{
	nni_msg *msg;
	int      niov;
	nni_iov  iov[3];

	if (nni_atomic_get_bool(&p->closed)) {
		nni_aio_finish_error(aio, SERVER_SHUTTING_DOWN);
		return;
	}

	// This runs to send the message.
	msg = nni_aio_get_msg(aio);

	if (msg != NULL && p->proto == MQTT_PROTOCOL_VERSION_v5) {
		uint8_t *header = nni_msg_header(msg);
		if ((*header & 0XF0) == CMD_PUBLISH) {
			// check max qos
			uint8_t qos = nni_mqtt_msg_get_publish_qos(msg);
			if (qos > 0)
				p->sndmax --;
			if (qos > p->qosmax) {
				p->qosmax == 1 ? ((*header &= 0XF9), (*header |= 0X02)) : NNI_ARG_UNUSED(*header);
				p->qosmax == 0? *header &= 0XF9: NNI_ARG_UNUSED(*header);
			}
		}
	}

	// check max packet size for v311 and v5
	if (nni_msg_header_len(msg) + nni_msg_len(msg) > p->packmax) {
		nni_aio_finish_error(aio, UNSPECIFIED_ERROR);
		return;
	}

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
	nni_aio_set_iov(aio, niov, iov);
	// For now, we take main stream for high priority msg
	// int *flag = nni_aio_get_prov_data(aio);
	// *flag |= p->substreams[nni_random()%2].id;
	// nni_aio_set_prov_data(aio, flag);
	nng_stream_send(p->conn, aio);
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
	// If this is being sent, then cancel the pending transfer.
	// The callback on the txaio will cause the user aio to
	// be canceled too.
	if (nni_list_first(&p->sendq) == aio) {
		nni_aio_abort(p->txaio, rv);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&p->mtx);

	nni_aio_finish_error(aio, rv);
}

static void
mqtt_quictran_pipe_send_start(mqtt_quictran_pipe *p)
{
	nni_aio *aio;
	nni_aio *txaio;
	nni_msg *msg;
	int      niov;
	nni_iov  iov[3];

	if (nni_atomic_get_bool(&p->closed)) {
		while ((aio = nni_list_first(&p->sendq)) != NULL) {
			nni_list_remove(&p->sendq, aio);
			nni_aio_finish_error(aio, SERVER_SHUTTING_DOWN);
		}
		return;
	}

	if ((aio = nni_list_first(&p->sendq)) == NULL) {
		return;
	}

	// This runs to send the message.
	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		log_error("ERROR: sending null msg! Please report issue");
		nni_aio_list_remove(aio);
		return;
	}
	if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
		uint8_t *header = nni_msg_header(msg);
		if ((*header & 0XF0) == CMD_PUBLISH) {
			// check max qos
			uint8_t qos = nni_mqtt_msg_get_publish_qos(msg);
			if (qos > 0)
				p->sndmax --;
			if (qos > p->qosmax) {
				p->qosmax == 1 ? ((*header &= 0XF9), (*header |= 0X02)) : NNI_ARG_UNUSED(*header);
				p->qosmax == 0? *header &= 0XF9: NNI_ARG_UNUSED(*header);
			}
		}
	}
	// 2 data stream with low priority 2 sub stream 
	if (nni_msg_get_type(msg) == CMD_PUBLISH) {
		txaio = &p->substreams[nni_random()%2 + 2].saio;
		log_debug("send on sub aio %p", txaio);
	} else if (nni_msg_get_type(msg) == CMD_SUBSCRIBE) {
		txaio = &p->substreams[nni_random()%2].saio;
		log_debug("send on sub aio %p", txaio);
	} else {
		txaio = p->txaio;
		log_debug("send on main aio %p", txaio);
	}
	// check max packet size for v311 and v5
	if (nni_msg_header_len(msg) + nni_msg_len(msg) > p->packmax) {
		nni_aio_finish_error(txaio, UNSPECIFIED_ERROR);
		return;
	}

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
}

static void
mqtt_quictran_pipe_send(void *arg, nni_aio *aio)
{
	mqtt_quictran_pipe *p = arg;
	int                rv;

	if (nni_atomic_get_bool(&p->closed)) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_mtx_lock(&p->mtx);
	// Priority msg
	int *flags = nni_aio_get_prov_data(aio);
	if (flags && (*flags & QUIC_HIGH_PRIOR_MSG)) {
		// This is user aio from app layer
		mqtt_quictran_pipe_send_prior(p, aio);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	if (nni_aio_begin(aio) != 0) {
		nni_mtx_unlock(&p->mtx);
		return;
	}

	if ((rv = nni_aio_schedule(aio, mqtt_quictran_pipe_send_cancel, p)) != 0) {
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}

	nni_list_append(&p->sendq, aio);
	if (nni_list_first(&p->sendq) == aio) {
		mqtt_quictran_pipe_send_start(p);
	}
	nni_mtx_unlock(&p->mtx);
}

static void
mqtt_quictran_pipe_recv_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtt_quictran_pipe *p = arg;

	log_warn("mqtt_quictran_pipe_recv_cancel triggered!");
	nni_mtx_lock(&p->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&p->mtx);
		return;
	}

	if (nni_list_first(&p->recvq) == aio) {
		nni_aio_abort(p->rxaio, rv);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&p->mtx);
	nni_aio_finish_error(aio, rv);
}

static void
mqtt_quictran_pipe_recv_start(mqtt_quictran_pipe *p, nni_aio *aio)
{
	nni_iov  iov;

	if (nni_atomic_get_bool(&p->closed)) {
		log_warn("recv canceled due to disconnect!");
		while ((aio = nni_list_first(&p->recvq)) != NULL) {
			nni_list_remove(&p->recvq, aio);
			nni_aio_finish_error(aio, NNG_ECONNABORTED);
		}
		return;
	}
	if (nni_list_empty(&p->recvq)) {
		log_error("recv stopped due to empty recvq!");
		return;
	}

	int *flags = nni_aio_get_prov_data(aio);

	// Schedule a read of the header.
	if (flags) {
		int strmid = (*flags & QUIC_MULTISTREAM_FLAGS);
		quic_substream *stream = &p->substreams[strmid-1];
		if (!nni_aio_list_active(&stream->raio)) {
			nni_iov sub_iov;
			stream->gotrxhead  = 0;
			// 2 = MIN_FIXED_HEADER_LEN
			stream->wantrxhead = 2;
			sub_iov.iov_buf   = stream->rxlen;
			sub_iov.iov_len   = 2;
			memset(stream->rxlen, '\0', sizeof(uint64_t) * sizeof(stream->rxlen[0]));
			nni_aio_set_iov(&stream->raio, 1, &sub_iov);
			log_info(" start recv on sub aio %p", &stream->raio);
			nni_aio_set_prov_data(&stream->raio, &stream->id);
			nng_stream_recv(p->conn, &stream->raio);
		} else {
			log_error("Init Receving action on a scheduled aio!");
		}
	} else if (!nni_aio_list_active(p->rxaio)) {
		p->gotrxhead  = 0;
		// 2 = MIN_FIXED_HEADER_LEN
		p->wantrxhead = 2;
		iov.iov_buf   = p->rxlen;
		iov.iov_len   = 2;
		memset(p->rxlen, '\0', sizeof(uint64_t) * sizeof(p->rxlen[0]));
		nni_aio_set_iov(p->rxaio, 1, &iov);
		log_info(" start recv on main aio %p", p->rxaio);
		nng_stream_recv(p->conn, p->rxaio);
	} else {
		// wait last recv action to finish
		log_error("Main stream aio schedule failed! Aio is already active");
	}
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

	if (nni_lmq_get(&p->rxlmq, &msg) == 0) {
		// nni_aio_list_remove(aio);
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
#ifdef NNG_HAVE_MQTT_BROKER
	if (p->connack != NULL) {
		nni_aio_set_msg(aio, p->connack);
		nni_msg_set_conn_param(p->connack, p->cparam);
		p->connack = NULL;
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish(aio, 0, 0);
		return;
	}
#endif
	nni_list_append(&p->recvq, aio);
	if (nni_list_first(&p->recvq) == aio) {
		mqtt_quictran_pipe_recv_start(p, aio);
	}
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
	p->ep     = ep;
	p->rcvmax = 0;
	p->sndmax = 65535;
#ifdef NNG_HAVE_MQTT_BROKER
	p->cparam = NULL;
#endif
	nni_dialer_getopt(ep->ndialer, NNG_OPT_MQTT_CONNMSG, &connmsg, NULL,
	    NNI_TYPE_POINTER);

	if (connmsg == NULL) {
		mqtt_version = 0;
		log_error("User forget to set CONNECT msg!");
	} else {
		mqtt_version = nni_mqtt_msg_get_connect_proto_version(connmsg);
	}

	if (mqtt_version == MQTT_PROTOCOL_VERSION_v311) {
		if (ep->no_local_v4) {
			uint8_t proto_ver = mqtt_version;
			proto_ver |= 0x80;
			nni_mqtt_msg_set_connect_proto_version(
			    connmsg, proto_ver);
		}
		rv = nni_mqtt_msg_encode(connmsg);
		// Stupid Mosquitto hack.
		nni_mqtt_msg_set_connect_proto_version(connmsg, mqtt_version);
	} else if (mqtt_version == MQTT_PROTOCOL_VERSION_v5) {
		property *prop = nni_mqtt_msg_get_connect_property(connmsg);
		property_data *data;
		data = property_get_value(prop, MAXIMUM_PACKET_SIZE);
		if (data)
			p->rcvmax = data->p_value.u32;
		rv = nni_mqttv5_msg_encode(connmsg);
	} else {
		nni_plat_printf("Warning. MQTT protocol version is not specificed.\n");
		rv = 1;
	}

	if (rv != MQTT_SUCCESS ||
	   (mqtt_version != MQTT_PROTOCOL_VERSION_v311 &&
	    mqtt_version != MQTT_PROTOCOL_VERSION_v5)) {
		// Free the msg from user
		nni_msg_free(connmsg);
		log_error("Warning. Cancelled a illegal connnect msg from user.\n");
		// Using MQTT V311 as default protocol version
		mqtt_version = 4; // Default version 4
		nni_mqtt_msg_alloc(&connmsg, 0);
		nni_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
		nni_mqtt_msg_set_connect_proto_version(
		    connmsg, MQTT_PROTOCOL_VERSION_v311);
		nni_mqtt_msg_set_connect_keep_alive(connmsg, 60);
		nni_mqtt_msg_set_connect_clean_session(connmsg, true);
		ep->connmsg = connmsg;
	}

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
	nni_iov sub_iov[4];
	for (uint8_t i = 0; i < QUIC_SUB_STREAM_NUM; i++)
	{
		// simply go through all sub stream.
		quic_substream *stream = &p->substreams[i];
		if (!nni_aio_list_active(&stream->raio)) {
			stream->gotrxhead    = 0;
			// 2 = MIN_FIXED_HEADER_LEN
			stream->wantrxhead   = 2;
			sub_iov[i].iov_buf   = stream->rxlen;
			sub_iov[i].iov_len   = 2;
			nni_aio_set_iov(&stream->raio, 1, &sub_iov[i]);
			nni_aio_set_prov_data(&stream->raio, &stream->id);
			log_debug(" start recv on sub aio %p", &stream->raio);
			nng_stream_recv(p->conn, &stream->raio);
		}
	}
}

static void
mqtt_quictran_ep_fini(void *arg)
{
	mqtt_quictran_ep *ep = arg;

	nni_mtx_lock(&ep->mtx);
	ep->fini = true;
	if (ep->refcnt != 0) {
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

// This parses off the optional source address that this transport uses.
// The special handling of this URL format is quite honestly an historical
// mistake, which we would remove if we could.


// we delete mqtt_quictran_url_parse_source

// static void
// mqtt_quictran_timer_cb(void *arg)
// {
// 	mqtt_quictran_ep *ep = arg;
// 	if (nni_aio_result(ep->timeaio) == 0) {
// 		nng_stream_listener_accept(ep->listener, ep->connaio);
// 	}
// }

// static void
// mqtt_quictran_accept_cb(void *arg)
// {
// 	mqtt_quictran_ep *  ep  = arg;
// 	nni_aio *          aio = ep->connaio;
// 	mqtt_quictran_pipe *p;
// 	int                rv;
// 	nng_stream *       conn;

// 	nni_mtx_lock(&ep->mtx);

// 	if ((rv = nni_aio_result(aio)) != 0) {
// 		goto error;
// 	}

// 	conn = nni_aio_get_output(aio, 0);
// 	if ((rv = mqtt_quictran_pipe_alloc(&p)) != 0) {
// 		nng_stream_free(conn);
// 		goto error;
// 	}

// 	if (ep->closed) {
// 		mqtt_quictran_pipe_fini(p);
// 		nng_stream_free(conn);
// 		rv = NNG_ECLOSED;
// 		goto error;
// 	}
// 	mqtt_quictran_pipe_start(p, conn, ep);
// 	nng_stream_listener_accept(ep->listener, ep->connaio);
// 	nni_mtx_unlock(&ep->mtx);
// 	return;

// error:
// 	// When an error here occurs, let's send a notice up to the consumer.
// 	// That way it can be reported properly.
// 	if ((aio = ep->useraio) != NULL) {
// 		ep->useraio = NULL;
// 		nni_aio_finish_error(aio, rv);
// 	}
// 	switch (rv) {

// 	case NNG_ENOMEM:
// 	case NNG_ENOFILES:
// 		nng_sleep_aio(10, ep->timeaio);
// 		break;

// 	default:
// 		if (!ep->closed) {
// 			nng_stream_listener_accept(ep->listener, ep->connaio);
// 		}
// 		break;
// 	}
// 	nni_mtx_unlock(&ep->mtx);
// }

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

	// XXX Reset the d_started flag to allow eatablish multistream fast
	nni_atomic_flag_reset(&ep->ndialer->d_started);

	if ((rv = nni_aio_begin(aio)) != 0) {
		log_error("ep connect rv %d", rv);
		return;
	}
	if (ep->closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->backoff != 0) {
		ep->backoff = ep->backoff * 2;
		ep->backoff = ep->backoff > ep->backoff_max
		    ? (nni_duration) (nni_random() % 1000)
		    : ep->backoff;
		log_info("reconnect in %ld", ep->backoff);
		nni_msleep(ep->backoff);
	} else {
		ep->backoff = nni_random()%2000;
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
	rv = nni_copyin_int(v, &ep->reason_code, sizeof(ep->reason_code), 0, 256, t);
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
mqtt_quictran_ep_set_connmsg(
    void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_copyin_ptr(&ep->connmsg, v, sz, t);
	nni_mtx_unlock(&ep->mtx);

	return (rv);
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
mqtt_quictran_ep_set_ep_closed(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	bool             tmp;
	int              rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&ep->mtx);
		ep->closed = tmp;
		if (tmp == true) {
			mqtt_quictran_pipe *p;
			NNI_LIST_FOREACH (&ep->busypipes, p) {
				nni_pipe_close(p->npipe);
			}
		}
		nni_mtx_unlock(&ep->mtx);
	}
	return (rv);
}

static int
mqtt_quictran_ep_set_priority(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
	mqtt_quictran_ep *ep = arg;
	int rv;
	int tmp;

	nni_mtx_lock(&ep->mtx);
	if((rv = nni_copyin_int(&tmp, v, sizeof(int), 0, 65535, t)) != 0) {
		nni_stream_dialer_set(ep->dialer, NNG_OPT_QUIC_PRIORITY, NULL, sizeof(int), NNI_TYPE_INT32);
		return rv;
	}

	nni_stream_dialer_set(ep->dialer, NNG_OPT_QUIC_PRIORITY, &tmp, sizeof(int), NNI_TYPE_INT32);
	nni_mtx_unlock(&ep->mtx);

	return (rv);
}

static int
mqtt_quictran_ep_set_nolocal_v4(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_quictran_ep *ep = arg;
	bool             tmp;
	int              rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&ep->mtx);
		ep->no_local_v4 = tmp;
		nni_mtx_unlock(&ep->mtx);
	}
	return (rv);
}

/*
static int
mqtt_quictran_ep_bind(void *arg)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	nni_mtx_lock(&ep->mtx);
	rv = nng_stream_listener_listen(ep->listener);
	nni_mtx_unlock(&ep->mtx);

	return (rv);
}

static void
mqtt_quictran_ep_accept(void *arg, nni_aio *aio)
{
	mqtt_quictran_ep *ep = arg;
	int              rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->useraio != NULL) {
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
	if (!ep->started) {
		ep->started = true;
		nng_stream_listener_accept(ep->listener, ep->connaio);
	} else {
		mqtt_quictran_ep_match(ep);
	}
	nni_mtx_unlock(&ep->mtx);
}
*/

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
	    .o_name = NNG_OPT_BRIDGE_SET_EP_CLOSED,
	    .o_set  = mqtt_quictran_ep_set_ep_closed,
	},
	{
	    .o_name = NNG_OPT_MQTT_QUIC_PRIORITY,
	    .o_set  = mqtt_quictran_ep_set_priority,
	},
	{
	    .o_name = NNG_OPT_MQTT_NO_LOCAL_V4,
	    .o_set  = mqtt_quictran_ep_set_nolocal_v4,
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
