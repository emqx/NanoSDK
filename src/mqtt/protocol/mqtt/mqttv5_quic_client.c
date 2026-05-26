//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "nng/mqtt/mqtt_quic_client.h"
#include "sqlite_handler.h"
#include "core/nng_impl.h"
#include "core/sockimpl.h"
#include "nng/supplemental/nanolib/conf.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/mqtt/mqtt_qos_db_api.h"
#include "supplemental/quic/quic_api.h"

#define NNG_MQTT_SELF 0
#define NNG_MQTT_SELF_NAME "mqtt-client"
#define NNG_MQTT_PEER 0
#define NNG_MQTT_PEER_NAME "mqtt-server"
#define MQTT_QUIC_RETRTY 5  // 5 seconds as default minimum timer
#define MQTT_QUIC_KEEPALIVE 5  // 5 seconds as default

typedef struct mqtt_sock_s   mqtt_sock_t;
typedef struct mqtt_pipe_s   mqtt_pipe_t;
typedef struct mqtt_quic_ctx mqtt_quic_ctx;
typedef nni_mqtt_packet_type packet_type_t;

static int prior_flags = QUIC_HIGH_PRIOR_MSG;

static void mqtt_quic_sock_init(void *arg, nni_sock *sock);
static void mqtt_quic_sock_fini(void *arg);
static void mqtt_quic_sock_open(void *arg);
static void mqtt_quic_sock_send(void *arg, nni_aio *aio);
static void mqtt_quic_sock_recv(void *arg, nni_aio *aio);
static void mqtt_quic_send_cb(void *arg);
static void mqtt_quic_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

static int  quic_mqtt_pipe_init(void *arg, nni_pipe *qstrm, void *sock);
static void quic_mqtt_pipe_fini(void *arg);
static int  quic_mqtt_pipe_start(void *arg);
static void quic_mqtt_pipe_stop(void *arg);
static int  quic_mqtt_pipe_close(void *arg);

static void mqtt_quic_ctx_init(void *arg, void *sock);
static void mqtt_quic_ctx_fini(void *arg);
static void mqtt_quic_ctx_recv(void *arg, nni_aio *aio);
static void mqtt_quic_ctx_send(void *arg, nni_aio *aio);

static int
mqtt_pipe_send_msg(nni_aio *aio, nni_msg *msg, mqtt_pipe_t *p, uint16_t packet_id);

#if defined(NNG_SUPP_SQLITE)
static void *mqtt_quic_sock_get_sqlite_option(mqtt_sock_t *s);
#endif

struct mqtt_client_cb {
	int (*connect_cb)(void *, void *);
	void *connarg;
	int (*msg_send_cb)(void *, void *);
	void *sendarg;
	int (*msg_recv_cb)(void *, void *);
	void *recvarg;
	int (*disconnect_cb)(void *, void *);
	void *discarg;
};

struct mqtt_quic_ctx {
	mqtt_sock_t * mqtt_sock;
	nni_aio *     saio;
	nni_aio *     raio;
	nni_list_node sqnode;
	nni_list_node rqnode;
};

// A mqtt_sock_s is our per-socket protocol private structure.
struct mqtt_sock_s {
	bool            multi_stream;
	bool            qos_first;
	nni_mtx         mtx; // more fine grained mutual exclusion
	nni_atomic_bool closed;
	nni_atomic_int  next_packet_id; // next packet id to use, shared by multiple pipes
	nni_duration    retry;
	nni_duration    keepalive; // mqtt keepalive
	nni_duration    timeleft;  // left time to send next ping

	mqtt_quic_ctx master;     // to which we delegate send/recv calls
	nni_list      recv_queue; // ctx pending to receive
	nni_list      send_queue; // ctx pending to send
	nni_lmq  send_messages; // send messages queue (only for major stream)
	nni_lmq *ack_lmq;       // created for ack aio callback
	nni_lmq *topic_lmq;  // queued msg waiting to be send as first in new stream
	nni_id_map  *topic_map;  // Cache the topics that of msg in topic_lmq
	nni_id_map  *sub_streams; // sub (bidirectional) pipes, only for multi-stream mode
	nni_id_map  *pub_streams; // pub pipes, unidirectional stream
	mqtt_pipe_t *pipe;        // the major pipe (control stream)
	                   // main quic pipe, others needs a map to store the
	                   // relationship between MQTT topics and quic pipes
	nni_aio   time_aio; // timer aio to resend unack msg
	nni_aio  *ack_aio;  // set by user, expose puback/pubcomp
	nni_sock *nsock;

	nni_mqtt_sqlite_option *sqlite_opt;
	conf_bridge_node       *bridge_conf;

	struct mqtt_client_cb cb; // user cb
};

// A mqtt_pipe_s is our per-stream protocol private structure.
// equal to self-defined pipe in other protocols
struct mqtt_pipe_s {
	nni_mtx         lk;
	void           *qpipe; // QUIC version of nni_pipe
	bool            busy;
	bool            ready;			// mark if QUIC stream is ready
	mqtt_sock_t    *mqtt_sock;
	nni_list        recv_queue;    // ctx pending to receive
	nni_id_map      sent_unack;    // unacknowledged sent     messages
	nni_id_map      recv_unack;    // unacknowledged received messages
	nni_aio         send_aio;      // send aio to the underlying transport
	nni_aio         recv_aio;      // recv aio to the underlying transport
	nni_aio         rep_aio;       // aio for resending qos msg and PINGREQ
	nni_lmq 		send_inflight; // only used in multi-stream mode
	nni_lmq         recv_messages; // recv messages queue
	nni_lmq         send_messages; // send messages queue
	nni_msg        *idmsg;		   // only valid in multi-stream
	conn_param     *cparam;
	uint16_t        rid;           // index of resending packet id
	uint8_t         reason_code;   // MQTTV5 reason code
	nni_atomic_bool closed;
	uint8_t         pingcnt;
	nni_msg        *pingmsg;
};

static inline int
mqtt_pipe_recv_msgq_putq(mqtt_pipe_t *p, nni_msg *msg)
{
	if (0 != nni_lmq_put(&p->recv_messages, msg)) {
		size_t max_que_len = p->mqtt_sock->bridge_conf != NULL
		    ? p->mqtt_sock->bridge_conf->max_recv_queue_len
		    : NNG_TRAN_MAX_LMQ_SIZE;

		if (max_que_len > nni_lmq_cap(&p->recv_messages)) {

			size_t double_que_cap =
			    nni_lmq_cap(&p->recv_messages) * 2;
			size_t resize_que_len = double_que_cap < max_que_len
			    ? double_que_cap
			    : max_que_len;

			if (0 !=
			    nni_lmq_resize(
			        &p->recv_messages, resize_que_len)) {
				log_warn("Resize receive lmq failed due to "
				         "memory error!");
			} else {
				if (0 == nni_lmq_put(&p->recv_messages, msg)) {
					return 0;
				}
				log_warn("Message dropped due to receive "
				         "message queue is full!");
			}
		}
		return -1;
	} else {
		return 0;
	}
}

// Should be called with mutex lock hold after pipe is secured
// return rv>0 when aio should be finished (error or successed)
static int
mqtt_send_msg(nni_aio *aio, nni_msg *msg, mqtt_sock_t *s)
{
	mqtt_pipe_t *p   = s->pipe;
	nni_msg     *tmsg;
	uint16_t     ptype, packet_id;
	uint8_t      qos = 0;

	nni_mtx_lock(&p->lk);
	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
		// impossible to reach here
	case NNG_MQTT_DISCONNECT:
	case NNG_MQTT_PUBACK:
	case NNG_MQTT_PUBREC:
	case NNG_MQTT_PUBREL:
	case NNG_MQTT_PUBCOMP:
		// TODO MQTT V5
	case NNG_MQTT_PINGREQ:
		break;

	case NNG_MQTT_PUBLISH:
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (qos == 0) {
			break; // QoS 0 need no packet id
		}
		// fall through
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id = nni_mqtt_msg_get_packet_id(msg);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_id_get(&p->sent_unack, packet_id);
		if (tmsg != NULL) {
			log_warn("msg %d lost due to packetID duplicated!", packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio) {
				nni_aio_finish_error(m_aio, UNSPECIFIED_ERROR);
			}
			nni_id_remove(&p->sent_unack, packet_id);
			nni_msg_free(tmsg);
		}
		// cache QoS msg with packetid for potential resending
		nni_msg_clone(msg);
		if (0 != nni_id_set(&p->sent_unack, packet_id, msg)) {
			log_warn("Cache QoS msg failed");
			nni_msg_free(msg);
			nni_id_remove(&p->sent_unack, packet_id);
			nni_mqtt_msg_set_aio(msg, NULL);
			nni_aio_finish_error(aio, UNSPECIFIED_ERROR);
		}
		break;
	default:
		log_error("Undefined type");
		nni_mtx_unlock(&p->lk);
		return NNG_EPROTO;
	}
	if (s->qos_first)
		if (ptype == NNG_MQTT_SUBSCRIBE || ptype == NNG_MQTT_UNSUBSCRIBE ||
		   (qos > 0 && ptype == NNG_MQTT_PUBLISH)) {
			nni_aio_set_msg(aio, msg);
			nni_aio_set_prov_data(aio, &prior_flags);
			nni_pipe_send(p->qpipe, aio);
			log_debug("sending highpriority QoS msg in parallel");
			nni_mtx_unlock(&p->lk);
			return -1;
		}
	if (!p->busy) {
		nni_aio_set_msg(&p->send_aio, msg);
		p->busy = true;
		nni_pipe_send(p->qpipe, &p->send_aio);
	} else {
		if (nni_lmq_full(&p->send_messages)) {
			size_t max_que_len = p->mqtt_sock->bridge_conf != NULL
			    ? p->mqtt_sock->bridge_conf->max_send_queue_len
			    : NNG_TRAN_MAX_LMQ_SIZE;

			int run_remove = 1;
			if (max_que_len > nni_lmq_cap(&p->send_messages)) {
				size_t double_que_cap =
				    nni_lmq_cap(&p->send_messages) * 2;
				size_t resize_que_len =
				    double_que_cap < max_que_len
				    ? double_que_cap
				    : max_que_len;

				if (0 != nni_lmq_resize(
				        &p->send_messages, resize_que_len)) {
					log_debug("Max sendq cap%d len%d",
					    nni_lmq_cap(&p->send_messages),
						nni_lmq_len(&p->send_messages));
					log_warn("msg lost due to flight "
					         "window is full");
				} else {
					log_info("Resize max send queue to %d",
					    nni_lmq_cap(&p->send_messages));
					run_remove = 0;
				}
			}
			// Run remove when send_messages already gets to
			// max_que_len or resize failed.
			if (run_remove) {
				(void) nni_lmq_get(&p->send_messages, &tmsg);
				log_warn("remove old msg due to flight window is full");
				uint32_t tmp_type = nni_msg_get_type(tmsg);
				if (tmp_type == CMD_PUBLISH ||
				    tmp_type == CMD_SUBSCRIBE ||
				    tmp_type == CMD_UNSUBSCRIBE) {
					nni_msg *old_msg = nni_id_get(&p->sent_unack,
					        nni_mqtt_msg_get_packet_id(tmsg));
					if (old_msg != NULL) {
						nni_id_remove(&p->sent_unack,
						        nni_mqtt_msg_get_packet_id(tmsg));
						nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
						if (m_aio) {
							nni_aio_finish_error(m_aio, NNG_ECANCELED);
						}
						nni_mqtt_msg_set_aio(old_msg, NULL);
						nni_msg_free(old_msg);
					}
				}
				nni_msg_free(tmsg);
			}
		}
		if (0 != nni_lmq_put(&p->send_messages, msg)) {
			log_error("enqueue failed!");
			nni_id_remove(&p->sent_unack, packet_id);
			// remove aio from msg in case double finish
			nni_mqtt_msg_set_aio(msg, NULL);
			nni_aio_finish_error(aio, ECANCELED);
			nni_msg_free(msg);
		}
	}
	nni_mtx_unlock(&p->lk);
	// only finish aio when we dont care about ack
	if (ptype != NNG_MQTT_SUBSCRIBE &&
	    ptype != NNG_MQTT_UNSUBSCRIBE && qos == 0) {
		return 0;
	}
	return -1;
}

// send msg with specific pipe/stream for only Pub/SUb/UnSub
// aio is from user space for tracking the result
static int
mqtt_pipe_send_msg(nni_aio *aio, nni_msg *msg, mqtt_pipe_t *p, uint16_t packet_id)
{
	nni_msg     *tmsg;
	uint16_t     ptype;
	uint8_t      qos = 0;

	nni_mtx_lock(&p->lk);
	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
		log_error("wrong type of msg is being sent via data stream!");
	case NNG_MQTT_PUBACK:
	case NNG_MQTT_PUBREC:
	case NNG_MQTT_PUBREL:
	case NNG_MQTT_PUBCOMP:
		// TODO MQTT V5
	case NNG_MQTT_PINGREQ:
		break;

	case NNG_MQTT_PUBLISH:
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (qos == 0) {
			break; // QoS 0 need no packet id
		}
		// fall through
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id = nni_mqtt_msg_get_packet_id(msg);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_id_get(&p->sent_unack, packet_id);
		if (tmsg != NULL) {
			log_warn("msg %d lost due to "
			         "packetID duplicated!", packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio) {
				nni_aio_finish_error(m_aio, UNSPECIFIED_ERROR);
			}
			nni_id_remove(&p->sent_unack, packet_id);
			nni_msg_free(tmsg);
		}
		nni_msg_clone(msg);
		if (0 != nni_id_set(&p->sent_unack, packet_id, msg)) {
			log_warn("Cache QoS msg failed");
			nni_msg_free(msg);
			nni_id_remove(&p->sent_unack, packet_id);
			nni_aio_finish_error(aio, UNSPECIFIED_ERROR);
		}
		break;
	default:
		nni_mtx_unlock(&p->lk);
		return NNG_EPROTO;
	}
	if (!p->busy) {
		// TODO: qos_first in data pipe
		nni_aio_set_msg(&p->send_aio, msg);
		p->busy = true;
		nni_pipe_send(p->qpipe, &p->send_aio);
	} else {
		if (nni_lmq_full(&p->send_inflight)) {
			(void) nni_lmq_get(&p->send_inflight, &tmsg);
			log_warn("remove old msg due to flight window is full");
			uint32_t tmp_type = nni_msg_get_type(tmsg);
			if (tmp_type == CMD_PUBLISH ||
			    tmp_type == CMD_SUBSCRIBE ||
			    tmp_type == CMD_UNSUBSCRIBE) {
				nni_msg *old_msg = nni_id_get(&p->sent_unack,
				    nni_mqtt_msg_get_packet_id(tmsg));
				if (old_msg != NULL) {
					nni_id_remove(&p->sent_unack, packet_id);
					nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
					if (m_aio) {
						nni_aio_finish_error(m_aio, NNG_ECANCELED);
					}
					nni_mqtt_msg_set_aio(old_msg, NULL);
					nni_msg_free(old_msg);
				}
			}
			nni_msg_free(tmsg);
		}
		if (0 != nni_lmq_put(&p->send_inflight, msg)) {
			log_error("enqueue msg failed!");
			nni_msg_free(msg);
			nni_id_remove(&p->sent_unack, packet_id);
			nni_mqtt_msg_set_aio(msg, NULL);
			nni_aio_finish_error(aio, ECANCELED);
		}
	}
	nni_mtx_unlock(&p->lk);

	// only finish aio when we dont care about ack
	if (ptype != NNG_MQTT_SUBSCRIBE &&
	    ptype != NNG_MQTT_UNSUBSCRIBE && qos == 0) {
		return 0;
	}
	return -1;
}

// only work for data strm.
static void
mqtt_quic_data_strm_send_cb(void *arg)
{
	mqtt_pipe_t *p   = arg;
	mqtt_sock_t *s   = p->mqtt_sock;
	nni_msg     *msg = NULL;

	if (nni_aio_result(&p->send_aio) != 0) {
		// We failed to send... clean up and deal with it in transport.
		p->busy = false;
		nni_aio_set_msg(&p->send_aio, NULL);
		return;
	}
	nni_mtx_lock(&p->lk);
	if (nni_atomic_get_bool(&p->closed)) {
		// This occurs if the mqtt_pipe_close has been called.
		// In that case we don't want any more processing.
		nni_mtx_unlock(&p->lk);
		return;
	}
#if defined(NNG_SUPP_SQLITE)
	// TODO how to let sqlite work with multi-stream?
#endif
	// ignore msg in s->send_queue, I am a data stream
	// Check cached msg in lmq
	// this msg is already encoded by mqtt_send_msg
	if (nni_lmq_get(&p->send_inflight, &msg) == 0) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		nni_pipe_send(p->qpipe, &p->send_aio);
		nni_mtx_unlock(&p->lk);
		// share same cb with main stream
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}

	nni_aio_set_msg(&p->send_aio, NULL);
	p->busy = false;
	nni_mtx_unlock(&p->lk);

	if (s->cb.msg_send_cb)
		s->cb.msg_send_cb(NULL, s->cb.sendarg);

	return;
}

// main stream send_cb
static void
mqtt_quic_send_cb(void *arg)
{
	mqtt_pipe_t *p   = arg;
	mqtt_sock_t *s   = p->mqtt_sock;
	nni_msg     *msg = NULL;
	nni_aio     *aio;

	if (nni_aio_result(&p->send_aio) != 0) {
		// We failed to send... clean up and deal with it.
		log_warn("fail to send on aio");
		// msg is already be freed in QUIC transport
		nni_aio_set_msg(&p->send_aio, NULL);
		return;
	}
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		// This occurs if the mqtt_pipe_close has been called.
		// In that case we don't want any more processing.
		return;
	}
	nni_mtx_lock(&s->mtx);
	s->timeleft = s->keepalive;

	// Check cached aio first
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		int rv = 0;
		// TODO >= or JUST > ???
		if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish(aio, rv, 0);
			return;
		}
		nni_mtx_unlock(&s->mtx);
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}
	nni_mtx_unlock(&s->mtx);
	// Check cached msg in lmq later
	// this msg is already proessed by mqtt_send_msg
	nni_mtx_lock(&p->lk);
	if (nni_lmq_get(&p->send_messages, &msg) == 0) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		nni_pipe_send(p->qpipe, &p->send_aio);
		nni_mtx_unlock(&p->lk);
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}

#if defined(NNG_SUPP_SQLITE)
	nni_mqtt_sqlite_option *sqlite = mqtt_quic_sock_get_sqlite_option(s);
	if (sqlite_is_enabled(sqlite)) {
		if (!nni_lmq_empty(&sqlite->offline_cache)) {
			sqlite_flush_offline_cache(sqlite);
		}
		if (NULL != (msg = sqlite_get_cache_msg(sqlite))) {
			p->busy = true;
			nni_aio_set_msg(&p->send_aio, msg);
			nni_pipe_send(p->qpipe, &p->send_aio);
			nni_mtx_unlock(&p->lk);
			return;
		}
	}
#endif

	nni_aio_set_msg(&p->send_aio, NULL);
	p->busy = false;
	nni_mtx_unlock(&p->lk);

	if (s->cb.msg_send_cb)
		s->cb.msg_send_cb(NULL, s->cb.sendarg);

	return;
}

// only publish & suback/unsuback packet is valid
static void
mqtt_quic_data_strm_recv_cb(void *arg)
{
	mqtt_pipe_t *  p = arg;
	mqtt_sock_t *  s = p->mqtt_sock;
	nni_aio *      user_aio = NULL;
	nni_msg *      cached_msg = NULL;
	nni_aio *      aio;
	mqtt_quic_ctx *ctx;

	if (nni_aio_result(&p->recv_aio) != 0) {
		// stream is closed in transport layer
		return;
	}

	nni_mtx_lock(&p->lk);
	nni_msg *msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (nni_atomic_get_bool(&p->closed)) {
		//free msg and dont return data when pipe is closed.
		nni_mtx_unlock(&p->lk);
		if (msg) {
			nni_msg_free(msg);
		}
		return;
	}
	if (msg == NULL) {
		nni_pipe_recv(p->qpipe, &p->recv_aio);
		nni_mtx_unlock(&p->lk);
		return;
	}

	nni_mqtt_msg_proto_data_alloc(msg);
	nni_mqttv5_msg_decode(msg);

	packet_type_t packet_type = nni_mqtt_msg_get_packet_type(msg);

	int32_t       packet_id;
	uint8_t       qos;

	// schedule another receive
	nni_pipe_recv(p->qpipe, &p->recv_aio);

	// set conn_param for upper layer
	if (p->cparam)
		nng_msg_set_conn_param(msg, p->cparam);
	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		log_error("CONNACK received in data stream!");
		nni_msg_free(msg);
		break;
	case NNG_MQTT_PUBACK:
		// we have received a PUBACK, successful delivery of a QoS 1
		// FALLTHROUGH
	case NNG_MQTT_PUBCOMP:
		// we have received a PUBCOMP, successful delivery of a QoS 2
		packet_id  = nni_mqtt_msg_get_packet_id(msg);
		p->rid     = packet_id;
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			nni_mqtt_msg_set_aio(cached_msg, NULL);
			nni_msg_free(cached_msg);
		}
		if (s->ack_aio == NULL) {
			// no callback being set
			log_debug("Ack Reason code:");
			nni_msg_free(msg);
			break;
		}
		if (!nni_aio_busy(s->ack_aio)) {
			nni_aio_set_msg(s->ack_aio, msg);
			nni_aio_finish(s->ack_aio, 0, nni_msg_len(msg));
		} else {
			nni_lmq_put(s->ack_lmq, msg);
			log_debug("ack msg cached!");
		}
		break;
	case NNG_MQTT_SUBACK:
		// we have received a SUBACK, successful subscription
		// FALLTHROUGH
	case NNG_MQTT_UNSUBACK:
		// we have received a UNSUBACK, successful unsubscription
		packet_id  = nni_mqtt_msg_get_packet_id(msg);
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		p->rid     = packet_id;
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			nni_mqtt_msg_set_aio(cached_msg, NULL);
			// should we support sub/unsub cb here?
			// if (packet_type == NNG_MQTT_SUBACK ||
			    // packet_type == NNG_MQTT_UNSUBACK) {
				// got a matched callback
			if (user_aio != NULL) {
				nni_msg_clone(msg);
				nni_aio_set_msg(user_aio, msg);
			}
			nni_msg_free(cached_msg);
		}
		nni_msg_free(msg);
		break;
	case NNG_MQTT_PUBREL:
		packet_id = nni_mqtt_msg_get_pubrel_packet_id(msg);
		cached_msg = nni_id_get(&p->recv_unack, packet_id);
		nni_msg_free(msg);
		if (cached_msg == NULL) {
			log_warn("ERROR! packet id %d not found\n", packet_id);
			break;
		}
		nni_id_remove(&p->recv_unack, packet_id);
		// return msg to user
		if ((ctx = nni_list_first(&p->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(p, cached_msg)) {
				nni_msg_free(cached_msg);
				cached_msg = NULL;
			}
			break;
		}
		nni_list_remove(&p->recv_queue, ctx);
		user_aio  = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(user_aio, cached_msg);
		break;
	case NNG_MQTT_PUBLISH:
		// clone conn_param every single time
		conn_param_clone(p->cparam);
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);
		nng_msg_set_cmd_type(msg, CMD_PUBLISH);
		if (2 > qos) {
			// aio should be placed in p->recv_queue to achieve parallel
			if ((ctx = nni_list_first(&p->recv_queue)) == NULL) {
				if (0 != mqtt_pipe_recv_msgq_putq(p, msg)) {
					nni_msg_free(msg);
					msg = NULL;
				}
				// log_error("no ctx found!! create more ctxs!");
				break;
			}
			nni_list_remove(&p->recv_queue, ctx);
			user_aio  = ctx->raio;
			ctx->raio = NULL;
			nni_aio_set_msg(user_aio, msg);
			break;
		} else {
			packet_id = nni_mqtt_msg_get_publish_packet_id(msg);
			if ((cached_msg = nni_id_get(
			         &p->recv_unack, packet_id)) != NULL) {
				// packetid already exists.
				// sth wrong with the broker
				// replace old with new
				log_error("packet id %d duplicates in", packet_id);
				if ((aio = nni_mqtt_msg_get_aio(cached_msg)) != NULL) {
					nng_aio_finish_error(aio, UNSPECIFIED_ERROR);
					nni_mqtt_msg_set_aio(cached_msg, NULL);
				}
				nni_msg_free(cached_msg);
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
		}
		break;
	case NNG_MQTT_PINGRESP:
		// PINGRESP is ignored in protocol layer
		// Rely on health checker of Quic stream
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;
	case NNG_MQTT_PUBREC:
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;
	default:
		// unexpected packet type, server misbehaviour
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		// close quic stream
		nni_pipe_close(p->qpipe);
		return;
	}
	nni_mtx_unlock(&p->lk);

	if (user_aio) {
		nni_aio_finish(user_aio, 0, 0);
	}
	// Trigger publish cb
	if (packet_type == NNG_MQTT_PUBLISH)
		if (s->cb.msg_recv_cb) // Trigger cb
			s->cb.msg_recv_cb(msg, s->cb.recvarg);
}

/***
 * recv cb func for singe-stream mode or main stream
*/
static void
mqtt_quic_recv_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_aio * user_aio = NULL;
	nni_msg * cached_msg = NULL;
	nni_msg * msg = NULL;
	nni_aio *aio;
	int rv = 0;

	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		// pipe is already closed somewhere
		// free msg and dont return data when pipe is closed.
		//msg = nni_aio_get_msg(&p->recv_aio);
		//if (msg) {
		//	nni_msg_free(msg);
		//}
		return;
	}

	nni_mtx_lock(&p->lk);
	if (nni_aio_result(&p->recv_aio) != 0 &&
	    !nni_atomic_get_bool(&p->closed)) { // not really affective
		nni_mtx_unlock(&p->lk);
		// stream is closed in transport layer
		if (p->qpipe != NULL) {
			log_info("nni pipe close!!");
			nni_pipe_close(p->qpipe);
		}
		return;
	}
	msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (msg == NULL) {
		nni_pipe_recv(p->qpipe, &p->recv_aio);
		nni_mtx_unlock(&p->lk);
		return;
	}

	// nni_msg_set_pipe(msg, nni_pipe_id(p->pipe));
	nni_mqtt_msg_proto_data_alloc(msg);
	if ((rv = nni_mqttv5_msg_decode(msg)) != MQTT_SUCCESS) {
		// Msg should be clear if decode failed. We reuse it to send disconnect.
		// Or it would encode a malformed packet.
		nni_mqtt_msg_set_packet_type(msg, NNG_MQTT_DISCONNECT);
		nni_mqtt_msg_set_disconnect_reason_code(msg, rv);
		nni_mqtt_msg_set_disconnect_property(msg, NULL);
		// Composed a disconnect msg
		if ((rv = nni_mqttv5_msg_encode(msg)) != MQTT_SUCCESS) {
			log_error("Error in encoding disconnect.\n");
			nni_msg_free(msg);
			nni_mtx_unlock(&p->lk);
			nni_pipe_close(p->qpipe);
			return;
		}
		if (!p->busy) {
			p->busy = true;
			nni_aio_set_msg(&p->send_aio, msg);
			nni_pipe_send(p->qpipe, &p->send_aio);
			nni_mtx_unlock(&p->lk);
			return;
		}
		if (nni_lmq_full(&p->send_inflight)) {
			nni_msg     *tmsg;
			(void) nni_lmq_get(&p->send_inflight, &tmsg);
			log_warn("msg lost due to flight window is full");
			nni_msg_free(tmsg);
		}
		if (0 != nni_lmq_put(&p->send_inflight, msg)) {
			log_warn("msg send failed due to busy socket");
		}
		nni_mtx_unlock(&p->lk);
		return;
	}

	packet_type_t packet_type = nni_mqtt_msg_get_packet_type(msg);

	int32_t        packet_id;
	uint8_t        qos;
	mqtt_quic_ctx *ctx;

	// reset ping state
	p->pingcnt = 0;

	//Schedule another receive
	nni_pipe_recv(p->qpipe, &p->recv_aio);

	// set conn_param for upper layer
	if (p->cparam)
		nng_msg_set_conn_param(msg, p->cparam);
	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		nng_msg_set_cmd_type(msg, CMD_CONNACK);
		// turn to publish msg and free in WAIT state
		p->cparam  = nng_msg_get_conn_param(msg);
		if (p->cparam != NULL) {
			conn_param_clone(p->cparam);
			// Set keepalive
			s->keepalive = conn_param_get_keepalive(p->cparam) * 1000;
			s->timeleft  = s->keepalive;
			log_info("Update keepalive to %dms", s->keepalive);
		}
		// Clone CONNACK for connect_cb & user aio cb
		if (s->cb.connect_cb) {
			nni_msg_clone(msg);
		}
		if (s->ack_aio != NULL && !nni_aio_busy(s->ack_aio)) {
			nni_msg_clone(msg);
			nni_aio_finish_msg(s->ack_aio, msg);
		}
		if ((ctx = nni_list_first(&p->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(p, msg)) {
				nni_msg_free(msg);
				msg = NULL;
			}
			break;
		}
		nni_list_remove(&p->recv_queue, ctx);
		user_aio  = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(user_aio, msg);
		break;
	case NNG_MQTT_PUBACK:
		// we have received a PUBACK, successful delivery of a QoS 1
		// FALLTHROUGH
	case NNG_MQTT_PUBCOMP:
		// we have received a PUBCOMP, successful delivery of a QoS 2
		packet_id  = nni_mqtt_msg_get_packet_id(msg);
		p->rid     = packet_id;
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			nni_mqtt_msg_set_aio(cached_msg, NULL);
			nni_msg_free(cached_msg);
		}
		if (s->ack_aio == NULL) {
			// no callback being set
			log_debug("Ack Reason code:");
			nni_msg_free(msg);
			break;
		}
		if (!nni_aio_busy(s->ack_aio)) {
			nni_aio_set_msg(s->ack_aio, msg);
			nni_aio_finish(s->ack_aio, 0, nni_msg_len(msg));
		} else {
			nni_lmq_put(s->ack_lmq, msg);
			log_debug("ack msg cached!");
		}
		break;
	case NNG_MQTT_SUBACK:
		log_debug("SUBACK received!");
		// we have received a SUBACK, successful subscription
		// FALLTHROUGH
	case NNG_MQTT_UNSUBACK:
		// we have received a UNSUBACK, successful unsubscription
		packet_id  = nni_mqtt_msg_get_packet_id(msg);
		p->rid     = packet_id;
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			nni_mqtt_msg_set_aio(cached_msg, NULL);
			if (user_aio != NULL) {
				// should we support sub/unsub cb here?
				nni_msg_clone(msg);
				nni_aio_set_msg(user_aio, msg);
			}
			nni_msg_free(cached_msg);
		}
		nni_msg_free(msg);
		break;
	case NNG_MQTT_PUBREL:
		packet_id = nni_mqtt_msg_get_pubrel_packet_id(msg);
		cached_msg = nni_id_get(&p->recv_unack, packet_id);
		nni_msg_free(msg);
		if (cached_msg == NULL) {
			log_warn("ERROR! packet id %d not found\n", packet_id);
			break;
		}
		nni_id_remove(&p->recv_unack, packet_id);
		// return msg to user
		if ((ctx = nni_list_first(&p->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(p, cached_msg)) {
				nni_msg_free(cached_msg);
				log_warn("app is not keeping up with sdk!");
				cached_msg = NULL;
			}
			break;
		}
		nni_list_remove(&p->recv_queue, ctx);
		user_aio  = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(user_aio, cached_msg);
		break;
	case NNG_MQTT_PUBLISH:
		// clone conn_param every single time
		conn_param_clone(p->cparam);
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);
		nng_msg_set_cmd_type(msg, CMD_PUBLISH);
		if (2 > qos) {
			// No one waiting to receive yet, putting msginto lmq
			if ((ctx = nni_list_first(&p->recv_queue)) == NULL) {
				if (s->cb.msg_recv_cb) {
					// trigger cb in the end. only one method is allowed to consume msg
					break;
				}
				if (0 != mqtt_pipe_recv_msgq_putq(p, msg)) {
					nni_msg_free(msg);
					msg = NULL;
				}
				log_debug("ERROR: no ctx found!! create more ctxs!");
				break;
			}
			nni_list_remove(&p->recv_queue, ctx);
			user_aio  = ctx->raio;
			ctx->raio = NULL;
			nni_aio_set_msg(user_aio, msg);
			break;
		} else {
			packet_id = nni_mqtt_msg_get_publish_packet_id(msg);
			if ((cached_msg = nni_id_get(
			         &p->recv_unack, packet_id)) != NULL) {
				// packetid already exists.
				// sth wrong with the broker
				// replace old with new
				log_error(
				    "ERROR: packet id %d duplicates in",
				    packet_id);
				if ((aio = nni_mqtt_msg_get_aio(cached_msg)) != NULL) {
					nng_aio_finish_error(aio, UNSPECIFIED_ERROR);
					nni_mqtt_msg_set_aio(cached_msg, NULL);
				}
				nni_msg_free(cached_msg);
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
		}
		break;
	case NNG_MQTT_PINGRESP:
		// PINGRESP is ignored in protocol layer
		// Rely on health checker of Quic stream
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;
	case NNG_MQTT_PUBREC:
		// return PUBREL
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;
	case NNG_MQTT_DISCONNECT:
		log_debug("Broker disconnect QUIC actively");
		p->reason_code = *(uint8_t *)nni_msg_body(msg);
		log_info(
		    " Disconnect received from Broker %d", *(uint8_t *)nni_msg_body(msg));
		// we wait for other side to close the stream
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;

	default:
		// unexpected packet type, server misbehaviour
		nni_msg_free(msg);
		// close quic stream
		nni_mtx_unlock(&p->lk);
		nni_pipe_close(p->qpipe);
		return;
	}
	nni_mtx_unlock(&p->lk);

	if (user_aio) {
		nni_aio_finish(user_aio, 0, 0);
	}
	// Trigger connect cb first in case connack being freed
	if (packet_type == NNG_MQTT_CONNACK)
		if (s->cb.connect_cb) {
			s->cb.connect_cb(msg, s->cb.connarg);
		}
	// Trigger publish cb
	if (packet_type == NNG_MQTT_PUBLISH)
		if (s->cb.msg_recv_cb) // Trigger cb
			s->cb.msg_recv_cb(msg, s->cb.recvarg);
}

// Timer callback, we use it for retransmition.
static void
mqtt_timer_cb(void *arg)
{
	mqtt_sock_t *s = arg;
	mqtt_pipe_t *p;

	if (nng_aio_result(&s->time_aio) != 0) {
		log_warn("sleep aio finish error!");
		nni_sleep_aio(s->retry * NNI_SECOND, &s->time_aio);
		return;
	}
	nni_mtx_lock(&s->mtx);

	p = s->pipe;

	if (NULL == p || nni_atomic_get_bool(&p->closed)) {
		// QUIC connection has been shut down
		nni_mtx_unlock(&s->mtx);
		return;
	}
	// Ping would be send at transport layer

	if (p->pingcnt > 1) {
		log_warn("MQTT Timeout and disconnect");
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;
	}

	// Update left time to send pingreq
	s->timeleft -= s->retry;

	if (!p->busy && !nni_aio_busy(&p->send_aio) && p->pingmsg &&
			s->timeleft <= 0) {
		p->busy = true;
		s->timeleft = s->keepalive;
		// send pingreq
		nni_msg_clone(p->pingmsg);
		nni_aio_set_msg(&p->send_aio, p->pingmsg);
		nni_pipe_send(p->qpipe, &p->send_aio);
		p->pingcnt ++;
		nni_mtx_unlock(&s->mtx);
		log_info("Send pingreq (sock%p)(%dms)", s, s->keepalive);
		nni_sleep_aio(s->retry, &s->time_aio);
		return;
	}

	// start message resending
	// uint16_t   pid = p->rid;
	// msg = nni_id_get_min(&p->sent_unack, &pid);
	// if (msg != NULL) {
	// 	uint16_t ptype;
	// 	ptype = nni_mqtt_msg_get_packet_type(msg);
	// 	if (ptype == NNG_MQTT_PUBLISH) {
	// 		nni_mqtt_msg_set_publish_dup(msg, true);
	// 	}
	// 	if (!p->busy) {
	// 		p->busy = true;
	// 		nni_msg_clone(msg);
	// 		aio = nni_mqtt_msg_get_aio(msg);
	// 		if (aio) {
	// 			nni_aio_bump_count(aio,
	// 			    nni_msg_header_len(msg) +
	// 			        nni_msg_len(msg));
	// 			nni_aio_set_msg(aio, NULL);
	// 		}
	// 		nni_aio_set_msg(&p->send_aio, msg);
	// 		quic_pipe_send(p->qpipe, &p->send_aio);

	// 		nni_mtx_unlock(&s->mtx);
	// 		nni_sleep_aio(s->retry  * NNI_SECOND, &s->time_aio);
	// 		return;
	// 	}
	// }
#if defined(NNG_SUPP_SQLITE)
	if (!p->busy) {
		nni_msg     *msg = NULL;
		nni_mqtt_sqlite_option *sqlite =
		    mqtt_quic_sock_get_sqlite_option(s);
		if (sqlite_is_enabled(sqlite)) {
			if (!nni_lmq_empty(&sqlite->offline_cache)) {
				sqlite_flush_offline_cache(sqlite);
			}
			if (NULL != (msg = sqlite_get_cache_msg(sqlite))) {
				p->busy = true;
				nni_aio_set_msg(&p->send_aio, msg);
				nni_pipe_send(p->qpipe, &p->send_aio);

				nni_mtx_unlock(&s->mtx);
				nni_sleep_aio(s->retry, &s->time_aio);
				return;
			}
		}
	}
#endif
	nni_mtx_unlock(&s->mtx);
	nni_sleep_aio(s->retry, &s->time_aio);
	return;
}

/* MQTT over Quic Sock */
/******************************************************************************
 *                            Socket Implementation                           *
 ******************************************************************************/

static void mqtt_quic_sock_init(void *arg, nni_sock *sock)
{
	mqtt_sock_t *s = arg;
	s->nsock       = sock;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);

	// this is a pre-defined timer for global timer
	s->sqlite_opt   = NULL;
	s->qos_first    = false;
	s->multi_stream = false;

	nni_mtx_init(&s->mtx);
	mqtt_quic_ctx_init(&s->master, s);
	nni_atomic_set(&s->next_packet_id, 1);

	s->bridge_conf = NULL;
	s->pub_streams = NULL;
	s->sub_streams = NULL;

	// this is "semi random" start for request IDs.
	s->retry     = MQTT_QUIC_RETRTY * NNI_SECOND;
	s->keepalive = NNI_SECOND * 10; // default mqtt keepalive
	s->timeleft  = NNI_SECOND * 10;

	nni_lmq_init(&s->send_messages, NNG_MAX_SEND_LMQ);
	nni_aio_list_init(&s->send_queue);
	NNI_LIST_INIT(&s->recv_queue, mqtt_quic_ctx, rqnode);
	nni_aio_init(&s->time_aio, mqtt_timer_cb, s);

	s->pipe = NULL;

	s->cb.connect_cb = NULL;
	s->cb.disconnect_cb = NULL;
	s->cb.msg_recv_cb = NULL;
	s->cb.msg_send_cb = NULL;
}

static void
mqtt_quic_sock_fini(void *arg)
{
	mqtt_sock_t *  s = arg;
	nni_aio *      aio;
	nni_msg *      tmsg = NULL, *msg = NULL;
	size_t         count = 0;
	mqtt_quic_ctx *ctx;

#if defined(NNG_SUPP_SQLITE) && defined(NNG_HAVE_MQTT_BROKER)
	nni_mqtt_sqlite_db_fini(s->sqlite_opt);
	nng_mqtt_free_sqlite_opt(s->sqlite_opt);
#endif

	log_debug("mqtt_quic_sock_fini %p", s);

	if (s->ack_aio != NULL) {
		nni_aio_fini(s->ack_aio);
		nng_free(s->ack_aio, sizeof(nni_aio *));
	}

	if (s->ack_lmq != NULL) {
		nni_lmq_fini(s->ack_lmq);
		nng_free(s->ack_lmq, sizeof(nni_lmq));
	}

	if (s->topic_lmq != NULL) {
		nni_lmq_fini(s->topic_lmq);
		nng_free(s->topic_lmq, sizeof(nni_lmq));
		nni_id_map_fini(s->topic_map);
		nng_free(s->topic_map, sizeof(nni_id_map));
	}
	// emulate disconnect notify msg as a normal publish
	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(aio, tmsg);
		// only return pipe closed error once for notification
		// sync action to avoid NULL conn param
		count == 0 ? nni_aio_finish_sync(aio, NNG_ECONNSHUT, 0)
		           : nni_aio_finish_error(aio, NNG_ECLOSED);
		// there should be no msg waiting
		count++;
	}
	while ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		if (msg != NULL) {
			nni_msg_free(msg);
		}
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	if (s->multi_stream) {
		nni_id_map_fini(s->sub_streams);
		nng_free(s->sub_streams, sizeof(nni_id_map));
		nni_id_map_fini(s->pub_streams);
		nng_free(s->pub_streams, sizeof(nni_id_map));
	}
	mqtt_quic_ctx_fini(&s->master);
	nni_lmq_fini(&s->send_messages);
	nni_aio_fini(&s->time_aio);
}

static void
mqtt_quic_sock_open(void *arg)
{
	mqtt_sock_t *s = arg;
	NNI_ARG_UNUSED(s);
}

static void
mqtt_quic_pipe_close(void *key, void *val)
{
	NNI_ARG_UNUSED(key);
	quic_mqtt_pipe_stop(val);
}

static void
mqtt_quic_sock_close(void *arg)
{
	nni_aio *aio;
	mqtt_sock_t *s = arg;
	mqtt_quic_ctx *ctx;

	nni_mtx_lock(&s->mtx);
	nni_sock_hold(s->nsock);
	nni_aio_close(&s->time_aio);
	nni_atomic_set_bool(&s->closed, true);

	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	// need to disconnect connection before sock fini
	// quic_disconnect(p->qsock, p->qpipe);
	// quic_sock_close(p->qsock);
	if (s->multi_stream) {
		nni_id_map_foreach(s->sub_streams,mqtt_quic_pipe_close);
		nni_id_map_foreach(s->pub_streams,mqtt_quic_pipe_close);
	}
	nni_lmq_flush(&s->send_messages);

	nni_sock_rele(s->nsock);
	nni_mtx_unlock(&s->mtx);
}

static void
mqtt_quic_sock_send(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s = arg;
	mqtt_quic_ctx_send(&s->master, aio);
}

static void
mqtt_quic_sock_recv(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s   = arg;
	mqtt_quic_ctx_recv(&s->master, aio);
}

#ifdef NNG_SUPP_SQLITE
static void *
mqtt_quic_sock_get_sqlite_option(mqtt_sock_t *s)
{
	return (s->sqlite_opt);
}
#endif

static int
mqtt_quic_sock_set_multi_stream(void *arg, const void *buf, size_t sz, nni_type t)
{
	mqtt_sock_t *s = arg;
	int               rv;
	bool              b;

	if (((rv = nni_copyin_bool(&b, buf, sz, t)) != 0)) {
		return (rv);
	}
	nni_mtx_lock(&s->mtx);
	s->multi_stream = b;
	if (s->sub_streams == NULL && s->pub_streams == NULL && s->multi_stream) {
		log_info("Quic Multistream bridging is enabled");
		s->sub_streams = nng_alloc(sizeof(nni_id_map));
		nni_id_map_init(s->sub_streams, 0x0000u, 0xffffu, true);
		s->pub_streams = nng_alloc(sizeof(nni_id_map));
		nni_id_map_init(s->pub_streams, 0x0000u, 0xffffu, true);
	} else {
		s->multi_stream = false;
		log_warn("multi-stream disabled!");
	}
	if (s->multi_stream && s->topic_lmq == NULL) {
		s->topic_lmq = nni_alloc(sizeof(nni_lmq));
		nni_lmq_init(s->topic_lmq, NNG_MAX_RECV_LMQ);
		s->topic_map = nni_alloc(sizeof(nni_id_map));
		nni_id_map_init(s->topic_map, 0x0000u, 0xffffu, true);
	}
	nni_mtx_unlock(&s->mtx);
	return (0);
}

static int
mqtt_quic_sock_set_bridge_config(
    void *arg, const void *v, size_t sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
	mqtt_sock_t *s = arg;
	if (t == NNI_TYPE_POINTER) {
		nni_mtx_lock(&s->mtx);
		s->bridge_conf = *(conf_bridge_node **) v;
		s->qos_first = s->bridge_conf->qos_first;
		nni_mtx_unlock(&s->mtx);
		return (0);
	}
	return NNG_EUNREACHABLE;
}

static int
mqtt_quic_sock_set_sqlite_option(
    void *arg, const void *v, size_t sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
#if defined(NNG_SUPP_SQLITE)
	mqtt_sock_t *s = arg;
	if (t == NNI_TYPE_POINTER) {
		nni_mtx_lock(&s->mtx);
		s->sqlite_opt = *(nni_mqtt_sqlite_option **) v;
		nni_mtx_unlock(&s->mtx);
		return (0);
	}
#else
	NNI_ARG_UNUSED(arg);
	NNI_ARG_UNUSED(v);
	NNI_ARG_UNUSED(t);
#endif
	return NNG_EUNREACHABLE;
}

// Multi-stream API
/**
 * Create independent & seperated stream for specific topic.
 * Only effective on Publish
 * This stream is unidirecitional by default
*/
/***
 * create a unidirectional stream and pub msg to it.
 * mapping sub topics (if >1) with the new stream.
*/

/***
 * create a bidirectional/unidirectional stream
 * send PUB/SUB packet to bind stream with topic
 * TODO: send UNSUB and close the stream?
*/
static int
quic_mqtt_stream_bind(mqtt_sock_t *s, mqtt_pipe_t *p, nni_pipe *npipe)
{
	NNI_ARG_UNUSED(npipe);
	int                 rv  = 0;
	nni_msg            *msg = NULL;
	nni_mqtt_topic_qos *topics;
	uint32_t            count, hash;
	// deal with data stream
	if (nni_lmq_get(s->topic_lmq, &msg) != 0) {
		log_warn("An unexpected data stream is created!");
		return -1;
	}
	if (nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_SUBSCRIBE) {
		// packet id is already encoded
		topics = nni_mqtt_msg_get_subscribe_topics(msg, &count);
		// Create a new stream no matter how many topics in Sub
		for (uint32_t i = 0; i < count; i++) {
			// Sub on same topic twice gonna replace old pipe with
			// new This may result in potentioal memleak
			hash = DJBHashn((char *) topics[i].topic.buf,
			    topics[i].topic.length);
			if (nni_id_set(s->sub_streams, hash, p) != 0) {
				log_error("Error in setting sub streams.");
				return -1;
			}
			log_info("New Sub Stream has been bound to topic (code %ld)", hash);
		}
		nni_msg_clone(msg);
		p->idmsg = msg;
		if ((rv = mqtt_pipe_send_msg(nni_mqtt_msg_get_aio(msg), msg, p, 0)) >= 0) {
			nni_aio_finish(nni_mqtt_msg_get_aio(msg), rv, 0);
			nni_mqtt_msg_set_aio(msg, NULL);
		}
	} else if (nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH) {
		uint32_t topic_len;
		char    *topic =
		    (char *) nni_mqtt_msg_get_publish_topic(msg, &topic_len);
		hash = DJBHashn(topic, topic_len);
		if (nni_id_set(s->pub_streams, hash, p) != 0) {
			log_error("Error in setting sub streams.");
			return -1;
		}
		nni_msg_clone(msg);
		p->idmsg = msg;
		log_info("New Pub Stream has been bound to %.*s (code %ld)", topic_len, topic, hash);
		// must be a new pub stream
		if ((rv = mqtt_pipe_send_msg(nni_mqtt_msg_get_aio(msg), msg, p, 0)) >= 0) {
			nni_aio_finish(nni_mqtt_msg_get_aio(msg), rv, 0);
			nni_mqtt_msg_set_aio(msg, NULL);
		}
		char *num_ptr;
		if (NULL != (num_ptr = nni_id_get(s->topic_map, hash))) {
			// Iterate topic lmq and send
			size_t lmqsz = nni_lmq_len(s->topic_lmq);
			log_debug("topic_lmq sz%ld Cached msg number%d", lmqsz, (int)(num_ptr - (char *)s));
			for (int i=0; i<(int)lmqsz && (void *)num_ptr != s; ++i) {
				nng_msg *lm;
				nni_lmq_get(s->topic_lmq, &lm);
				if (lm == NULL)
					continue;
				if (nni_mqtt_msg_get_packet_type(lm) != NNG_MQTT_PUBLISH) {
					nni_lmq_put(s->topic_lmq, lm);
					continue;
				}
				uint32_t tpl;
				char *tp = (char *) nni_mqtt_msg_get_publish_topic(lm, &tpl);
				if (tpl == topic_len && 0 == strncmp(tp, topic, tpl)) {
					if ((rv = mqtt_pipe_send_msg(nni_mqtt_msg_get_aio(lm), lm, p, 0)) >= 0) {
						nni_aio_finish(nni_mqtt_msg_get_aio(lm), rv, 0);
						nni_mqtt_msg_set_aio(lm, NULL);
					}
					num_ptr --;
					continue;
				}
				nni_lmq_put(s->topic_lmq, lm);
			}
			nni_id_set(s->topic_map, hash, NULL);
		}
	}
	return rv;
}
// end of Multi-stream API


/******************************************************************************
 *                          Stream(PIPE) Implementation                       *
 ******************************************************************************/
// allocate main stream with pipe
static int
quic_mqtt_pipe_init(void *arg, nni_pipe *pipe, void *sock)
{
	bool major     = false;
	mqtt_pipe_t *p = arg;
	p->mqtt_sock   = sock;
	p->cparam      = NULL;

	if (p->mqtt_sock->pipe == NULL) {
		p->mqtt_sock->pipe = p;
		major = true;
	} else {
		p->cparam = p->mqtt_sock->pipe->cparam;
	}

	p->pingcnt = 0;
	p->pingmsg = NULL;
	nni_msg_alloc(&p->pingmsg, 0);
	if (p->pingmsg == NULL) {
		log_error("Error in create a pingmsg");
		return NNG_ENOMEM;
	} else {
		uint8_t buf[2];
		buf[0] = 0xC0;
		buf[1] = 0x00;
		nni_msg_header_append(p->pingmsg, buf, 2);
	}

	p->qpipe = pipe;
	p->rid = 1;
	p->reason_code = 0;
	p->busy  = false;
	p->ready = false;
	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, true);

	nni_aio_init(&p->rep_aio, NULL, p);
	major == true
	    ? nni_aio_init(&p->send_aio, mqtt_quic_send_cb, p)
	    : nni_aio_init(&p->send_aio, mqtt_quic_data_strm_send_cb, p);
	major == true ? nni_aio_init(&p->recv_aio, mqtt_quic_recv_cb, p)
	              : nni_aio_init(&p->recv_aio, mqtt_quic_data_strm_recv_cb, p);
	// Packet IDs are 16 bits
	// We start at a random point, to minimize likelihood of
	// accidental collision across restarts.
	nni_id_map_init(&p->sent_unack, 0x0000u, 0xffffu, true);
	nni_id_map_init(&p->recv_unack, 0x0000u, 0xffffu, true);
	nni_lmq_init(&p->recv_messages, NNG_MAX_RECV_LMQ);
	// only data stream has send inflight?
	if (p->mqtt_sock->multi_stream)
		nni_lmq_init(&p->send_inflight, NNG_MAX_RECV_LMQ);
	nni_mtx_init(&p->lk);
	NNI_LIST_INIT(&p->recv_queue, mqtt_quic_ctx, rqnode);
	nni_lmq_init(&p->send_messages, NNG_MAX_SEND_LMQ);

	// TODO Not compatible with multi stream
	// Move ctx and msgs from sock to pipe
	mqtt_quic_ctx *ctx = NULL;
	nni_msg *msg = NULL;
	nni_mtx_lock(&p->mqtt_sock->mtx);
	while ((ctx = nni_list_first(&p->mqtt_sock->recv_queue)) != NULL) {
		nni_list_remove(&p->mqtt_sock->recv_queue, ctx);
		nni_list_append(&p->recv_queue, ctx);
	}
	while (nni_lmq_get(&p->mqtt_sock->send_messages, &msg) == 0) {
		nni_lmq_put(&p->send_messages, msg);
	}
	nni_mtx_unlock(&p->mqtt_sock->mtx);

	if (!nni_aio_list_active(&p->mqtt_sock->time_aio) && major)
		nni_sleep_aio(p->mqtt_sock->retry, &p->mqtt_sock->time_aio);

	// if (!major && topic != NULL && topic->pipeType == PIPE_TYPE_SUB) {
	// 	p->ready = true;
	// 	nni_atomic_set_bool(&p->closed, false);
	// 	mqtt_sub_stream_start(p, topic->msg, topic->packetid, topic->aio);
	// 	nni_list_remove(&p->mqtt_sock->topicq, topic);
	// } else if (!major && topic != NULL && topic->pipeType == PIPE_TYPE_PUB) {
	// 	mqtt_pub_stream_start(p, topic->msg, topic->aio);
	// 	nni_list_remove(&p->mqtt_sock->topicq, topic);
	// }

	return (0);
}

static void
quic_mqtt_pipe_fini(void *arg)
{
	nni_aio *aio;
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_msg * msg;

	log_warn("quic_mqtt_pipe_fini! pipe finit!");
	if ((msg = nni_aio_get_msg(&p->recv_aio)) != NULL) {
		nni_aio_set_msg(&p->recv_aio, NULL);
		nni_msg_free(msg);
	}
	if ((msg = nni_aio_get_msg(&p->send_aio)) != NULL) {
		nni_aio_set_msg(&p->send_aio, NULL);
		nni_msg_free(msg);
	}

	uint16_t count = 0;
	mqtt_quic_ctx *ctx;

	nni_msg *tmsg = nano_msg_notify(p->cparam, p->reason_code, 1, false);
	nni_msg_set_cmd_type(tmsg, CMD_DISCONNECT_EV);
	// clone once for pub DISCONNECT_EV
	conn_param_clone(p->cparam);
	nni_msg_set_conn_param(tmsg, p->cparam);

	nni_mtx_lock(&p->lk);

	// emulate disconnect notify msg as a normal publish
	while ((ctx = nni_list_first(&p->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&p->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(aio, tmsg);
		// only return pipe closed error once for notification
		// sync action to avoid NULL conn param
		count == 0 ? nni_aio_finish_sync(aio, NNG_ECONNSHUT, 0)
		           : nni_aio_finish_error(aio, NNG_ECLOSED);
		// there should be no msg waiting
		count++;
	}
	if (count == 0) {
		log_warn("disconnect msg of bridging is lost due to no ctx "
		            "on receving");
		nni_msg_free(tmsg);
		conn_param_free(p->cparam);
	}
	nni_lmq_fini(&p->send_messages);
	nni_mtx_unlock(&p->lk);

	// hold nni_sock twice for thread safety
	nni_sock_hold(s->nsock);
	nni_sock_hold(s->nsock);

	nni_mtx_lock(&s->mtx);

	if (p->pingmsg)
		nni_msg_free(p->pingmsg);

	nni_aio_fini(&p->send_aio);
	nni_aio_fini(&p->recv_aio);
	nni_aio_fini(&p->rep_aio);
	nni_aio_abort(&s->time_aio, 0);

	nni_id_map_fini(&p->recv_unack);
	nni_id_map_fini(&p->sent_unack);
	if (p->mqtt_sock->multi_stream)
		nni_lmq_fini(&p->send_inflight);
	nni_lmq_fini(&p->recv_messages);
	nni_mtx_fini(&p->lk);

	// only report disconnect when major pipe is closed
	if (s->pipe == p && s->cb.disconnect_cb != NULL) {
		s->cb.disconnect_cb(NULL, s->cb.discarg);
	}
	if (p->cparam == NULL || p != s->pipe) {
		// connect failed or data stream close
		// also triggered stream finit, ignore it
		nni_mtx_unlock(&s->mtx);
		nni_sock_rele(s->nsock);
		nni_sock_rele(s->nsock);
		return;
	}

	s->pipe = NULL;

	while ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		if (msg != NULL) {
			nni_msg_free(msg);
		}
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		nni_list_remove(&s->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		msg       = nni_aio_get_msg(aio);
		if (msg != NULL) {
			nni_msg_free(msg);
		}
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	conn_param_free(p->cparam);
	nni_mtx_unlock(&s->mtx);

	nni_sock_rele(s->nsock);
	nni_sock_rele(s->nsock);
}

/**
 * If main stream: deal with cached aio in send_queue
 * If Data stream: send first msg in topic_lmq then bind
*/
static int
quic_mqtt_pipe_start(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_pipe    *npipe = p->qpipe;
	nni_aio     *aio;
	nni_msg     *msg;
	int          rv = 0;

	nni_mtx_lock(&s->mtx);
	// p_dialer is not available when pipe init and sock init. Until pipe start.
	p->ready = true;
	nni_atomic_set_bool(&p->closed, false);
	if (p == s->pipe) {
		// It is main stream, deal with cached send aio
		if ((aio = nni_list_first(&s->send_queue)) != NULL) {
			nni_list_remove(&s->send_queue, aio);
			msg    = nni_aio_get_msg(aio);
			int rv = 0;
			if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
				nni_mtx_unlock(&s->mtx);
				nni_aio_finish(aio, rv, 0);
				nni_pipe_recv(npipe, &p->recv_aio);
				return 0;
			}
		}
	} else {
		rv = quic_mqtt_stream_bind(s, p, npipe);
	}

	nni_mtx_unlock(&s->mtx);
	if (rv == 0)
		nni_pipe_recv(npipe, &p->recv_aio);
	else
		nni_pipe_close(npipe);
	return 0;
}


static void
quic_mqtt_pipe_stop(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_msg *msg;

	log_info("Stopping MQTT over QUIC Stream");
	if (!nni_atomic_get_bool(&p->closed)) {
		nni_aio_stop(&p->send_aio);
		nni_aio_stop(&p->recv_aio);
		nni_aio_abort(&p->rep_aio, NNG_ECANCELED);
		nni_aio_finish_error(&p->rep_aio, NNG_ECANCELED);
		nni_aio_stop(&p->rep_aio);
		nni_aio_stop(&s->time_aio);
	}
	if (p != s->pipe) {
		// close & finit data stream
		log_warn("close data stream of topic");
		nni_atomic_set_bool(&p->closed, true);
		nni_mtx_lock(&s->mtx);

		nni_aio_close(&p->send_aio);
		nni_aio_close(&p->recv_aio);
		nni_aio_close(&p->rep_aio);
		nni_lmq_flush(&p->recv_messages);
		nni_lmq_flush(&p->send_inflight);

		p->ready = false;

		if ((msg = nni_aio_get_msg(&p->recv_aio)) != NULL) {
			nni_aio_set_msg(&p->recv_aio, NULL);
			nni_msg_free(msg);
		}
		if ((msg = nni_aio_get_msg(&p->send_aio)) != NULL) {
			nni_aio_set_msg(&p->send_aio, NULL);
			nni_msg_free(msg);
		}

		nni_aio_fini(&p->send_aio);
		nni_aio_fini(&p->recv_aio);
		nni_aio_fini(&p->rep_aio);
		nni_id_map_fini(&p->recv_unack);
		nni_id_map_fini(&p->sent_unack);
		nni_lmq_fini(&p->send_inflight);
		nni_lmq_fini(&p->recv_messages);
		nni_mtx_fini(&p->lk);

		nni_mtx_unlock(&s->mtx);
	}
}
// main stream close
static int
quic_mqtt_pipe_close(void *arg)
{
	log_info(" ##### quic_mqtt_pipe_close ##### ");
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	if (p != s->pipe) {
		log_error("close data pipe in main stream close cb!");
		return -1;
	} else if (nni_atomic_get_bool(&p->closed)) {
		log_error("double close pipe!");
		return 0;
	}

	nni_atomic_set_bool(&s->pipe->closed, true);

	nni_mtx_lock(&p->lk);
	nni_atomic_set_bool(&p->closed, true);
	p->ready = false;

	nni_id_map_foreach(&p->sent_unack, mqtt_close_unack_msg_cb);
	nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);

	nni_aio_close(&p->send_aio);
	nni_aio_close(&p->recv_aio);
	nni_aio_close(&p->rep_aio);
#if defined(NNG_SUPP_SQLITE)
	if (!nni_lmq_empty(&p->send_messages)) {
		log_info("cached msg into sqlite");
		sqlite_flush_lmq(
		    mqtt_quic_sock_get_sqlite_option(s), &p->send_messages);
	}
#endif
	nni_lmq_flush(&p->recv_messages);
	nni_lmq_flush(&p->send_messages);
	if (p->mqtt_sock->multi_stream)
		nni_lmq_flush(&p->send_inflight);
	nni_mtx_unlock(&p->lk);

	nni_mtx_lock(&s->mtx);

	if (s->pipe != p) {
		if (p->idmsg == NULL)
			log_warn("NULL idmsg found, Fail to clean up QUIC Stream");
		if (nni_mqtt_msg_get_packet_type(p->idmsg) ==
		    NNG_MQTT_SUBSCRIBE) {
			// packet id is already encoded
			nni_mqtt_topic_qos *topics;
			uint32_t            count;
			topics = nni_mqtt_msg_get_subscribe_topics(
			    p->idmsg, &count);
			// Create a new stream no matter how many topics in Sub
			for (uint32_t i = 0; i < count; i++) {
				// Sub on same topic twice gonna replace old
				// pipe with new This may result in potentioal
				// memleak
				nni_id_remove(s->sub_streams,
				    DJBHashn((char *) topics[i].topic.buf,
				        topics[i].topic.length));
			}
			nni_msg_free(p->idmsg);
		} else if (nni_mqtt_msg_get_packet_type(p->idmsg) ==
		    NNG_MQTT_PUBLISH) {
			uint32_t topic_len;
			char *topic = (char *) nni_mqtt_msg_get_publish_topic(
			    p->idmsg, &topic_len);
			nni_id_remove(
			    s->sub_streams, DJBHashn(topic, topic_len));
			nni_msg_free(p->idmsg);
		}
	}
	nni_mtx_unlock(&s->mtx);

	return 0;
}

/******************************************************************************
 *                             Context Implementation                         *
 ******************************************************************************/

static void
mqtt_quic_ctx_init(void *arg, void *sock)
{
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t   *s   = sock;

	ctx->mqtt_sock = s;
	ctx->raio      = NULL;
	ctx->saio      = NULL;
	NNI_LIST_NODE_INIT(&ctx->sqnode);
	NNI_LIST_NODE_INIT(&ctx->rqnode);
}

static void
mqtt_quic_ctx_fini(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static void
mqtt_quic_ctx_send(void *arg, nni_aio *aio)
{
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t   *s   = ctx->mqtt_sock;
	mqtt_pipe_t   *p;
	nni_pipe      *npipe;
	nni_msg       *msg;
	uint16_t       packet_id;
	int            rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}

	nni_mtx_lock(&s->mtx);
	p = s->pipe;

	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	if (p == NULL || nni_atomic_get_bool(&s->closed) || nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_mqtt_packet_type ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype)
	{
	case NNG_MQTT_PUBLISH:
		if (nni_mqtt_msg_get_publish_qos(msg) == 0) {
			break;
		}
		// fall through
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id = mqtt_get_next_packet_id(&s->next_packet_id);
		nni_mqtt_msg_set_packet_id(msg, packet_id);
		break;
	default:
		break;
	}
	if (nni_mqttv5_msg_encode(msg) != MQTT_SUCCESS) {
		nni_mtx_unlock(&s->mtx);
		log_error("MQTTv5 client encoding msg failed!");
		nni_msg_free(msg);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}

	if (p == NULL || p->ready == false) {
		// connection is lost or not established yet
#if defined(NNG_SUPP_SQLITE)
		if (ptype == NNG_MQTT_PUBLISH) {
			nni_mqtt_sqlite_option *sqlite =
			    mqtt_quic_sock_get_sqlite_option(s);
			if (sqlite_is_enabled(sqlite)) {
				// the msg order is exactly as same as the ctx in send_queue
				nni_lmq_put(&sqlite->offline_cache, msg);
				if (nni_lmq_full(&sqlite->offline_cache)) {
					sqlite_flush_offline_cache(sqlite);
				}
				nni_mtx_unlock(&s->mtx);
				nni_aio_set_msg(aio, NULL);
				nni_aio_finish_error(aio, NNG_ECLOSED);
				return;
			}
		}
#endif
		if (ptype == NNG_MQTT_CONNECT &&
		    !nni_list_active(&s->send_queue, aio)) {
			// cache aio
			nni_list_append(&s->send_queue, aio);
			nni_mtx_unlock(&s->mtx);
		} else {
			// aio is already on the list.
			// caching pubmsg in lmq of sock
			if (ptype == NNG_MQTT_PUBLISH) {
				log_info("caching msg!");
				if (0 != nni_lmq_put(&s->send_messages, msg)) {
					log_warn("caching msg failed due to full lmq!");
					nni_msg_free(msg);
					nni_mtx_unlock(&s->mtx);
					nni_aio_set_msg(aio, NULL);
					nni_aio_finish_error(aio, NNG_EBUSY);
					return;
				}
				nni_mtx_unlock(&s->mtx);
				nni_aio_set_msg(aio, NULL);
				nni_aio_finish(aio, 0, 0);
				return;
			} else {
				nni_msg_free(msg);
				nni_mtx_unlock(&s->mtx);
				nni_aio_set_msg(aio, NULL);
				nni_aio_finish_error(aio, NNG_EBUSY);
			}
		}
		return;
	}

	if (s->multi_stream == false) {
		if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish(aio, rv, 0);
			return;
		}
	} else {
		npipe = p->qpipe;
		nni_mqtt_msg_set_aio(msg, aio);
		if (nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_SUBSCRIBE) {
			nni_lmq_put(s->topic_lmq, msg);
			rv = nni_dialer_start(npipe->p_dialer, NNG_FLAG_NONBLOCK);
			if (rv != 0) {
				nni_aio_finish_error(aio, rv);
				log_error("Error in dialer start (sub) %d", rv);
			}
		} else if (nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH) {
			char *       num_ptr;
			uint32_t     topic_len;
			char *       topic = (char *) nni_mqtt_msg_get_publish_topic(msg, &topic_len);
			mqtt_pipe_t *pub_pipe;
			uint32_t     hash = DJBHashn(topic, topic_len);
			pub_pipe = nni_id_get(s->pub_streams, hash);
			// check if pub stream already exist
			if (pub_pipe == NULL) {
				nni_lmq_put(s->topic_lmq, msg);
				// check if the stream is already dialing
				if (NULL == (num_ptr = nni_id_get(s->topic_map, hash))) {
					nni_id_set(s->topic_map, hash, (void *)s);
					/*
					 * You can set dialer quic priority before
					 * dialer_start to create a stream with the
					 * specified priority.
					 */
					//nni_dialer_setopt(npipe->p_dialer, NNG_OPT_MQTT_QUIC_PRIORITY, &val, sizeof(int), NNI_TYPE_INT32);
					rv = nni_dialer_start(npipe->p_dialer, NNG_FLAG_NONBLOCK);
					if (rv != 0) {
						nni_aio_finish_error(aio, rv);
						log_error("Error in dialer start (pub) %d", rv);
					}
				} else {
					nni_id_set(s->topic_map, hash, (void *)(num_ptr + 1));
				}
			} else {
				if ((rv = mqtt_pipe_send_msg(aio, msg, pub_pipe, 0)) >= 0) {
					nni_mtx_unlock(&s->mtx);
					nni_aio_finish(aio, rv, 0);
					return;
				}
			}
		} else {
			log_warn("invalid msg type 0x%x", nni_mqtt_msg_get_packet_type(msg));
		}
	}
	nni_mtx_unlock(&s->mtx);
	return;
}

static void
mqtt_quic_ctx_recv(void *arg, nni_aio *aio)
{
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t   *s   = ctx->mqtt_sock;
	mqtt_pipe_t   *p;
	nni_msg       *msg = NULL;
	int            rv;

	if ((rv = nni_aio_begin(aio)) != 0) {
		log_error("aio begin failed %d", rv);
		return;
	}

	nni_mtx_lock(&s->mtx);
	p = s->pipe;
	// TODO Should socket is closed be check first?
	if (p == NULL) {
		goto wait;
	}

	if (nni_atomic_get_bool(&s->closed)) {
		nni_mtx_unlock(&s->mtx);
		log_info("recv action on closed socket!");
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	if (aio == s->ack_aio) {
		if (nni_lmq_get(s->ack_lmq, &msg) == 0) {
			nni_aio_set_msg(aio, msg);
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish_msg(aio, msg);
			return;
		}
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish(aio, NNG_ECANCELED, 0);
		return;
	}

	nni_mtx_lock(&p->lk);
	if (nni_lmq_get(&p->recv_messages, &msg) == 0) {
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&p->lk);
		nni_mtx_unlock(&s->mtx);
		//let user gets a quick reply
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}
	// no msg available
	if (ctx->raio != NULL) {
		nni_mtx_unlock(&p->lk);
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		log_warn("ERROR! former aio not finished!");
		nni_aio_finish_error(aio, NNG_ESTATE);
		return;
	}
	// cache aio
	ctx->raio = aio;
	nni_list_append(&p->recv_queue, ctx);
	nni_mtx_unlock(&p->lk);
	nni_mtx_unlock(&s->mtx);
	return;

	// no open pipe
wait:
	if (ctx->raio != NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		log_warn("ERROR! former aio not finished!");
		nni_aio_finish_error(aio, NNG_ESTATE);
		return;
	}
	// cache aio
	ctx->raio = aio;
	nni_list_append(&s->recv_queue, ctx);
	nni_mtx_unlock(&s->mtx);
	return;
}

static nni_proto_pipe_ops mqtt_quic_pipe_ops = {
	.pipe_size  = sizeof(mqtt_pipe_t),
	.pipe_init  = quic_mqtt_pipe_init,
	.pipe_fini  = quic_mqtt_pipe_fini,
	.pipe_start = quic_mqtt_pipe_start,
	.pipe_close = quic_mqtt_pipe_close,
	.pipe_stop  = quic_mqtt_pipe_stop,
};

static nni_option mqtt_quic_ctx_options[] = {
	{
	    .o_name = NULL,
	},
};

static nni_proto_ctx_ops mqtt_quic_ctx_ops = {
	.ctx_size    = sizeof(mqtt_quic_ctx),
	.ctx_options = mqtt_quic_ctx_options,
	.ctx_init    = mqtt_quic_ctx_init,
	.ctx_fini    = mqtt_quic_ctx_fini,
	.ctx_recv    = mqtt_quic_ctx_recv,
	.ctx_send    = mqtt_quic_ctx_send,
};

static nni_option mqtt_quic_sock_options[] = {
	{
	    .o_name = NNG_OPT_MQTT_SQLITE,
	    .o_set  = mqtt_quic_sock_set_sqlite_option,
	},
	{
	    .o_name = NNG_OPT_QUIC_ENABLE_MULTISTREAM,
	    .o_set  = mqtt_quic_sock_set_multi_stream,
	},
	{
	    .o_name = NNG_OPT_MQTT_BRIDGE_CONF,
	    .o_set  = mqtt_quic_sock_set_bridge_config,
	},
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
	.sock_send    = mqtt_quic_sock_send,
	.sock_recv    = mqtt_quic_sock_recv,
};

static nni_proto mqtt_quic_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MQTT_SELF, NNG_MQTT_SELF_NAME },
	.proto_peer     = { NNG_MQTT_PEER, NNG_MQTT_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mqtt_quic_sock_ops,
	.proto_pipe_ops = &mqtt_quic_pipe_ops,
	.proto_ctx_ops  = &mqtt_quic_ctx_ops,
};

int
nng_mqttv5_quic_client_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mqtt_quic_proto));
}

int
nng_mqttv5_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.connect_cb = cb;
		mqtt_sock->cb.connarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqttv5_quic_set_msg_send_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.msg_send_cb = cb;
		mqtt_sock->cb.sendarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqttv5_quic_set_msg_recv_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.msg_recv_cb = cb;
		mqtt_sock->cb.recvarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqttv5_quic_set_disconnect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.disconnect_cb = cb;
		mqtt_sock->cb.discarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

/*
// As taking msquic as tranport, we exclude the dialer for now.
int
nng_mqtt_quic_client_open(nng_socket *sock, const char *url)
{
	return nng_mqtt_quic_open_conf(sock, url, NULL);
}

// open mqtt quic transport with self-defined conf params
int
nng_mqtt_quic_open_conf(nng_socket *sock, const char *url, void *node)
{
	int       rv = 0;
	nni_sock *nsock = NULL;
	void     *qsock = NULL;

	mqtt_sock_t *msock = NULL;

	// Quic settings
	if ((rv = nni_proto_open(sock, &mqtt_msquic_proto)) == 0) {
		nni_sock_find(&nsock, sock->id);
		if (nsock) {
			// set bridge conf
			nng_mqtt_quic_set_config(sock, node);
			quic_open();
			quic_proto_open(&mqtt_msquic_proto);
			quic_proto_set_bridge_conf(node);
			rv = quic_connect_ipv4(url, nsock, NULL, &qsock);
			if (rv == 0) {
				msock = nni_sock_proto_data(nsock);
				msock->qsock = qsock;
			}
		} else {
			rv = -1;
		}
	}
	nni_sock_rele(nsock);
	return rv;
}

int
nng_mqtt_quic_client_close(nng_socket *sock)
{
	nni_sock *nsock = NULL;
	mqtt_sock_t *s= NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		s= nni_sock_proto_data(nsock);
		if (!s)
			return -1;
		if (s->pipe && s->pipe->qpipe) {
			quic_disconnect(s->qsock, s->pipe->qpipe);
		} else {
			quic_disconnect(s->qsock, NULL);
		}

		// nni_sock_close(nsock);
		nni_sock_rele(nsock);

		return 0;
	}


	return -2;
}

// init an AIO for Acknoledgement message only, in order to make QoS/connect truly asychrounous
// For QoS 0 message, we do not care the result of sending
// valid with Connack + puback + pubcomp
// return 0 if set callback sucessfully
int
nng_mqtt_quic_ack_callback_set(nng_socket *sock, void (*cb)(void *), void *arg)
{
	nni_sock *nsock = NULL;
	nni_aio  *aio;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		if ((aio = NNI_ALLOC_STRUCT(aio)) == NULL) {
			return (NNG_ENOMEM);
		}
		nni_aio_init(aio, (nni_cb) cb, aio);
		nni_aio_set_prov_data(aio, arg);
		mqtt_sock->ack_aio = aio;
		mqtt_sock->ack_lmq = nni_alloc(sizeof(nni_lmq));
		nni_lmq_init(mqtt_sock->ack_lmq, NNG_MAX_RECV_LMQ);
	} else {
		nni_sock_rele(nsock);
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqtt_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.connect_cb = cb;
		mqtt_sock->cb.connarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqtt_quic_set_disconnect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.disconnect_cb = cb;
		mqtt_sock->cb.discarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqtt_quic_set_msg_recv_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.msg_recv_cb = cb;
		mqtt_sock->cb.recvarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}

int
nng_mqtt_quic_set_msg_send_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.msg_send_cb = cb;
		mqtt_sock->cb.sendarg = arg;
	} else {
		return -1;
	}
	nni_sock_rele(nsock);
	return 0;
}
*/
