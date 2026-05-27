//
// Copyright 2024 NanoMQ Team, Inc. <jaylin@emqx.io>
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
#include "core/platform.h"
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

static int g_qos0_sent = 0;
static int g_qos_sent  = 0;
static int g_qos_acked = 0;

static int prior_flags = QUIC_HIGH_PRIOR_MSG;

static void mqtt_quic_sock_init(void *arg, nni_sock *sock);
static void mqttv5_quic_sock_init(void *arg, nni_sock *sock);
static void mqtt_quic_sock_fini(void *arg);
static void mqtt_quic_sock_open(void *arg);
static void mqtt_quic_sock_send(void *arg, nni_aio *aio);
static void mqtt_quic_sock_recv(void *arg, nni_aio *aio);
static void mqtt_quic_send_cb(void *arg);
static void mqtt_quic_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

static int  mqtt_quic_pipe_init(void *arg, nni_pipe *qstrm, void *sock);
static void mqtt_quic_pipe_fini(void *arg);
static int  mqtt_quic_pipe_start(void *arg);
static void mqtt_quic_pipe_stop(void *arg);
static int  mqtt_quic_pipe_close(void *arg);

static void mqtt_quic_ctx_init(void *arg, void *sock);
static void mqtt_quic_ctx_fini(void *arg);
static void mqtt_quic_ctx_recv(void *arg, nni_aio *aio);
static void mqtt_quic_ctx_send(void *arg, nni_aio *aio);
static void mqtt_quic_cancel_send(nni_aio *aio, void *arg, int rv);

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
	uint8_t         mqtt_ver; // mqtt version.
	bool            multi_stream;
	bool            qos_first;
	bool            retry_qos_0;
	nni_mtx         mtx; // more fine grained mutual exclusion
	nni_atomic_bool closed;
	nni_atomic_int  next_packet_id; // next packet id to use, shared by multiple pipes
	nni_duration  retry;
	nni_duration  keepalive;       // mqtt keepalive
	nni_duration  timeleft;        // left time to send next ping
	reason_code   disconnect_code; // disconnect reason code
	property      *dis_prop;        // disconnect property
	nni_id_map    sent_unack;      // unacknowledged sent     messages
	mqtt_quic_ctx master;          // to which we delegate send/recv calls
	nni_list      recv_queue;      // ctx pending to receive
	nni_list      send_queue; // aio pending to send, not ctx!
	nni_sock     *nsock;
	nni_lmq      *ack_lmq;  // created for ack aio callback
	mqtt_pipe_t  *pipe;     // the major pipe (control stream)
	                   // main quic pipe, others needs a map to store the
	                   // relationship between MQTT topics and quic pipes

	nni_mqtt_sqlite_option *sqlite_opt;
	conf_bridge_node       *bridge_conf;

	struct mqtt_client_cb cb; // user cb
	
	nni_time retry_wait;
	uint8_t  timeout_backoff;
#ifdef NNG_ENABLE_STATS
	nni_stat_item st_rcv_max;
	nni_stat_item mqtt_reconnect;
	nni_stat_item msg_resend;
	nni_stat_item msg_send_drop;
	nni_stat_item msg_recv_drop;
	nni_stat_item msg_bytes_cached;
#endif
};

// A mqtt_pipe_s is our per-stream protocol private structure.
// equal to self-defined pipe in other protocols
struct mqtt_pipe_s {
	nni_mtx         lk;
	void           *qpipe; // QUIC version of nni_pipe
	bool            busy;
	bool            ready;			// mark if QUIC stream is ready
	mqtt_sock_t    *mqtt_sock;
	nni_id_map      recv_unack;    // unacknowledged received messages
	nni_aio         send_aio;      // send aio to the underlying transport
	nni_aio         recv_aio;      // recv aio to the underlying transport
	nni_aio   		time_aio; // timer aio to resend unack msg
	nni_lmq         recv_messages; // recv messages queue
	nni_lmq         send_messages; // send messages queue
	uint16_t        rid;           // index of resending packet id
	uint8_t         reason_code;   // MQTTV5 reason code
	nni_atomic_bool closed;
	uint8_t         pingcnt;
	nni_msg        *pingmsg;
#ifdef NNG_HAVE_MQTT_BROKER
	conn_param *cparam;
#endif
};

// QUIC is still retrying, Although we abort sending in protocol layer.
static void
mqtt_quic_cancel_send(nni_aio *aio, void *arg, int rv)
{
	NNI_ARG_UNUSED(rv);
	nni_msg             *msg, *tmsg;
	uint16_t             packet_id;
	mqtt_sock_t         *s   = arg;
	mqtt_pipe_t         *p;
	nni_mqtt_proto_data *proto_data;

	nni_mtx_lock(&s->mtx);
	msg = nni_aio_get_msg(aio);
	if (msg != NULL) {
		// deal with canceld QoS msg
		proto_data = nni_msg_get_proto_data(msg);
		if (proto_data) {
			uint8_t type = proto_data->fixed_header.common.packet_type;
			if (type == NNG_MQTT_PUBLISH)
				packet_id = proto_data->var_header.publish.packet_id;
			else if (type == NNG_MQTT_SUBSCRIBE)
				packet_id = proto_data->var_header.subscribe.packet_id;
			else if (type == NNG_MQTT_UNSUBSCRIBE)
				packet_id = proto_data->var_header.unsubscribe.packet_id;
			p = s->pipe;
			if (p != NULL) {
				tmsg = nni_id_get(&s->sent_unack, packet_id);
				if (tmsg != msg)
					log_warn("QoS msg got overwritten!");
				if (tmsg != NULL) {
					log_warn("Warning : QoS action of msg %d is canceled due to "
									"timeout!", packet_id);
					nni_id_remove(&s->sent_unack, packet_id);
					nni_aio_set_msg(aio, NULL);
					nni_mqtt_msg_set_aio(tmsg, NULL);
					nni_msg_free(tmsg);
#ifdef NNG_ENABLE_STATS
					nni_stat_inc(&p->mqtt_sock->msg_send_drop, 1);
#endif
				} else
					log_warn("canceling QoS aio, however msg is lost!");
				nni_aio_finish_error(aio, NNG_ECANCELED);
			}
		}
	}

	if (nni_list_active(&s->send_queue, aio)) {
		nni_list_remove(&s->send_queue, aio);
	}
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
	}
	nni_mtx_unlock(&s->mtx);
}

//need to free msg
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

			if (0 != nni_lmq_resize(
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
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&p->mqtt_sock->msg_recv_drop, 1);
#endif
		return -1;
	}
	return 0;
}

// Should be called with sock mutex locked after pipe is secured
// flag indicates if need to skip msg in sqlite 1: check sqlite 0: only aio
static int
mqtt_quic_send_msg(nni_aio *aio, mqtt_sock_t *s)
{
	mqtt_pipe_t *p   = s->pipe;
	nni_msg     *tmsg, *msg = NULL;
	uint16_t     ptype, packet_id;
	uint8_t      qos = 0;

	if (p == NULL || nni_atomic_get_bool(&p->closed) || aio == NULL) {
		//pipe closed, should never gets here
		// sending msg on a closed pipe
		goto out;
	}
	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		// Not gonna hit this.
#if defined(NNG_SUPP_SQLITE)
		nni_mqtt_sqlite_option *sqlite =
		    mqtt_quic_sock_get_sqlite_option(s);
		if (sqlite_is_enabled(sqlite)) {
			if (!nni_lmq_empty(&sqlite->offline_cache)) {
				sqlite_flush_offline_cache(sqlite);
			}
			msg = sqlite_get_cache_msg(sqlite);
		}
#endif
		if (msg == NULL) {
			goto out;
		}
	}

	nni_msg_set_timestamp(msg, nni_clock());
	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
		// impossible to reach here
	case NNG_MQTT_DISCONNECT:
	case NNG_MQTT_PUBACK:
	case NNG_MQTT_PUBREC:
	case NNG_MQTT_PUBREL:
	case NNG_MQTT_PUBCOMP:
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
		log_debug("send msg id %d", packet_id);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_id_get(&s->sent_unack, packet_id);
		if (tmsg != NULL) {
			log_warn("msg %p packetID %d conflict due to duplicated!",tmsg, packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio) {
				//Potential aio racing bug
				if (nni_aio_list_active(m_aio))
					nni_aio_finish_error(m_aio, NNG_ECANCELED);
			}
			nni_id_remove(&s->sent_unack, packet_id);
			nni_msg_free(tmsg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
		}
		// cache QoS msg with packetid for potential resending
		nni_msg_clone(msg);
		if (0 != nni_id_set(&s->sent_unack, packet_id, msg)) {
			log_warn("QoS msg caching failed. send aborted");
			nni_aio_set_msg(aio, NULL);
			nni_mqtt_msg_set_aio(msg, NULL);
			nni_mtx_unlock(&s->mtx);
			nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
			nni_aio_finish_error(aio, NNG_ECANCELED);
			return NNG_ECANCELED;
		} else if (!s->qos_first) {
			// Cancel Timeout is simply not compatible with QoS first logic
			int rv;
			if ((rv = nni_aio_schedule(aio, mqtt_quic_cancel_send, s)) != 0) {
				log_warn("Cancel_Func scheduling failed, send abort!");
				nni_id_remove(&s->sent_unack, packet_id);
				nni_aio_set_msg(aio, NULL);
				nni_mqtt_msg_set_aio(msg, NULL);
				nni_mtx_unlock(&s->mtx);
				nni_msg_free(msg);	// User need to realloc this msg again
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
				nni_aio_finish_error(aio, rv);
				return NNG_ECANCELED;
			}
		}
		break;
	default:
		log_error("Undefined msg type");
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_aio_finish_error(aio, NNG_EPROTO);
		return NNG_EPROTO;
	}
	if (s->qos_first)
		if (ptype == NNG_MQTT_SUBSCRIBE || ptype == NNG_MQTT_UNSUBSCRIBE ||
		   (qos > 0 && ptype == NNG_MQTT_PUBLISH)) {
			nni_aio_set_msg(aio, msg);
			// this make cancel_send impossible
			nni_aio_set_prov_data(aio, &prior_flags);
			g_qos_sent ++;
			nni_pipe_send(p->qpipe, aio);
			log_debug("sending high priority QoS msg %d in parallel", packet_id);
			nni_mtx_unlock(&s->mtx);
			return 0;
		}

	if (!p->busy) {
		nni_aio_set_msg(&p->send_aio, msg);
		p->busy = true;
		g_qos0_sent ++;
		nni_pipe_send(p->qpipe, &p->send_aio);
	} else {
		if (s->retry_qos_0 || qos > 0) {
			if (nni_lmq_full(&p->send_messages)) {
				//log_warn("Cached Message lost! pipe is busy and lmq is full\n");
				(void) nni_lmq_get(&p->send_messages, &tmsg);
				nni_msg_free(tmsg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
			}
			if (0 != nni_lmq_put(&p->send_messages, msg)) {
				nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
				log_warn("Message lost while enqueing");
			}
			// let qos msg retry/cancel take care of user aio
		} else {
			nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
			log_info("drop qos 0 msg due to busy aio");
		}
	}
out:
	nni_mtx_unlock(&s->mtx);
	if (0 == qos && ptype != NNG_MQTT_SUBSCRIBE &&
		ptype != NNG_MQTT_UNSUBSCRIBE) {
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish(aio, 0, 0);
	}
	return 0;
}

static void
mqtt_quic_send_cb(void *arg)
{
	mqtt_pipe_t *p   = arg;
	mqtt_sock_t *s   = p->mqtt_sock;
	nni_msg     *msg = NULL;
	nni_aio     *aio;
	int          rv;

	if ((rv = nni_aio_result(&p->send_aio)) != 0) {
		// We failed to send... clean up and deal with it.
		log_warn("fail to send on aio %p rv %d", &p->send_aio, rv);
		nni_msg_free(nni_aio_get_msg(&p->send_aio));
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		if (rv == NNG_ECANCELED) {
			// msg is already be freed in QUIC transport
			nni_aio_set_msg(&p->send_aio, NULL);
			p->busy = false;
			return;
		} else if (rv == NNG_ECONNABORTED || rv == SERVER_SHUTTING_DOWN) {
			s->disconnect_code = SERVER_UNAVAILABLE;
			nni_pipe_close(p->qpipe);
			return;
		} else {
			// what to do when send on substream failed?
			log_warn("send msg on closed pipe failed %d", rv);
			return;
		}
	}
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		// This occurs if the mqtt_pipe_close has been called.
		// In that case we don't want any more processing.
		log_warn("send msg on closed pipe failed %d", rv);
		return;
	}
	nni_mtx_lock(&s->mtx);
	s->timeleft = s->keepalive;

	// Check cached aio first
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		p->busy = false;
		mqtt_quic_send_msg(aio, s);
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}
	// Check cached msg in lmq later
	// this msg is already processed by mqtt_quic_send_msg
	if (nni_lmq_get(&p->send_messages, &msg) == 0) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		// Only QoS > 0 went into lmq
		g_qos_sent ++;
		nni_pipe_send(p->qpipe, &p->send_aio);
		nni_mtx_unlock(&s->mtx);
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}

#if defined(NNG_SUPP_SQLITE)
	// only quic send SQLite cached msg in send cb
	nni_mqtt_sqlite_option *sqlite = mqtt_quic_sock_get_sqlite_option(s);
	if (sqlite_is_enabled(sqlite)) {
		if (!nni_lmq_empty(&sqlite->offline_cache)) {
			sqlite_flush_offline_cache(sqlite);
		}
		if (NULL != (msg = sqlite_get_cache_msg(sqlite))) {
			p->busy = true;
			nni_aio_set_msg(&p->send_aio, msg);
			nni_pipe_send(p->qpipe, &p->send_aio);
			nni_mtx_unlock(&s->mtx);
			return;
		}
	}
#endif

	nni_aio_set_msg(&p->send_aio, NULL);
	p->busy = false;
	nni_mtx_unlock(&s->mtx);
	if (s->cb.msg_send_cb)
		s->cb.msg_send_cb(NULL, s->cb.sendarg);
	return;
}

static void
mqtt_quic_recv_cb(void *arg)
{
	mqtt_pipe_t *p          = arg;
	mqtt_sock_t *s          = p->mqtt_sock;
	nni_aio     *user_aio   = NULL;
	nni_msg     *cached_msg = NULL;
	nni_msg     *msg        = NULL;
	int          rv = 0;

	rv = nni_aio_result(&p->recv_aio);
	if (rv != 0) {
		log_warn("MQTT client recv error %d!", rv);
		if (rv == NNG_ECONNABORTED) {
			if (s->disconnect_code == SUCCESS) {
				s->disconnect_code = SERVER_SHUTTING_DOWN;
			}
			nni_pipe_close(p->qpipe);
		} else if (rv == NNG_ECANCELED) {
			log_info("Sub Stream stopped, keep receving");
			nni_pipe_recv(p->qpipe, &p->recv_aio);
		} else {
			nni_pipe_close(p->qpipe);
		}
		return;
	}

	nni_mtx_lock(&s->mtx);
	msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		log_info("start recv due to closed pipe");
		// free msg and dont return data when pipe is closed.
		if (msg) {
			nni_msg_free(msg);
		}
		nni_mtx_unlock(&s->mtx);
		return;
	}
	if (msg == NULL) {
		nni_pipe_recv(p->qpipe, &p->recv_aio);
		nni_mtx_unlock(&s->mtx);
		return;
	}
	nni_mqtt_msg_proto_data_alloc(msg);
	int (*decode_func)(nni_msg *);
	int (*encode_func)(nni_msg *);
	if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
		decode_func = nni_mqttv5_msg_decode;
		encode_func = nni_mqttv5_msg_encode;
	} else {
		decode_func = nni_mqtt_msg_decode;
		encode_func = nni_mqtt_msg_encode;
	}
	if ((rv = decode_func(msg)) != MQTT_SUCCESS) {
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_recv_drop, 1);
#endif
		// Msg should be clear if decode failed. We reuse it to send disconnect.
		// Or it would encode a malformed packet.
		if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
			nni_mqtt_msg_set_packet_type(msg, NNG_MQTT_DISCONNECT);
			nni_mqtt_msg_set_disconnect_reason_code(msg, rv);
			nni_mqtt_msg_set_disconnect_property(msg, NULL);
			// Composed a disconnect msg
			if ((rv = encode_func(msg)) != MQTT_SUCCESS) {
				log_error("Error in encoding disconnect.\n");
				nni_msg_free(msg);
				nni_mtx_unlock(&s->mtx);
				nni_pipe_close(p->qpipe);
				return;
			}
			if (!p->busy) {
				p->busy = true;
				nni_aio_set_msg(&p->send_aio, msg);
				nni_pipe_send(p->qpipe, &p->send_aio);
				nni_mtx_unlock(&s->mtx);
				return;
			}
			if (nni_lmq_full(&p->send_messages)) {
				nni_msg     *tmsg;
				(void) nni_lmq_get(&p->send_messages, &tmsg);
				log_warn("cached msg lost due to flight window is full");
				nni_msg_free(tmsg);
			}
			if (0 != nni_lmq_put(&p->send_messages, msg)) {
				nni_msg_free(msg);
				log_warn("Disconnect msg send failed due to busy socket");
			}
			nni_mtx_unlock(&s->mtx);
			// close pipe in send cb
			return;	
		} else {
			nni_mtx_unlock(&s->mtx);
			nni_msg_free(msg);
			nni_pipe_close(p->qpipe);
			return;
		}
	}

	packet_type_t  packet_type = nni_mqtt_msg_get_packet_type(msg);
	int32_t        packet_id;
	uint8_t        qos;
	mqtt_quic_ctx *ctx;

	//Schedule another receive
	nni_pipe_recv(p->qpipe, &p->recv_aio);

	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		// Clone CONNACK for connect_cb & user aio cb
		if (s->cb.connect_cb) {
			nni_msg_clone(msg);
		}
		break;
	case NNG_MQTT_PUBACK:
		// we have received a PUBACK, successful delivery of a QoS 1
		// FALLTHROUGH
	case NNG_MQTT_PUBCOMP:
		// we have received a PUBCOMP, successful delivery of a QoS 2
	case NNG_MQTT_SUBACK:
		// we have received a SUBACK, successful subscription
		// FALLTHROUGH
	case NNG_MQTT_UNSUBACK:
		g_qos_acked ++;
		// we have received a UNSUBACK, successful unsubscription
		packet_id  = nni_mqtt_msg_get_packet_id(msg);
		log_debug("get ack msg id %d", packet_id);
		p->rid ++;
		cached_msg = nni_id_get(&s->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&s->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			nni_aio_set_msg(user_aio, NULL);
			nni_mqtt_msg_set_aio(cached_msg, NULL);
			log_info("acked msg %p packetid %d latency %d", cached_msg,
					packet_id, nni_clock() - nni_msg_get_timestamp(cached_msg));
			nni_msg_free(cached_msg);
			// Only return ACK msg of Sub action, nor Pub action is supported now.
			if (packet_type == NNG_MQTT_SUBACK ||
			    packet_type == NNG_MQTT_UNSUBACK)
				if (user_aio != NULL) {
					// should we support sub/unsub cb here?
					nni_msg_clone(msg);
					nni_aio_set_msg(user_aio, msg);
				}
		} else {
			log_warn("QoS msg ack failed %d", packet_id);
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
		if ((ctx = nni_list_first(&s->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(p, cached_msg)) {
				nni_msg_free(cached_msg);
				log_warn("ERROR: no ctx found! msg queue full! QoS2 msg lost!");
				cached_msg = NULL;
			}
			break;
		}
		nni_list_remove(&s->recv_queue, ctx);
		user_aio  = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(user_aio, cached_msg);
		break;
	case NNG_MQTT_PUBLISH:
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (2 > qos) {
			// No one waiting to receive yet, putting msginto lmq
			if ((ctx = nni_list_first(&s->recv_queue)) == NULL) {
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
			nni_list_remove(&s->recv_queue, ctx);
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
				log_error("ERROR: packet id %d duplicate", packet_id);
				// if ((aio = nni_mqtt_msg_get_aio(cached_msg)) != NULL) {
				// 	nng_aio_finish_error(aio, NNG_EEXIST);
				// 	nni_mqtt_msg_set_aio(cached_msg, NULL);
				// }
				nni_msg_free(cached_msg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_recv_drop, 1);
#endif
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
			log_debug("received qos 2 msg %p id %d awaits release !", msg, packet_id);
		}
		break;
	case NNG_MQTT_PINGRESP:
		// PINGRESP is ignored in protocol layer
		// Rely on health checker of Quic stream
		nni_msg_free(msg);
		p->pingcnt = 0;
		s->timeleft = s->keepalive;
		nni_mtx_unlock(&s->mtx);
		return;
	case NNG_MQTT_PUBREC:
		// return PUBREL
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
		return;
	case NNG_MQTT_DISCONNECT:
		if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v311) {
			s->disconnect_code = NORMAL_DISCONNECTION;
		} else if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
			s->disconnect_code = *(uint8_t *)nni_msg_body(msg);
		} else {
			log_error("Invalid mqtt version");
		}
		log_info(
		    " Disconnect received from Broker %d", *(uint8_t *)nni_msg_body(msg));
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;

	default:
		// close quic stream
		nni_mtx_unlock(&s->mtx);
		if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v311) {
			s->disconnect_code = PROTOCOL_ERROR;
		} else if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
			s->disconnect_code = MALFORMED_PACKET;
		} else {
			log_error("Invalid mqtt version");
		}
		// unexpected packet type, server misbehaviour
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_recv_drop, 1);
#endif
		nni_pipe_close(p->qpipe);
		return;
	}
	if (user_aio) {
		nni_aio_finish(user_aio, 0, 0);
	}
	nni_mtx_unlock(&s->mtx);

	// Trigger connect cb first in case connack being freed
	if (packet_type == NNG_MQTT_CONNACK)
		if (s->cb.connect_cb) {
			s->cb.connect_cb(msg, s->cb.connarg);
		}
	// Trigger publish cb
	if (packet_type == NNG_MQTT_PUBLISH)
		if (s->cb.msg_recv_cb) // Trigger cb
			s->cb.msg_recv_cb(msg, s->cb.recvarg);
	return;
}

// Timer callback, we use it for retransmition.
static void
mqtt_timer_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	if (nng_aio_result(&p->time_aio) != 0) {
		log_warn("sleep aio finish error!");
		return;
	}
	nni_mtx_lock(&s->mtx);
	p = s->pipe;
	if (NULL == p || nni_atomic_get_bool(&p->closed)) {
		// QUIC connection has been shut down
		nni_mtx_unlock(&s->mtx);
		return;
	}
	if (p->pingcnt > s->timeout_backoff) {
		log_warn("MQTT Timeout and disconnect");
		s->disconnect_code = KEEP_ALIVE_TIMEOUT;
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;
	}

	// Update left time to send pingreq
	s->timeleft -= s->retry;
	// Ping would be send at transport layer
	if (!p->busy && p->pingmsg && s->timeleft <= 0) {
		p->busy = true;
		s->timeleft = s->keepalive;
		// send pingreq
		nni_msg_clone(p->pingmsg);
		nni_aio_set_msg(&p->send_aio, p->pingmsg);
		nni_pipe_send(p->qpipe, &p->send_aio);
		p->pingcnt ++;
		nni_mtx_unlock(&s->mtx);
		log_info("Send pingreq (sock%p)(%dms), qos sent%d acked%d q0%d",
				s, s->keepalive, g_qos_sent, g_qos_acked, g_qos0_sent);
		nni_sleep_aio(s->retry, &p->time_aio);
		return;
	}

	// start message resending
	uint16_t   pid;
	nni_msg   *msg = nni_id_get_min(&s->sent_unack, &pid);
	if (msg != NULL) {
		nni_time now  = nni_clock();
		nni_time time = now - nni_msg_get_timestamp(msg);
		if (time > s->retry_wait && msg != NULL) {
			nni_mqtt_packet_type ptype;	//uint16_t
			ptype = nni_mqtt_msg_get_packet_type(msg);
			if (ptype == NNG_MQTT_PUBLISH) {
				uint8_t *header = nni_msg_header(msg);
				*header |= 0X08;
			}
			if (!p->busy) {
				g_qos_sent ++;
				p->busy = true;
				nni_msg_clone(msg);
				nni_aio_set_msg(&p->send_aio, msg);
				log_info("msg packetid%d resend start, sent: qos %d acked %d qos 0 %d",
						pid, g_qos_sent, g_qos_acked, g_qos0_sent);
				nni_pipe_send(p->qpipe, &p->send_aio);
				nni_msg_set_timestamp(msg, now);
				nni_mtx_unlock(&s->mtx);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_resend, 1);
#endif
				nni_sleep_aio(s->retry, &p->time_aio);
				return;
			} else {
				log_info("msg id %d resend canceld due to blocked pipe", pid);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
			}
		}
	}
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
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_resend, 1);
#endif
				nni_sleep_aio(s->retry, &p->time_aio);
				return;
			}
		}
	}
#endif
	nni_mtx_unlock(&s->mtx);
	nni_sleep_aio(s->retry, &p->time_aio);
	return;
}

/* MQTT over Quic Sock */
/******************************************************************************
 *                            Socket Implementation                           *
 ******************************************************************************/

static void mqttv5_quic_sock_init(void *arg, nni_sock *sock)
{
	mqtt_sock_t *s = arg;
	mqtt_quic_sock_init(arg, sock);
	s->mqtt_ver = MQTT_PROTOCOL_VERSION_v5;
}

static void mqtt_quic_sock_init(void *arg, nni_sock *sock)
{
	mqtt_sock_t *s = arg;
	s->nsock       = sock;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);
	nni_atomic_set(&s->next_packet_id, 1);

	// this is a pre-defined timer for global timer
	s->sqlite_opt   = NULL;
	s->qos_first    = false;
	s->multi_stream = false;

	nni_mtx_init(&s->mtx);
	mqtt_quic_ctx_init(&s->master, s);

	s->bridge_conf = NULL;
	s->retry_qos_0 = true;
	// this is "semi random" start for request IDs.
	s->retry      = NNI_SECOND * MQTT_QUIC_RETRTY;
	s->retry_wait = NNI_SECOND * 3;
	s->keepalive  = NNI_SECOND * 10; // default mqtt keepalive
	s->timeleft   = NNI_SECOND * 10;

	s->timeout_backoff = 1;
	// For caching aio
	nni_aio_list_init(&s->send_queue);
	// For caching ctx
	NNI_LIST_INIT(&s->recv_queue, mqtt_quic_ctx, rqnode);

	nni_id_map_init(&s->sent_unack, 0x0000u, 0xffffu, true);

	s->pipe = NULL;
	s->mqtt_ver = MQTT_PROTOCOL_VERSION_v311;

	s->cb.connect_cb = NULL;
	s->cb.disconnect_cb = NULL;
	s->cb.msg_recv_cb = NULL;
	s->cb.msg_send_cb = NULL;

#ifdef NNG_ENABLE_STATS
	static const nni_stat_info mqtt_reconnect = {
		.si_name   = "mqtt_client_reconnect",
		.si_desc   = "MQTT reconnect times",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_EVENTS,
		.si_atomic = true,
	};
	nni_stat_init(&s->mqtt_reconnect, &mqtt_reconnect);
	static const nni_stat_info msg_resend = {
		.si_name   = "mqtt_msg_resend",
		.si_desc   = "MQTT message resend times",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_MESSAGES,
		.si_atomic = true,
	};
	nni_stat_init(&s->msg_resend, &msg_resend);
	static const nni_stat_info msg_send_drop = {
		.si_name   = "mqtt_msg_send_drop",
		.si_desc   = "uplink msg dropped",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_MESSAGES,
		.si_atomic = true,
	};
	nni_stat_init(&s->msg_send_drop, &msg_send_drop);
	static const nni_stat_info msg_recv_drop = {
		.si_name   = "mqtt_msg_recv_drop",
		.si_desc   = "downlink msg dropped",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_MESSAGES,
		.si_atomic = true,
	};
	nni_stat_init(&s->msg_recv_drop, &msg_recv_drop);
	static const nni_stat_info msg_bytes_cached = {
		.si_name   = "mqtt_msg_bytes_cached",
		.si_desc   = "cached msg payload size",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_BYTES,
		.si_atomic = true,
	};
	nni_stat_init(&s->msg_bytes_cached, &msg_bytes_cached);
	nni_sock_add_stat(s->nsock, &s->mqtt_reconnect);
	nni_sock_add_stat(s->nsock, &s->msg_resend);
	nni_sock_add_stat(s->nsock, &s->msg_send_drop);
	nni_sock_add_stat(s->nsock, &s->msg_recv_drop);
	nni_sock_add_stat(s->nsock, &s->msg_bytes_cached);
#endif
}

static void
mqtt_quic_sock_fini(void *arg)
{
	mqtt_sock_t *s = arg;

#if defined(NNG_SUPP_SQLITE) && defined(NNG_HAVE_MQTT_BROKER)
	nni_mqtt_sqlite_db_fini(s->sqlite_opt);
	nng_mqtt_free_sqlite_opt(s->sqlite_opt);
#endif

	log_debug("mqtt_quic_sock_fini %p", s);
	nni_id_map_fini(&s->sent_unack);
	mqtt_quic_ctx_fini(&s->master);
	nni_mtx_fini(&s->mtx);
}

static void
mqtt_quic_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static int
mqtt_quic_sock_get_disconnect_prop(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	int          rv;

	nni_mtx_lock(&s->mtx);
	rv = nni_copyout_ptr(s->dis_prop, v, szp, t);
	nni_mtx_lock(&s->mtx);
	return (rv);
}

static void
mqtt_quic_sock_close(void *arg)
{
	uint16_t count = 0;
	nni_aio *aio;
	nni_msg *msg;
	mqtt_sock_t *s = arg;
	mqtt_quic_ctx *ctx;

	nni_atomic_set_bool(&s->closed, true);
	nni_mtx_lock(&s->mtx);
	nni_sock_hold(s->nsock);

	// emulate disconnect notify msg as a normal publish
	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		// msg       = nni_aio_get_msg(aio);
		// if (msg)
		// 	nni_msg_free(msg);
		// nni_aio_set_msg(aio, NULL);
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
	nni_id_map_foreach(&s->sent_unack, mqtt_close_unack_aio_cb);
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
	NNI_ARG_UNUSED(s);
	NNI_ARG_UNUSED(buf);
	NNI_ARG_UNUSED(sz);
	NNI_ARG_UNUSED(t);
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
		s->retry     = s->bridge_conf->resend_interval;
		s->retry_qos_0 = s->bridge_conf->retry_qos_0;
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

/******************************************************************************
 *                          Stream(PIPE) Implementation                       *
 ******************************************************************************/
// allocate quic stream with pipe
static int
mqtt_quic_pipe_init(void *arg, nni_pipe *pipe, void *sock)
{
	mqtt_pipe_t *p = arg;
	conf_bridge_node *node;

	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, true);
	p->qpipe = pipe;
	p->mqtt_sock   = sock;
	p->rid = 1;
	p->pingcnt = 0;
	p->pingmsg = NULL;
	node = p->mqtt_sock->bridge_conf;

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

	nni_aio_init(&p->send_aio, mqtt_quic_send_cb, p);
	nni_aio_init(&p->recv_aio, mqtt_quic_recv_cb, p);
	nni_aio_init(&p->time_aio, mqtt_timer_cb, p);
	// Packet IDs are 16 bits
	// We start at a random point, to minimize likelihood of
	// accidental collision across restarts.
	nni_id_map_init(&p->recv_unack, 0x0000u, 0xffffu, true);
	if (node == NULL) {
		nni_lmq_init(&p->recv_messages, NNG_MAX_RECV_LMQ);
		nni_lmq_init(&p->send_messages, NNG_MAX_SEND_LMQ);
	} else {
		nni_lmq_init(&p->send_messages, node->max_send_queue_len);
		nni_lmq_init(&p->recv_messages, node->max_recv_queue_len);
	}

	nni_mtx_init(&p->lk);
#ifdef NNG_HAVE_MQTT_BROKER
	p->cparam = NULL;
#endif
	p->ready = false;
	return (0);
}

static void
mqtt_quic_pipe_fini(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_msg * msg;

	log_debug("mqtt_quic_pipe_fini! pipe finit!");
	if ((msg = nni_aio_get_msg(&p->recv_aio)) != NULL) {
		nni_aio_set_msg(&p->recv_aio, NULL);
		nni_msg_free(msg);
	}
	// if ((msg = nni_aio_get_prov_data(&p->recv_aio)) != NULL) {
	// 	nni_aio_set_prov_data(&p->recv_aio, NULL);
	// 	nni_msg_free(msg);
	// }
	// if ((msg = nni_aio_get_msg(&p->send_aio)) != NULL) {
	// 	nni_aio_set_msg(&p->send_aio, NULL);
	// 	nni_msg_free(msg);
	// }
	if (p->pingmsg)
		nni_msg_free(p->pingmsg);

	nni_aio_fini(&p->send_aio);
	nni_aio_fini(&p->recv_aio);
	nni_aio_fini(&p->time_aio);

	nni_id_map_fini(&p->recv_unack);
	nni_lmq_fini(&p->recv_messages);
	nni_lmq_fini(&p->send_messages);
	nni_mtx_fini(&p->lk);

	// hold nni_sock twice for thread safety
	nni_sock_hold(s->nsock);
	nni_sock_hold(s->nsock);
	// pipe_close clears s->pipe before pipe_fini runs.
	if (nni_atomic_get_bool(&p->closed) && s->cb.disconnect_cb != NULL) {
		s->cb.disconnect_cb(NULL, s->cb.discarg);
	}
	nni_sock_rele(s->nsock);
	nni_sock_rele(s->nsock);
}

static int
mqtt_quic_pipe_start(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_pipe    *npipe = p->qpipe;
	nni_aio     *aio;
	bool         notify_connect;

	// p_dialer is not available when pipe init and sock init. Until pipe start.
	nni_mtx_lock(&s->mtx);
	nni_atomic_set_bool(&p->closed, false);
	s->pipe       = p;
	s->disconnect_code = SUCCESS;
	s->dis_prop        = NULL;
	p->ready = true;
	p->busy  = false;
	notify_connect = (s->cb.connect_cb != NULL);

	// deal with cached send aio
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		mqtt_quic_send_msg(aio, s);
		nni_pipe_recv(npipe, &p->recv_aio);
		nni_sleep_aio(s->retry, &p->time_aio);
		if (notify_connect) {
			s->cb.connect_cb(NULL, s->cb.connarg);
		}
		return (0);
	}
	nni_mtx_unlock(&s->mtx);
	// initiate the global resend timer
	nni_sleep_aio(s->retry, &p->time_aio);
	nni_pipe_recv(npipe, &p->recv_aio);
#ifdef NNG_ENABLE_STATS
	nni_stat_inc(&s->mqtt_reconnect, 1);
#endif
	if (notify_connect) {
		s->cb.connect_cb(NULL, s->cb.connarg);
	}
	return 0;
}

static void
mqtt_quic_pipe_stop(void *arg)
{
	mqtt_pipe_t *p = arg;

	log_info("Stopping MQTT over QUIC Stream");
	nni_aio_stop(&p->send_aio);
	nni_aio_stop(&p->recv_aio);
	nni_aio_stop(&p->time_aio);
}

static int
mqtt_quic_pipe_close(void *arg)
{
	log_trace(" ##### mqtt_quic_pipe_close ##### ");
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	if (nni_atomic_get_bool(&p->closed)) {
		log_error("double close pipe!");
		return 0;
	}

	nni_mtx_lock(&s->mtx);
	nni_atomic_set_bool(&p->closed, true);
	p->ready = false;
	s->pipe  = NULL;

	nni_aio_close(&p->send_aio);
	nni_aio_close(&p->recv_aio);
	nni_aio_close(&p->time_aio);
#if defined(NNG_SUPP_SQLITE)
	if (!nni_lmq_empty(&p->send_messages)) {
		log_info("cached msg into sqlite");
		sqlite_flush_lmq(
		    mqtt_quic_sock_get_sqlite_option(s), &p->send_messages);
	}
#endif
	nni_lmq_flush(&p->recv_messages);
	nni_lmq_flush(&p->send_messages);
	//TODO add timeout cancel for msgs in sent_unack
	nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);
	// nni_id_map_foreach(&s->sent_unack, mqtt_close_unack_msg_cb);
#ifdef NNG_HAVE_MQTT_BROKER
	if (p->cparam == NULL) {
		nni_mtx_unlock(&s->mtx);
		return 0;
	}
	nni_aio     *user_aio;
	uint16_t 	 count = 0;
	mqtt_quic_ctx *ctx;
	nni_msg *tmsg = nano_msg_notify(p->cparam, s->disconnect_code, 1, false);
	nni_msg_set_cmd_type(tmsg, CMD_DISCONNECT_EV);
	// clone once for pub DISCONNECT_EV
	conn_param_clone(p->cparam);
	nni_msg_set_conn_param(tmsg, p->cparam);

	// emulate disconnect notify msg as a normal publish
	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed. just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, ctx);
		user_aio       = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(user_aio, tmsg);
		// only return pipe closed error once for notification
		// sync action to avoid NULL conn param
		count == 0 ? nni_aio_finish_sync(user_aio, NNG_ECONNSHUT, 0)
		           : nni_aio_finish_error(user_aio, NNG_ECLOSED);
		// there should be no msg waiting
		count++;
	}
	if (count == 0) {
		log_warn("disconnect msg of bridging is lost due to no ctx "
		            "on receving");
		nni_msg_free(tmsg);
		conn_param_free(p->cparam);
	}
#endif
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
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t   *s   = ctx->mqtt_sock;
	nni_aio *      aio;

	nni_mtx_lock(&s->mtx);
	// send queue only has aio
	if ((aio = ctx->saio) != NULL) {
		if (nni_list_active(&s->send_queue, aio)) {
			ctx->saio = NULL;
			nni_list_remove(&s->send_queue, aio);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
	} // not really needed
	if (nni_list_active(&s->recv_queue, ctx)) {
		if ((aio = ctx->raio) != NULL) {
			ctx->raio = NULL;
			nni_list_remove(&s->recv_queue, ctx);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
	}
	nni_mtx_unlock(&s->mtx);
}

static void
mqtt_quic_ctx_send(void *arg, nni_aio *aio)
{
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t   *s   = ctx->mqtt_sock;
	mqtt_pipe_t   *p;
	nni_msg       *msg;
	uint16_t       packet_id;
	uint8_t        qos = 0;
	int            rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	if (nni_atomic_get_bool(&s->closed)) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_mtx_lock(&s->mtx);

	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	nni_mqtt_packet_type ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype)
	{
	case NNG_MQTT_PUBLISH:
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (qos == 0) {
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
	// check return
	if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v311) {
		rv = nni_mqtt_msg_encode(msg);
	} else if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
		rv = nni_mqttv5_msg_encode(msg);
	} else {
		log_error("Invalid mqtt version");
		rv = PROTOCOL_ERROR;
	}
	if (rv != MQTT_SUCCESS) {
		nni_mtx_unlock(&s->mtx);
		log_error("MQTT client encoding msg failed!");
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	p = s->pipe;
	if (p == NULL || nni_atomic_get_bool(&p->closed) || p->ready == false) {
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
		nni_mtx_unlock(&s->mtx);
		if (qos > 0) {
#ifdef NNG_HAVE_MQTT_BROKER
			if (nng_lmq_full(s->bridge_conf->ctx_msgs)) {
				log_warn("Rolling update overwrites old Message");
				nni_msg *tmsg;
				(void) nng_lmq_get(s->bridge_conf->ctx_msgs, &tmsg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
				nni_msg_free(tmsg);
			}
			if (nng_lmq_put(s->bridge_conf->ctx_msgs, msg) != 0) {
				log_warn("Msg lost! put msg to ctx_msgs failed!");
				nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
			} else {
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_bytes_cached, nni_msg_len(msg));
#endif
			}
#else
			nni_mtx_lock(&s->mtx);
			if (!nni_list_active(&s->send_queue, aio)) {
				// cache aio
				nni_list_append(&s->send_queue, aio);
				nni_mtx_unlock(&s->mtx);
				log_warn("client sending msg while disconnected! cached");
			} else {
				nni_msg_free(msg);
				nni_mtx_unlock(&s->mtx);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
				log_info("aio is already cached! drop qos 0 msg");
			}
#endif
		} else {
			nni_msg_free(msg);
			log_info("Discard QoS 0 msg while disconnected!");
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
		}
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	mqtt_quic_send_msg(aio, s);
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
	if (p == NULL) {
		goto wait;
	}

	if (nni_atomic_get_bool(&s->closed)||
	    nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
		log_info("recv on a closed socket!");
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (nni_lmq_get(&p->recv_messages, &msg) == 0) {
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&s->mtx);
		//let user gets a quick reply
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}
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

static int
mqtt_quic_sock_get_disconnect_code(void *arg, void *v, size_t *sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
	mqtt_sock_t *s = arg;
	int          rv;

	nni_mtx_lock(&s->mtx);
	rv = nni_copyin_int(
	    v, &s->disconnect_code, sizeof(reason_code), 0, 256, t);
	nni_mtx_unlock(&s->mtx);
	return (rv);
}

static int
mqtt_quic_sock_set_retry_interval(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	nni_duration    tmp;
	int rv;

	if ((rv = nni_copyin_ms(&tmp, v, sz, t)) == 0) {
		s->retry = tmp > 600000 ? 360000 : tmp;
	}
	return (rv);
}

static int
mqtt_quic_sock_set_retry_wait(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	nni_time    tmp;
	int rv;

	if ((rv = nni_copyin_u64(&tmp, v, sz, t)) == 0) {
		s->retry_wait = tmp;
	}
	return (rv);
}


static int
mqtt_quic_sock_set_retry_qos_0(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	bool tmp;
	int rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		s->retry_qos_0 = tmp;
	}
	return (rv);
}

static nni_proto_pipe_ops mqtt_quic_pipe_ops = {
	.pipe_size  = sizeof(mqtt_pipe_t),
	.pipe_init  = mqtt_quic_pipe_init,
	.pipe_fini  = mqtt_quic_pipe_fini,
	.pipe_start = mqtt_quic_pipe_start,
	.pipe_close = mqtt_quic_pipe_close,
	.pipe_stop  = mqtt_quic_pipe_stop,
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
	{
	    .o_name = NNG_OPT_MQTT_DISCONNECT_REASON,
	    .o_get  = mqtt_quic_sock_get_disconnect_code,
	},
		{
	    .o_name = NNG_OPT_MQTT_DISCONNECT_PROPERTY,
	    .o_get  = mqtt_quic_sock_get_disconnect_prop,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_QOS_0,
	    .o_set  = mqtt_quic_sock_set_retry_qos_0,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_INTERVAL,
	    .o_set  = mqtt_quic_sock_set_retry_interval,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_WAIT_TIME,
	    .o_set  = mqtt_quic_sock_set_retry_wait,
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

static nni_proto_sock_ops mqttv5_quic_sock_ops = {
	.sock_size    = sizeof(mqtt_sock_t),
	.sock_init    = mqttv5_quic_sock_init,
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

static nni_proto mqttv5_quic_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MQTT_SELF, NNG_MQTT_SELF_NAME },
	.proto_peer     = { NNG_MQTT_PEER, NNG_MQTT_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mqttv5_quic_sock_ops,
	.proto_pipe_ops = &mqtt_quic_pipe_ops,
	.proto_ctx_ops  = &mqtt_quic_ctx_ops,
};

int
nng_mqtt_quic_client_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mqtt_quic_proto));
}

int
nng_mqttv5_quic_client_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mqttv5_quic_proto));
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
nng_mqttv5_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	return nng_mqtt_quic_set_connect_cb(sock, cb, arg);
}

int
nng_mqttv5_quic_set_disconnect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	return nng_mqtt_quic_set_disconnect_cb(sock, cb, arg);
}

int
nng_mqttv5_quic_set_msg_recv_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	return nng_mqtt_quic_set_msg_recv_cb(sock, cb, arg);
}

int
nng_mqttv5_quic_set_msg_send_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	return nng_mqtt_quic_set_msg_send_cb(sock, cb, arg);
}

