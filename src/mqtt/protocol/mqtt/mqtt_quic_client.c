#ifdef NNG_SUPP_QUIC
//
// Copyright 2024 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "nng/mqtt/mqtt_quic_client.h"
#define NNG_MQTT_QUIC_NO_LEGACY_MACROS
#include "nng/mqtt/mqtt_quic.h"
#include <limits.h>
#include "sqlite_handler.h"
#include "core/nng_impl.h"
#include "nng/supplemental/nanolib/log.h"
#include "core/sockimpl.h"
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
typedef struct mqtt_quic_tx mqtt_quic_tx;
typedef nni_mqtt_packet_type packet_type_t;



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
static void mqtt_quic_pipe_close(void *arg);

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
	int qos0_sent, qos_sent, qos_acked;
	uint8_t         mqtt_ver; // mqtt version.
	bool            multi_stream;
	bool            qos_first;
	bool            retry_qos_0;
	bool            connected;	 	// a stupid tmp status of network
	nni_mtx         mtx; 			// more fine grained mutual exclusion
	nni_atomic_bool closed;
	nni_atomic_int  next_packet_id; // next packet id to use, shared by multiple pipes
	nni_duration  retry;
	nni_duration  keepalive;       // mqtt keepalive
	nni_duration  timeleft;        // left time to send next ping
	reason_code   disconnect_code; // disconnect reason code
	nni_id_map    sent_unack;      // unacknowledged sent     messages
	mqtt_quic_ctx master;          // to which we delegate send/recv calls
	nni_list      recv_queue;      // ctx pending to receive
	nni_list      send_queue; // aio pending to send, not ctx!
	nni_list      send_contexts; // explicit contexts with a pending send
	nni_sock     *nsock;
	nni_lmq       legacy_acks;
    nni_aio      *legacy_ack_aio;
    nng_dialer    legacy_dialer;
    bool          legacy_mode;
    bool          legacy_pending;
	mqtt_pipe_t  *pipe;     // the major pipe (control stream)
	                   // main quic pipe, others needs a map to store the
	                   // relationship between MQTT topics and quic pipes

	nni_mqtt_sqlite_option *sqlite_opt;

	struct mqtt_client_cb cb; // user cb
#ifdef NNG_ENABLE_STATS
	nni_stat_item st_rcv_max;
	nni_stat_item mqtt_reconnect;
	nni_stat_item msg_resend;
	nni_stat_item msg_send_drop;
	nni_stat_item msg_recv_drop;
	nni_stat_item msg_bytes_cached;
	nni_stat_item msg_sqlite_cached;
#endif
};

// A mqtt_pipe_s is our per-stream protocol private structure.
// equal to self-defined pipe in other protocols
struct mqtt_pipe_s {
	nni_mtx         lk;
	nni_pipe       *qpipe;
	nni_list        sends;
	nni_cv          sends_cv;
	size_t          send_count;
	bool            recv_pending;
	bool            ready;			// mark if QUIC stream is ready
	mqtt_sock_t    *mqtt_sock;
	nni_id_map      recv_unack;    // unacknowledged received messages
	nni_aio         recv_aio;      // recv aio to the underlying transport
	nni_aio   		time_aio; // timer aio to resend unack msg
	nni_lmq         recv_messages; // recv messages queue
	uint16_t        rid;           // index of resending packet id
	uint8_t         reason_code;   // MQTTV5 reason code
	nni_atomic_bool closed;
	uint8_t         pingcnt;
	nni_msg        *pingmsg;
};

// User QoS AIOs wait for MQTT ACKs, not stream SEND_COMPLETE. Each byte-send
// has its own AIO and reference; pipe_stop joins these callbacks before freeing.
struct mqtt_quic_tx {
	mqtt_pipe_t *pipe;
	nni_aio aio;
	nni_aio *user;
	uint16_t packet_id;
	bool offline;
	nni_list_node node;
	nni_reap_node reap;
};
static void
mqtt_quic_tx_reap(void *arg)
{
	mqtt_quic_tx *tx = arg;
	nni_aio_fini(&tx->aio);
	NNI_FREE_STRUCT(tx);
}
static nni_reap_list mqtt_quic_tx_reapers = {
	.rl_offset = offsetof(mqtt_quic_tx, reap),
	.rl_func = mqtt_quic_tx_reap,
};

// Caller holds s->mtx. The transport owns a separate message reference.
static int
mqtt_quic_tx_submit(mqtt_pipe_t *p, nni_msg *msg, nni_aio *user, uint16_t pid)
{
	mqtt_quic_tx *tx;
	if (p->send_count >= NNG_MAX_SEND_LMQ) return NNG_EAGAIN;
	if ((tx = NNI_ALLOC_STRUCT(tx)) == NULL) return NNG_ENOMEM;
	tx->pipe = p;
	tx->user = user;
	tx->packet_id = pid;
	nni_aio_init(&tx->aio, mqtt_quic_send_cb, tx);
	nni_msg_clone(msg);
	nni_aio_set_msg(&tx->aio, msg);
	nni_list_append(&p->sends, tx);
	p->send_count++;
	nni_pipe_send(p->qpipe, &tx->aio);
	return 0;
}

#if defined(NNG_SUPP_SQLITE)
static void
mqtt_quic_sqlite_drain(mqtt_pipe_t *p)
{
	nni_mqtt_sqlite_option *sqlite = mqtt_quic_sock_get_sqlite_option(p->mqtt_sock);
	if (!sqlite_is_enabled(sqlite) || p->send_count >= NNG_MAX_SEND_LMQ) return;
	if (!nni_lmq_empty(&sqlite->offline_cache)) sqlite_flush_offline_cache(sqlite);
	nni_msg *msg = sqlite_get_cache_msg(sqlite);
	if (msg == NULL) return;
	if (mqtt_quic_tx_submit(p, msg, NULL, 0) == 0) {
		mqtt_quic_tx *tx = nni_list_last(&p->sends);
		tx->offline = true;
		nni_msg_free(msg);
	} else {
		// Database insertion consumes the message, including on failure.
		(void) nni_mqtt_qos_db_set_client_offline_msg(
		    sqlite->db, msg, sqlite->db_name, sqlite->mqtt_version);
	}
}
#endif

// All helpers below require s->mtx. Explicit contexts permit one send at
// a time; socket sends remain independently cancellable and concurrent.
static void
mqtt_quic_send_detach(mqtt_sock_t *s, nni_aio *aio)
{
    mqtt_quic_ctx *ctx;
    NNI_LIST_FOREACH (&s->send_contexts, ctx) {
        if (ctx->saio == aio) {
            ctx->saio = NULL;
            nni_list_remove(&s->send_contexts, ctx);
            break;
        }
    }
}
static void
mqtt_quic_send_finish(mqtt_sock_t *s, nni_aio *aio, int rv)
{
    mqtt_quic_send_detach(s, aio);
    nni_aio_finish(aio, rv, 0);
}

// Cache/transport references are separate from the caller's reference.
static void
mqtt_quic_clear_unack(mqtt_sock_t *s)
{
    uint16_t id = 0;
    nni_msg *msg;
    while ((msg = nni_id_get_min(&s->sent_unack, &id)) != NULL) {
        nni_id_remove(&s->sent_unack, id);
        nni_aio *aio = nni_mqtt_msg_get_aio(msg);
        if (aio) {
            nni_mqtt_msg_set_aio(msg, NULL);
            nni_aio_set_prov_data(aio, NULL);
            mqtt_quic_send_finish(s, aio, NNG_ECLOSED);
        }
        nni_msg_free(msg);
    }
}
static void
mqtt_quic_cancel_send_locked(mqtt_sock_t *s, nni_aio *aio, int rv)
{
	nni_msg *msg = nni_aio_get_prov_data(aio);
	if (nni_list_active(&s->send_queue, aio)) {
		nni_list_remove(&s->send_queue, aio);
		mqtt_quic_send_finish(s, aio, rv);
		return;
	}
	if (msg && nni_mqtt_msg_get_aio(msg) == aio) {
		uint16_t pid = nni_mqtt_msg_get_packet_id(msg);
		nni_id_remove(&s->sent_unack, pid);
		nni_mqtt_msg_set_aio(msg, NULL);
		nni_aio_set_prov_data(aio, NULL);
		if (s->pipe) {
			mqtt_quic_tx *tx;
			NNI_LIST_FOREACH (&s->pipe->sends, tx) {
				if (tx->packet_id == pid) nni_aio_abort(&tx->aio, rv);
			}
		}
		nni_msg_free(msg);
		mqtt_quic_send_finish(s, aio, rv);
		return;
	}
	if (s->pipe) {
		mqtt_quic_tx *tx;
		NNI_LIST_FOREACH (&s->pipe->sends, tx) {
			if (tx->user != aio) continue;
			tx->user = NULL;
			nni_aio_abort(&tx->aio, rv);
			mqtt_quic_send_finish(s, aio, rv);
			break;
		}
	}
}
static void
mqtt_quic_cancel_send(nni_aio *aio, void *arg, int rv)
{
    mqtt_sock_t *s = arg;
    nni_mtx_lock(&s->mtx);
    mqtt_quic_cancel_send_locked(s, aio, rv);
    nni_mtx_unlock(&s->mtx);
}

//need to free msg
static inline int
mqtt_quic_recv_msgq_putq(mqtt_pipe_t *p, nni_msg *msg)
{
	if (0 != nni_lmq_put(&p->recv_messages, msg)) {
		size_t max_que_len = NNG_MAX_RECV_LMQ;

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

// Caller holds s->mtx throughout; never unlock the caller's mutex here.
static int
mqtt_quic_send_msg(nni_aio *aio, mqtt_sock_t *s)
{
	mqtt_pipe_t *p = s->pipe;
	nni_msg *msg = nni_aio_get_msg(aio);
	uint16_t pid = 0;
	int rv;
	if (p == NULL || nni_atomic_get_bool(&p->closed)) {
		mqtt_quic_send_finish(s, aio, NNG_ECLOSED);
		return NNG_ECLOSED;
	}
	if (msg == NULL) {
		mqtt_quic_send_finish(s, aio, NNG_EINVAL);
		return NNG_EINVAL;
	}
	if (nng_mqtt_quic_msg_get_stream(msg) != 0) {
		bool multi = false;
		size_t size = sizeof(multi);
		if (nni_pipe_getopt(p->qpipe, NNG_OPT_QUIC_ENABLE_MULTISTREAM,
		    &multi, &size, NNI_TYPE_BOOL) != 0 || !multi) {
			mqtt_quic_send_finish(s, aio, NNG_ENOTSUP);
			return NNG_ENOTSUP;
		}
	}
	packet_type_t type = nni_mqtt_msg_get_packet_type(msg);
	switch (type) {
	case NNG_MQTT_CONNECT: case NNG_MQTT_DISCONNECT:
	case NNG_MQTT_PUBACK: case NNG_MQTT_PUBREC: case NNG_MQTT_PUBREL:
	case NNG_MQTT_PUBCOMP: case NNG_MQTT_PINGREQ:
	case NNG_MQTT_PUBLISH: case NNG_MQTT_SUBSCRIBE: case NNG_MQTT_UNSUBSCRIBE:
		break;
	default:
		mqtt_quic_send_finish(s, aio, NNG_EPROTO);
		return NNG_EPROTO;
	}
	bool ack = type == NNG_MQTT_SUBSCRIBE || type == NNG_MQTT_UNSUBSCRIBE ||
	    (type == NNG_MQTT_PUBLISH && nni_mqtt_msg_get_publish_qos(msg) != 0);
	((nni_mqtt_proto_data *) nni_msg_get_proto_data(msg))->quic_priority = s->qos_first && ack;
	nni_msg_set_timestamp(msg, nni_clock());
	if (ack) {
		pid = nni_mqtt_msg_get_packet_id(msg);
		if (nni_id_get(&s->sent_unack, pid) != NULL) {
			mqtt_quic_send_finish(s, aio, NNG_EBUSY);
			return NNG_EBUSY;
		}
		if ((rv = nni_id_set(&s->sent_unack, pid, msg)) != 0) {
			mqtt_quic_send_finish(s, aio, rv);
			return rv;
		}
		nni_msg_clone(msg);
		nni_mqtt_msg_set_aio(msg, aio);
		nni_aio_set_prov_data(aio, msg);
	}
	if ((rv = mqtt_quic_tx_submit(p, msg, ack ? NULL : aio, pid)) != 0) {
		if (ack) mqtt_quic_cancel_send_locked(s, aio, rv);
		else mqtt_quic_send_finish(s, aio, rv);
		return rv;
	}
	if (ack) s->qos_sent++;
	else s->qos0_sent++;
	return 0;
}

static void
mqtt_quic_send_cb(void *arg)
{
	mqtt_quic_tx *tx = arg;
	mqtt_pipe_t *p = tx->pipe;
	mqtt_sock_t *s = p->mqtt_sock;
	int rv = nni_aio_result(&tx->aio);
	bool fatal = rv != 0 && rv != NNG_ECANCELED && rv != NNG_ETIMEDOUT &&
	    rv != NNG_EMSGSIZE && rv != NNG_EPROTO && rv != NNG_EAGAIN;
	nni_mtx_lock(&s->mtx);
	p->send_count--;
#if defined(NNG_SUPP_SQLITE)
	if (rv != 0 && tx->offline) {
		nni_mqtt_sqlite_option *sqlite = mqtt_quic_sock_get_sqlite_option(s);
		nni_msg *msg = nni_aio_get_msg(&tx->aio);
		if (msg != NULL && sqlite_is_enabled(sqlite)) {
			(void) nni_mqtt_qos_db_set_client_offline_msg(
			    sqlite->db, msg, sqlite->db_name, sqlite->mqtt_version);
			nni_aio_set_msg(&tx->aio, NULL);
		}
	}
#endif
	nni_msg_free(nni_aio_get_msg(&tx->aio));
	nni_aio_set_msg(&tx->aio, NULL);
	if (tx->user != NULL) {
		if (rv == 0) {
			nni_msg_free(nni_aio_get_msg(tx->user));
			nni_aio_set_msg(tx->user, NULL);
		}
		mqtt_quic_send_finish(s, tx->user, rv);
		tx->user = NULL;
	} else if (s->pipe == p && rv != 0 && rv != NNG_ECANCELED && tx->packet_id != 0) {
		nni_msg *cached = nni_id_get(&s->sent_unack, tx->packet_id);
		if (cached != NULL && nni_mqtt_msg_get_aio(cached) != NULL)
			mqtt_quic_cancel_send_locked(s, nni_mqtt_msg_get_aio(cached), rv);
	}
	int (*cb)(void *, void *) = rv == 0 ? s->cb.msg_send_cb : NULL;
	void *cbarg = s->cb.sendarg;
	if (s->pipe == p && !nni_atomic_get_bool(&p->closed) && !fatal) {
		if (rv == 0) s->timeleft = s->keepalive;
		nni_aio *aio;
		while (p->send_count < NNG_MAX_SEND_LMQ &&
		    (aio = nni_list_first(&s->send_queue)) != NULL) {
			nni_list_remove(&s->send_queue, aio);
#ifdef NNG_ENABLE_STATS
			nni_stat_dec(&s->msg_bytes_cached, nni_msg_len(nni_aio_get_msg(aio)));
#endif
			mqtt_quic_send_msg(aio, s);
		}
#if defined(NNG_SUPP_SQLITE)
		if (rv == 0) mqtt_quic_sqlite_drain(p);
#endif
	}
	nni_mtx_unlock(&s->mtx);
	if (fatal) nni_pipe_close(p->qpipe);
	if (cb) cb(NULL, cbarg);
	nni_mtx_lock(&s->mtx);
	nni_list_remove(&p->sends, tx);
	nni_cv_wake(&p->sends_cv);
	nni_mtx_unlock(&s->mtx);
	nni_reap(&mqtt_quic_tx_reapers, tx);
}

static void
mqtt_quic_legacy_ack(mqtt_sock_t *s, nni_msg *msg)
{
    nni_aio *aio = s->legacy_ack_aio;
    if (aio == NULL) return;
    nni_msg_clone(msg);
    if (!nni_aio_busy(aio)) {
        if (nni_aio_begin(aio) == 0) {
            nni_aio_set_msg(aio, msg);
            nni_aio_finish(aio, 0, nni_msg_len(msg));
            return;
        }
    } else if (nni_lmq_put(&s->legacy_acks, msg) == 0) {
        return;
    }
    nni_msg_free(msg);
}

static void
mqtt_quic_recv_start(mqtt_pipe_t *p)
{
	if (!p->recv_pending && !nni_atomic_get_bool(&p->closed) &&
	    !nni_lmq_full(&p->recv_messages)) {
		p->recv_pending = true;
		nni_pipe_recv(p->qpipe, &p->recv_aio);
	}
}

static void
mqtt_quic_deliver(mqtt_sock_t *s, mqtt_pipe_t *p, nni_msg *msg, bool connack)
{
	int (*cb)(void *, void *) = connack ? s->cb.connect_cb : s->cb.msg_recv_cb;
	void *cbarg = connack ? s->cb.connarg : s->cb.recvarg;
	mqtt_quic_ctx *ctx = nni_list_first(&s->recv_queue);
	if (cb) nni_msg_clone(msg);
	if (ctx) {
		nni_list_remove(&s->recv_queue, ctx);
		nni_aio *aio = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(aio, msg);
		nni_aio_finish(aio, 0, nni_msg_len(msg));
	} else if (cb) {
		nni_msg_free(msg);
	} else if (mqtt_quic_recv_msgq_putq(p, msg) != 0) {
		nni_msg_free(msg);
	}
	mqtt_quic_recv_start(p);
	nni_mtx_unlock(&s->mtx);
	if (cb) {
		cb(msg, cbarg);
		nni_msg_free(msg);
	}
}

static void
mqtt_quic_recv_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_msg *msg, *cached;
	nni_aio *aio;
	int rv = nni_aio_result(&p->recv_aio);
	if (rv) { nni_pipe_close(p->qpipe); return; }
	nni_mtx_lock(&s->mtx);
	msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (nni_atomic_get_bool(&p->closed) || nni_atomic_get_bool(&s->closed)) {
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		return;
	}
	if (!msg) { nni_mtx_unlock(&s->mtx); nni_pipe_close(p->qpipe); return; }
	if ((nni_msg_get_proto_data(msg) == NULL && (rv = nni_mqtt_msg_proto_data_alloc(msg)) != 0) ||
	    (rv = (s->mqtt_ver == 5 ? nni_mqttv5_msg_decode(msg) : nni_mqtt_msg_decode(msg))) != 0) {
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;
	}
	p->recv_pending = false;
	packet_type_t type = nni_mqtt_msg_get_packet_type(msg);
	uint64_t id;
	switch (type) {
	case NNG_MQTT_CONNACK:
        mqtt_quic_legacy_ack(s, msg);
		mqtt_quic_deliver(s, p, msg, true);
		return;
	case NNG_MQTT_PUBLISH:
		if (nni_mqtt_msg_get_publish_qos(msg) < 2) {
			mqtt_quic_deliver(s, p, msg, false);
			return;
		}
		id = nni_mqtt_msg_get_publish_packet_id(msg) | ((uint64_t)nng_mqtt_quic_msg_get_stream(msg) << 16);
		if ((cached = nni_id_get(&p->recv_unack, id)) != NULL) {
			nni_id_remove(&p->recv_unack, id);
			nni_msg_free(cached);
		}
		if (nni_id_set(&p->recv_unack, id, msg) != 0) nni_msg_free(msg);
		break;
	case NNG_MQTT_PUBREL:
		id = nni_mqtt_msg_get_pubrel_packet_id(msg) | ((uint64_t)nng_mqtt_quic_msg_get_stream(msg) << 16);
		nni_msg_free(msg);
		if ((cached = nni_id_get(&p->recv_unack, id)) != NULL) {
			nni_id_remove(&p->recv_unack, id);
			mqtt_quic_deliver(s, p, cached, false);
			return;
		}
		break;
    case NNG_MQTT_PUBREC:
        if (s->mqtt_ver != 5 || nni_msg_len(msg) < 3 ||
            ((uint8_t *)nni_msg_body(msg))[2] < 0x80) {
            nni_msg_free(msg);
            break;
        }
        // Negative PUBREC is the terminal acknowledgment. Return it through
        // the same AIO contract as PUBACK/PUBCOMP so callers can inspect it.
        // fall through
	case NNG_MQTT_PUBACK:
	case NNG_MQTT_PUBCOMP:
	case NNG_MQTT_SUBACK:
	case NNG_MQTT_UNSUBACK:
        if (type == NNG_MQTT_PUBACK || type == NNG_MQTT_PUBCOMP || type == NNG_MQTT_PUBREC) mqtt_quic_legacy_ack(s, msg);
		id = nni_mqtt_msg_get_packet_id(msg);
		if ((cached = nni_id_get(&s->sent_unack, id)) != NULL) {
			nni_id_remove(&s->sent_unack, id);
			aio = nni_mqtt_msg_get_aio(cached);
			nni_mqtt_msg_set_aio(cached, NULL);
			if (aio) {
				nni_aio_set_prov_data(aio, NULL);
				nni_msg_clone(msg);
				nni_msg_free(nni_aio_get_msg(aio));
				nni_aio_set_msg(aio, msg);
				mqtt_quic_send_finish(s, aio, 0);
			}
			nni_msg_free(cached);
		}
        if (s->legacy_mode && (type == NNG_MQTT_SUBACK || type == NNG_MQTT_UNSUBACK)) {
            mqtt_quic_deliver(s, p, msg, false);
            return;
        }
		nni_msg_free(msg);
		break;
	case NNG_MQTT_PINGRESP:
		p->pingcnt = 0;
		s->timeleft = s->keepalive;
		nni_msg_free(msg);
		break;
	case NNG_MQTT_DISCONNECT:
		s->disconnect_code = nni_msg_len(msg) ? *(uint8_t *)nni_msg_body(msg) : 0;
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;
	default:
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;
	}
	mqtt_quic_recv_start(p);
	nni_mtx_unlock(&s->mtx);
}

// Keepalive and retransmissions use the same pipe contract as application sends.
static void
mqtt_timer_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	if (nni_aio_result(&p->time_aio) != 0) return;
	nni_mtx_lock(&s->mtx);
	if (s->pipe != p || nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
		return;
	}
	if (s->keepalive > 0 && p->pingcnt > 2) {
		s->disconnect_code = KEEP_ALIVE_TIMEOUT;
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->qpipe);
		return;
	}
	if (s->keepalive > 0) {
		s->timeleft -= s->retry;
		if (s->timeleft <= 0 && mqtt_quic_tx_submit(p, p->pingmsg, NULL, 0) == 0) {
			s->timeleft = s->keepalive;
			p->pingcnt++;
		}
	}
	uint32_t cursor = 0;
	uint64_t id;
	void *value;
	nni_time now = nni_clock();
	while (nni_id_visit(&s->sent_unack, &id, &value, &cursor)) {
		nni_msg *msg = value;
		if (now - nni_msg_get_timestamp(msg) < 5000) continue;
		bool pending = false;
		mqtt_quic_tx *tx;
		NNI_LIST_FOREACH (&p->sends, tx) {
			if (tx->packet_id == id) { pending = true; break; }
		}
		if (pending) continue;
		nni_msg *copy;
		if (nni_msg_dup(&copy, msg) != 0) break;
		if (nni_mqtt_msg_get_packet_type(copy) == NNG_MQTT_PUBLISH)
			*(uint8_t *) nni_msg_header(copy) |= 0x08;
		int rv = mqtt_quic_tx_submit(p, copy, NULL, (uint16_t) id);
		nni_msg_free(copy);
		if (rv != 0) break;
		nni_msg_set_timestamp(msg, now);
		s->qos_sent++;
	}
#if defined(NNG_SUPP_SQLITE)
	if (p->send_count == 0) mqtt_quic_sqlite_drain(p);
#endif
	nni_sleep_aio(s->retry, &p->time_aio);
	nni_mtx_unlock(&s->mtx);
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


	s->retry_qos_0 = true;
	// this is "semi random" start for request IDs.
	s->retry      = NNI_SECOND * MQTT_QUIC_RETRTY;
	s->keepalive  = NNI_SECOND * 10; // default mqtt keepalive
	s->timeleft   = NNI_SECOND * 10;

	// For caching aio
	nni_aio_list_init(&s->send_queue);
	NNI_LIST_INIT(&s->send_contexts, mqtt_quic_ctx, sqnode);
    nni_lmq_init(&s->legacy_acks, NNG_MAX_RECV_LMQ);
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
	static const nni_stat_info msg_sqlite_cached = {
		.si_name   = "mqtt_msg_sqlite_cached",
		.si_desc   = "count all cached msg in sqlite",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_BYTES,
		.si_atomic = true,
	};
	nni_stat_init(&s->msg_sqlite_cached, &msg_sqlite_cached);
	nni_sock_add_stat(s->nsock, &s->mqtt_reconnect);
	nni_sock_add_stat(s->nsock, &s->msg_resend);
	nni_sock_add_stat(s->nsock, &s->msg_send_drop);
	nni_sock_add_stat(s->nsock, &s->msg_recv_drop);
	nni_sock_add_stat(s->nsock, &s->msg_bytes_cached);
	nni_sock_add_stat(s->nsock, &s->msg_sqlite_cached);
#endif
}

static void
mqtt_quic_sock_fini(void *arg)
{
	mqtt_sock_t *s = arg;


	log_debug("mqtt_quic_sock_fini %p", s);
    if (s->legacy_ack_aio) {
        nni_aio_stop(s->legacy_ack_aio);
        nni_aio_free(s->legacy_ack_aio);
    }
    nni_lmq_fini(&s->legacy_acks);
	nni_id_map_fini(&s->sent_unack);
	mqtt_quic_ctx_fini(&s->master);
	nni_mtx_fini(&s->mtx);
}

static void
mqtt_quic_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static void
mqtt_quic_sock_close(void *arg)
{
	uint16_t count = 0;
	nni_aio *aio;
	mqtt_sock_t *s = arg;
	mqtt_quic_ctx *ctx;

	nni_atomic_set_bool(&s->closed, true);
	nni_mtx_lock(&s->mtx);

	// Complete application operations before destroying the socket.
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
		count == 0 ? nni_aio_finish(aio, NNG_ECONNSHUT, 0)
		           : nni_aio_finish_error(aio, NNG_ECLOSED);
		// there should be no msg waiting
		count++;
	}
	while ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		mqtt_quic_send_finish(s, aio, NNG_ECLOSED);
	}
	mqtt_quic_clear_unack(s);
    if (s->legacy_ack_aio) nni_aio_close(s->legacy_ack_aio);
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
	NNI_ARG_UNUSED(arg); NNI_ARG_UNUSED(buf); NNI_ARG_UNUSED(sz); NNI_ARG_UNUSED(t);
	return NNG_ENOTSUP;
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



static int
mqtt_quic_sock_get_pipeid(void *arg, void *buf, size_t *szp, nni_type t)
{
    mqtt_sock_t *s = arg;
    nni_mtx_lock(&s->mtx);
    uint32_t pid = s->pipe ? nni_pipe_id(s->pipe->qpipe) : 0xffffffff;
    int rv = nni_copyout_u64(pid, buf, szp, t);
    if (!s->pipe && rv == 0) rv = NNG_ECLOSED;
    nni_mtx_unlock(&s->mtx);
    return rv;
}
static int
mqtt_quic_sock_get_connect_status(void *arg, void *buf, size_t *szp, nni_type t)
{
    mqtt_sock_t *s = arg;
    nni_mtx_lock(&s->mtx);
    int rv = nni_copyout_bool(s->connected, buf, szp, t);
    nni_mtx_unlock(&s->mtx);
    return rv;
}

/******************************************************************************
 *                          Stream(PIPE) Implementation                       *
 ******************************************************************************/
// allocate quic stream with pipe
static int
mqtt_quic_pipe_init(void *arg, nni_pipe *pipe, void *sock)
{
	mqtt_pipe_t *p = arg;

	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, true);
	p->qpipe = pipe;
	p->mqtt_sock   = sock;
	NNI_LIST_INIT(&p->sends, mqtt_quic_tx, node);
	nni_cv_init(&p->sends_cv, &p->mqtt_sock->mtx);
	p->rid = 1;
	p->pingcnt = 0;
	p->pingmsg = NULL;

	nni_aio_init(&p->recv_aio, mqtt_quic_recv_cb, p);
	nni_aio_init(&p->time_aio, mqtt_timer_cb, p);
	// Packet IDs are 16 bits
	// We start at a random point, to minimize likelihood of
	// accidental collision across restarts.
	nni_id_map_init(&p->recv_unack, 0x0000u, 0x4ffffu, true);

		nni_lmq_init(&p->recv_messages, NNG_MAX_RECV_LMQ);

	nni_mtx_init(&p->lk);
	int rv = nni_msg_alloc(&p->pingmsg, 0);
	if (rv != 0) return rv;
	uint8_t ping[] = { 0xc0, 0 };
	if ((rv = nni_msg_header_append(p->pingmsg, ping, sizeof(ping))) != 0) return rv;
	p->ready = false;
	return (0);
}

static void
mqtt_quic_pipe_fini(void *arg)
{
	mqtt_pipe_t *p = arg;
	nni_msg * msg;

	log_debug("mqtt_quic_pipe_fini! pipe finit!");
	if ((msg = nni_aio_get_msg(&p->recv_aio)) != NULL) {
		nni_aio_set_msg(&p->recv_aio, NULL);
		nni_msg_free(msg);
	}
	if (p->pingmsg)
		nni_msg_free(p->pingmsg);

	nni_cv_fini(&p->sends_cv);
	nni_aio_fini(&p->recv_aio);
	nni_aio_fini(&p->time_aio);

	nni_id_map_fini(&p->recv_unack);
	nni_lmq_fini(&p->recv_messages);
	nni_mtx_fini(&p->lk);


}

static int
mqtt_quic_pipe_start(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_pipe    *npipe = p->qpipe;
	nni_aio     *aio;

	// p_dialer is not available when pipe init and sock init. Until pipe start.
	nni_mtx_lock(&s->mtx);
    if (nni_atomic_get_bool(&s->closed) || s->pipe != NULL) {
        nni_mtx_unlock(&s->mtx);
        return NNG_EBUSY;
    }
	nni_atomic_set_bool(&p->closed, false);
	s->pipe            = p;
	size_t keepalive_size = sizeof(s->keepalive);
	(void)nni_pipe_getopt(p->qpipe, NNG_OPT_MQTT_KEEPALIVE_INTERVAL, &s->keepalive, &keepalive_size, NNI_TYPE_DURATION);
	s->timeleft = s->keepalive;
	s->disconnect_code = SUCCESS;
	p->ready           = true;
	s->connected       = true;

	// Drain all queued callers, including after a validation failure. There
	// may be no byte-send callback to wake the next caller after such a failure.
	while (p->send_count < NNG_MAX_SEND_LMQ &&
	    (aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
#ifdef NNG_ENABLE_STATS
		nni_stat_dec(&s->msg_bytes_cached, nni_msg_len(nni_aio_get_msg(aio)));
#endif
		mqtt_quic_send_msg(aio, s);
	}
	p->recv_pending = true;
	nni_mtx_unlock(&s->mtx);
	// initiate the global resend timer
	nni_sleep_aio(s->retry, &p->time_aio);
	nni_pipe_recv(npipe, &p->recv_aio);
#ifdef NNG_ENABLE_STATS
	nni_stat_inc(&s->mqtt_reconnect, 1);
#endif
	return 0;
}

static void
mqtt_quic_pipe_stop(void *arg)
{
	mqtt_pipe_t *p = arg;

	log_info("Stopping MQTT over QUIC Stream");
	nni_mtx_lock(&p->mqtt_sock->mtx);
	while (!nni_list_empty(&p->sends)) nni_cv_wait(&p->sends_cv);
	nni_mtx_unlock(&p->mqtt_sock->mtx);
	nni_aio_stop(&p->recv_aio);
	nni_aio_stop(&p->time_aio);
}

static void
mqtt_quic_pipe_close(void *arg)
{
	log_trace(" ##### mqtt_quic_pipe_close ##### ");
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	nni_mtx_lock(&s->mtx);
	if (nni_atomic_swap_bool(&p->closed, true)) {
		nni_mtx_unlock(&s->mtx);
		return;
	}
	bool active = s->pipe == p;
	int (*disconnect_cb)(void *, void *) = active ? s->cb.disconnect_cb : NULL;
	void *disconnect_arg = s->cb.discarg;
	p->ready     = false;
	if (active) {
		s->pipe = NULL;
		s->connected = false;
	}

	mqtt_quic_tx *tx;
	NNI_LIST_FOREACH (&p->sends, tx) nni_aio_close(&tx->aio);
	nni_aio_close(&p->recv_aio);
	nni_aio_close(&p->time_aio);
	nni_lmq_flush(&p->recv_messages);
	//TODO add timeout cancel for msgs in sent_unack
	nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);
	if (active) mqtt_quic_clear_unack(s);
	nni_mtx_unlock(&s->mtx);
	if (disconnect_cb) disconnect_cb(NULL, disconnect_arg);

	return;
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
    if ((aio = ctx->saio) != NULL) {
        mqtt_quic_cancel_send_locked(s, aio, NNG_ECLOSED);
    }
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
	if (nni_atomic_get_bool(&s->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

    if (ctx != &s->master && ctx->saio != NULL) {
        nni_mtx_unlock(&s->mtx);
        nni_aio_finish_error(aio, NNG_ESTATE);
        return;
    }
	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
    if (nni_msg_get_proto_data(msg) == NULL) {
        nni_mtx_unlock(&s->mtx);
        nni_aio_finish_error(aio, NNG_EINVAL);
        return;
    }
    if (s->legacy_mode && nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT) {
        if (!s->legacy_pending || nni_mqtt_msg_get_connect_proto_version(msg) != s->mqtt_ver) {
            nni_mtx_unlock(&s->mtx);
            nni_aio_finish_error(aio, NNG_ESTATE);
            return;
        }
        s->legacy_pending = false;
        nng_dialer d = s->legacy_dialer;
        nni_mtx_unlock(&s->mtx);
        nni_msg *copy = NULL;
        rv = nni_msg_dup(&copy, msg);
        if (rv == 0) {
            rv = nng_dialer_set_ptr(d, NNG_OPT_MQTT_CONNMSG, copy);
            if (rv) nni_msg_free(copy);
            else rv = nng_dialer_start(d, NNG_FLAG_NONBLOCK);
        }
        if (rv == 0) {
            nni_msg_free(msg);
            nni_aio_set_msg(aio, NULL);
        } else {
            nni_mtx_lock(&s->mtx);
            s->legacy_pending = true;
            nni_mtx_unlock(&s->mtx);
        }
        nni_aio_finish(aio, rv, 0);
        return;
    }
    if (nng_mqtt_quic_msg_get_stream(msg) != 0 && s->pipe != NULL) {
        bool multi = false;
        size_t size = sizeof(multi);
        if (nni_pipe_getopt(s->pipe->qpipe, NNG_OPT_QUIC_ENABLE_MULTISTREAM,
            &multi, &size, NNI_TYPE_BOOL) != 0 || !multi) {
            nni_mtx_unlock(&s->mtx);
            nni_aio_finish_error(aio, NNG_ENOTSUP);
            return;
        }
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
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	nni_aio_set_prov_data(aio, NULL);
	if ((rv = nni_aio_schedule(aio, mqtt_quic_cancel_send, s)) != 0) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
    if (ctx != &s->master) {
        ctx->saio = aio;
        nni_list_append(&s->send_contexts, ctx);
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
                if ((rv = nni_lmq_put(&sqlite->offline_cache, msg)) != 0) {
                    mqtt_quic_send_finish(s, aio, rv);
                    nni_mtx_unlock(&s->mtx);
                    return;
                }
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_bytes_cached, nng_msg_len(msg));
#endif
				if (nni_lmq_full(&sqlite->offline_cache)) {
					sqlite_flush_offline_cache(sqlite);
				}
				nni_aio_set_msg(aio, NULL);
				mqtt_quic_send_finish(s, aio, 0);
				nni_mtx_unlock(&s->mtx);
				return;
			}
		}
#endif
		if (!nni_atomic_get_bool(&s->closed)) {
			if (!nni_list_active(&s->send_queue, aio)) {
				// cache aio
				nni_list_append(&s->send_queue, aio);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_bytes_cached, nng_msg_len(msg));
#endif
				nni_mtx_unlock(&s->mtx);
				log_info("client sending msg while disconnected! aio cached");
				return;
			}
		}
        mqtt_quic_send_finish(s, aio, NNG_ECLOSED);
        nni_mtx_unlock(&s->mtx);
        return;
	}
	mqtt_quic_send_msg(aio, s);
	nni_mtx_unlock(&s->mtx);
	return;
}

static void
mqtt_quic_cancel_recv(nni_aio *aio, void *arg, int rv)
{
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t *s = ctx->mqtt_sock;
	nni_mtx_lock(&s->mtx);
	if (ctx->raio == aio) {
		ctx->raio = NULL;
		nni_list_remove(&s->recv_queue, ctx);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&s->mtx);
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
	if (nni_atomic_get_bool(&s->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
    if (aio == s->legacy_ack_aio) {
        if (nni_lmq_get(&s->legacy_acks, &msg) == 0) {
            nni_aio_set_msg(aio, msg);
            nni_aio_finish(aio, 0, nni_msg_len(msg));
        } else nni_aio_finish_error(aio, NNG_ECANCELED);
        nni_mtx_unlock(&s->mtx);
        return;
    }
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
		mqtt_quic_recv_start(p);
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
	if ((rv = nni_aio_schedule(aio, mqtt_quic_cancel_recv, ctx)) != 0) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, rv);
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
	rv = nni_copyout_int(s->disconnect_code, v, sz, t);
	nni_mtx_unlock(&s->mtx);
	return (rv);
}

static int
mqtt_quic_sock_set_retry_qos_0(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	bool tmp;
	int rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&s->mtx);
		s->retry_qos_0 = tmp;
		nni_mtx_unlock(&s->mtx);
	}
	return (rv);
}

static int
mqtt_quic_sock_set_retry_interval(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	nni_duration    tmp;
	int rv;

	if ((rv = nni_copyin_ms(&tmp, v, sz, t)) == 0) {
		if (tmp <= 0) return NNG_EINVAL;
		nni_mtx_lock(&s->mtx);
		s->retry = tmp > 600000 ? 600000 : tmp;
		nni_mtx_unlock(&s->mtx);
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
	    .o_name = NNG_OPT_QUIC_ENABLE_MULTISTREAM,
	    .o_set  = mqtt_quic_sock_set_multi_stream,
	},
	{
	    .o_name = NNG_OPT_MQTT_DISCONNECT_REASON,
	    .o_get  = mqtt_quic_sock_get_disconnect_code,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_INTERVAL,
	    .o_set  = mqtt_quic_sock_set_retry_interval,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_QOS_0,
	    .o_set  = mqtt_quic_sock_set_retry_qos_0,
	},
	{
	    .o_name = NNG_OPT_MQTT_SQLITE,
	    .o_set  = mqtt_quic_sock_set_sqlite_option,
	},
	{
	    .o_name = NNG_OPT_MQTT_CLIENT_PIPEID,
	    .o_get  = mqtt_quic_sock_get_pipeid,
	},
	{
	    .o_name = NNG_OPT_MQTT_CLIENT_CONNECT_BOOL,
	    .o_get  = mqtt_quic_sock_get_connect_status,
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


static int
mqtt_quic_open_legacy(nng_socket *sock, const char *url, const conf_quic *conf, bool v5)
{
    if (!sock || !url) return NNG_EINVAL;
    if (conf && (conf->qkeepalive > INT_MAX || conf->qdiscon_timeout > INT_MAX)) return NNG_EINVAL;
    // The stable MsQuic API exposes CUBIC, not the older fork's BBR extension.
    if (conf && conf->qcongestion_control != 0) return NNG_ENOTSUP;
    int rv = v5 ? nng_mqttv5_quic_client_open(sock) : nng_mqtt_quic_client_open(sock);
    if (rv) return rv;
    nng_dialer d;
    if ((rv = nng_dialer_create(&d, *sock, url)) != 0) goto error;
    if ((rv = nng_dialer_set_bool(d, NNG_OPT_QUIC_ENABLE_MULTISTREAM, conf && conf->multi_stream)) != 0) goto error;
    if (conf) {
        if (conf->qkeepalive && (rv = nng_dialer_set_int(d, NNG_OPT_QUIC_KEEPALIVE, (int)conf->qkeepalive))) goto error;
        if (conf->qconnect_timeout && (rv = nng_dialer_set_uint64(d, NNG_OPT_QUIC_CONNECT_TIMEOUT, conf->qconnect_timeout))) goto error;
        if (conf->qdiscon_timeout && (rv = nng_dialer_set_int(d, NNG_OPT_QUIC_DISCONNECT_TIMEOUT, (int)conf->qdiscon_timeout))) goto error;
        if (conf->qidle_timeout && (rv = nng_dialer_set_uint64(d, NNG_OPT_QUIC_IDLE_TIMEOUT, conf->qidle_timeout))) goto error;
        if (conf->tls.enable) {
            if ((rv = nng_dialer_set_bool(d, NNG_OPT_QUIC_TLS_VERIFY_PEER, conf->tls.verify_peer))) goto error;
            const char *values[] = {conf->tls.cafile, conf->tls.certfile, conf->tls.keyfile, conf->tls.key_password};
            const char *options[] = {NNG_OPT_QUIC_TLS_CA_PATH, NNG_OPT_QUIC_TLS_CACERT_PATH, NNG_OPT_QUIC_TLS_KEY_PATH, NNG_OPT_QUIC_TLS_KEY_PASSWORD};
            for (unsigned i = 0; i < 4; ++i)
                if (values[i] && values[i][0] && (rv = nng_dialer_set_string(d, options[i], values[i]))) goto error;
        }
    }
    nni_sock *nsock;
    if ((rv = nni_sock_find(&nsock, sock->id)) != 0) goto error;
    mqtt_sock_t *s = nni_sock_proto_data(nsock);
    nni_mtx_lock(&s->mtx);
    s->legacy_mode = true;
    s->legacy_pending = true;
    s->legacy_dialer = d;
    s->qos_first = conf && conf->qos_first;
    nni_mtx_unlock(&s->mtx);
    nni_sock_rele(nsock);
    return 0;
error:
    nng_close(*sock);
    return rv;
}
int nng_mqtt_quic_client_open_url(nng_socket *s, const char *url) { return mqtt_quic_open_legacy(s, url, NULL, false); }
int nng_mqttv5_quic_client_open_url(nng_socket *s, const char *url) { return mqtt_quic_open_legacy(s, url, NULL, true); }
int nng_mqtt_quic_client_open_conf(nng_socket *s, const char *url, conf_quic *c) { return mqtt_quic_open_legacy(s, url, c, false); }
int nng_mqttv5_quic_client_open_conf(nng_socket *s, const char *url, conf_quic *c) { return mqtt_quic_open_legacy(s, url, c, true); }

int nng_mqtt_quic_ack_callback_set(nng_socket *sock, void (*cb)(void *), void *arg)
{
    if (!sock || !cb) return NNG_EINVAL;
    nni_sock *nsock;
    int rv = nni_sock_find(&nsock, sock->id);
    if (rv) return rv;
    mqtt_sock_t *s = nni_sock_proto_data(nsock);
    nni_aio *aio;
    aio = NNI_ALLOC_STRUCT(aio);
    rv = aio ? 0 : NNG_ENOMEM;
    if (rv == 0) {
        nni_aio_init(aio, cb, aio);
        nni_aio_set_prov_data(aio, arg);
        nni_mtx_lock(&s->mtx);
        if (s->legacy_ack_aio != NULL) rv = NNG_EBUSY;
        else s->legacy_ack_aio = aio;
        nni_mtx_unlock(&s->mtx);
        if (rv) nni_aio_free(aio);
    }
    nni_sock_rele(nsock);
    return rv;
}
void conf_quic_tls_free(conf_quic *c)
{
    if (!c) return;
    nng_strfree(c->tls.cafile); nng_strfree(c->tls.certfile);
    nng_strfree(c->tls.keyfile); nng_strfree(c->tls.key_password);
    nng_free(c, sizeof(*c));
}
void conf_quic_tls_create(conf_quic **out, char *ca, char *cert, char *key, char *password)
{
    if (!out) return;
    *out = NULL;
    conf_quic *c = nng_alloc(sizeof(*c));
    if (!c) return;
    memset(c, 0, sizeof(*c));
    char **dest[] = {&c->tls.cafile, &c->tls.certfile, &c->tls.keyfile, &c->tls.key_password};
    char *src[] = {ca, cert, key, password};
    for (unsigned i = 0; i < 4; ++i) {
        if (src[i] && (*dest[i] = nng_strdup(src[i])) == NULL) {
            conf_quic_tls_free(c); return;
        }
    }
    c->tls.enable = true; c->tls.verify_peer = true;
    c->qkeepalive = 30; c->qconnect_timeout = 60;
    c->qdiscon_timeout = 30; c->qidle_timeout = 30;
    *out = c;
}

int
nng_mqtt_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg)
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		nni_mtx_lock(&mqtt_sock->mtx);
		mqtt_sock->cb.connect_cb = cb;
		mqtt_sock->cb.connarg = arg;
		nni_mtx_unlock(&mqtt_sock->mtx);
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
		nni_mtx_lock(&mqtt_sock->mtx);
		mqtt_sock->cb.msg_send_cb = cb;
		mqtt_sock->cb.sendarg = arg;
		nni_mtx_unlock(&mqtt_sock->mtx);
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
		nni_mtx_lock(&mqtt_sock->mtx);
		mqtt_sock->cb.msg_recv_cb = cb;
		mqtt_sock->cb.recvarg = arg;
		nni_mtx_unlock(&mqtt_sock->mtx);
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
		nni_mtx_lock(&mqtt_sock->mtx);
		mqtt_sock->cb.disconnect_cb = cb;
		mqtt_sock->cb.discarg = arg;
		nni_mtx_unlock(&mqtt_sock->mtx);
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


#endif // NNG_SUPP_QUIC
