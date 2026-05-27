//
// Copyright 2024 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "sqlite_handler.h"
// #include "nng/protocol/mqtt/mqtt.h"
#include "nng/supplemental/sqlite/sqlite3.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/mqtt/mqtt_qos_db_api.h"
// #include "nng/protocol/mqtt/mqtt_parser.h"
#include "nng/mqtt/mqtt_client.h"
#include "nng/supplemental/nanolib/log.h"

// MQTT client implementation.
//
// 1. MQTT client sockets have a single implicit dialer, and cannot
//    support creation of additional dialers or listeners.
// 2. Send sends PUBLISH messages.
// 3. Receive is used to receive published data from the server.

#define NNG_MQTT_SELF 0
#define NNG_MQTT_SELF_NAME "mqtt-client"
#define NNG_MQTT_PEER 0
#define NNG_MQTT_PEER_NAME "mqtt-server"

typedef struct mqtt_sock_s mqtt_sock_t;
typedef struct mqtt_pipe_s mqtt_pipe_t;
typedef struct mqtt_ctx_s  mqtt_ctx_t;

static void mqtt_sock_init(void *arg, nni_sock *sock);
static void mqtt_sock_fini(void *arg);
static void mqtt_sock_open(void *arg);
static void mqtt_sock_send(void *arg, nni_aio *aio);
static void mqtt_sock_recv(void *arg, nni_aio *aio);
static void mqtt_send_cb(void *arg);
static void mqtt_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

static int  mqtt_pipe_init(void *arg, nni_pipe *pipe, void *s);
static void mqtt_pipe_fini(void *arg);
static int  mqtt_pipe_start(void *arg);
static void mqtt_pipe_stop(void *arg);
static int  mqtt_pipe_close(void *arg);

static void mqtt_ctx_init(void *arg, void *sock);
static void mqtt_ctx_fini(void *arg);
static void mqtt_ctx_send(void *arg, nni_aio *aio);
static void mqtt_ctx_recv(void *arg, nni_aio *aio);
static void mqtt_ctx_cancel_send(nni_aio *aio, void *arg, int rv);

typedef nni_mqtt_packet_type packet_type_t;

#if defined(NNG_SUPP_SQLITE)
static void *mqtt_sock_get_sqlite_option(mqtt_sock_t *s);
#endif

// A mqtt_ctx_s is our per-ctx protocol private state.
struct mqtt_ctx_s {
	mqtt_sock_t * mqtt_sock;
	nni_aio *     saio; // send aio
	nni_aio *     raio; // recv aio
	nni_list_node sqnode;
	nni_list_node rqnode;
};

// A mqtt_pipe_s is our per-pipe protocol private structure.
struct mqtt_pipe_s {
	nni_atomic_bool closed;			// indicates mqtt connection status
	nni_pipe *      pipe;
	mqtt_sock_t *   mqtt_sock;

	nni_id_map      recv_unack;    // recv messages unacknowledged
	nni_aio         send_aio;      // send aio to the underlying transport
	nni_aio         recv_aio;      // recv aio to the underlying transport
	nni_aio         time_aio;      // timer aio to resend unack msg
	nni_lmq         recv_messages; // recv messages queue
	nni_lmq         send_messages; // send messages queue
	uint16_t        rid;           // index of resending packet id
	bool            busy;
	uint8_t         pingcnt;
	nni_msg        *pingmsg;
};

// A mqtt_sock_s is our per-socket protocol private structure.
struct mqtt_sock_s {
	bool            retry_qos_0;	// define if flush QoS 0 msg to disk
	nni_mtx         mtx;    		// more fine grained mutual exclusion
	uint8_t         mqtt_ver;       // mqtt version.
	nni_atomic_bool closed;
	nni_atomic_int  next_packet_id; // next packet id to use
	nni_duration    retry;
	nni_duration    keepalive; // mqtt keepalive
	nni_duration    timeleft;  // left time to send next ping
	mqtt_ctx_t      master; // to which we delegate send/recv calls
	mqtt_pipe_t    *mqtt_pipe;
	nni_sock       *nsock;
	nni_list        recv_queue; // ctx pending to receive
	nni_list        send_queue; // ctx pending to send (only offline msg)
	reason_code     disconnect_code; // disconnect reason code
	property       *dis_prop;        // disconnect property
	nni_id_map      sent_unack;      // send messages unacknowledged
#ifdef NNG_SUPP_SQLITE
	nni_mqtt_sqlite_option *sqlite_opt;
#endif
	// user defined option
	nni_time retry_wait;
	uint8_t  timeout_backoff;
#ifdef NNG_ENABLE_STATS
	nni_stat_item st_rcv_max;
	nni_stat_item mqtt_reconnect;
	nni_stat_item msg_resend;
	nni_stat_item msg_send_drop;
	nni_stat_item msg_recv_drop;
#endif
};

/******************************************************************************
 *                              Sock Implementation                           *
 ******************************************************************************/

static void
mqttv5_sock_init(void *arg, nni_sock *sock)
{
	mqtt_sock_t *s = arg;
	mqtt_sock_init(arg, sock);
	s->mqtt_ver = MQTT_PROTOCOL_VERSION_v5;
}

static void
mqtt_sock_init(void *arg, nni_sock *sock)
{
	mqtt_sock_t *s = arg;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);
	nni_atomic_set(&s->next_packet_id, 1);

	// this is "semi random" start for request IDs.
	s->retry      = NNI_SECOND * 5;
	s->retry_wait = NNI_SECOND * 3;
	s->keepalive  = NNI_SECOND * 10; // default mqtt keepalive
	s->timeleft   = NNI_SECOND * 10;

	s->timeout_backoff = 1;
	s->nsock           = sock;

	nni_mtx_init(&s->mtx);
	mqtt_ctx_init(&s->master, s);

	s->mqtt_ver  = MQTT_PROTOCOL_VERSION_v311;
	s->mqtt_pipe = NULL;
	NNI_LIST_INIT(&s->recv_queue, mqtt_ctx_t, rqnode);
	NNI_LIST_INIT(&s->send_queue, mqtt_ctx_t, sqnode);

	nni_id_map_init(&s->sent_unack, 0x0000u, 0xffffu, true);

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
	nni_sock_add_stat(s->nsock, &s->mqtt_reconnect);
	nni_sock_add_stat(s->nsock, &s->msg_resend);
	nni_sock_add_stat(s->nsock, &s->msg_send_drop);
	nni_sock_add_stat(s->nsock, &s->msg_recv_drop);
#endif
}

static void
mqtt_sock_fini(void *arg)
{
	mqtt_sock_t *s = arg;
	nni_id_map_fini(&s->sent_unack);
#ifdef NNG_SUPP_SQLITE
	nni_mqtt_sqlite_db_fini(s->sqlite_opt);
	nng_mqtt_free_sqlite_opt(s->sqlite_opt);
#endif
	mqtt_ctx_fini(&s->master);

	nni_mtx_fini(&s->mtx);
}

static void
mqtt_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static int
mqtt_sock_get_disconnect_prop(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	int          rv;

	nni_mtx_lock(&s->mtx);
	rv = nni_copyout_ptr(s->dis_prop, v, szp, t);
	nni_mtx_lock(&s->mtx);
	return (rv);
}

static int
mqtt_sock_get_disconnect_code(void *arg, void *v, size_t *sz, nni_opt_type t)
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

#ifdef NNG_SUPP_SQLITE
static void *
mqtt_sock_get_sqlite_option(mqtt_sock_t *s)
{
	return (s->sqlite_opt);
}
#endif

static int
mqtt_sock_set_retry_interval(void *arg, const void *v, size_t sz, nni_opt_type t)
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
mqtt_sock_set_retry_wait(void *arg, const void *v, size_t sz, nni_opt_type t)
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
mqtt_sock_set_retry_qos_0(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtt_sock_t *s = arg;
	bool tmp;
	int rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		s->retry_qos_0 = tmp;
	}
	return (rv);
}

static int
mqtt_sock_get_pipeid(void *arg, void *buf, size_t *szp, nni_type t)
{
	// For MQTT Client, only has one pipe
	mqtt_sock_t *s = arg;
	uint32_t     pid;
	if (s->mqtt_pipe == NULL) {
		pid = 0xffffffff;
		nni_copyout_u64(pid, buf, szp, t);
		return NNG_ECLOSED;
	}

	nni_pipe *npipe = s->mqtt_pipe->pipe;
	pid = nni_pipe_id(npipe);

	return (nni_copyout_u64(pid, buf, szp, t));
}

static int
mqtt_sock_set_sqlite_option(void *arg, const void *v, size_t sz, nni_opt_type t)
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

static void
mqtt_sock_close(void *arg)
{
	mqtt_sock_t *s = arg;
	mqtt_ctx_t * ctx;
	nni_aio *    aio;
	nni_msg *    msg;

	nni_atomic_set_bool(&s->closed, true);
	// clean ctx queue when pipe was closed.
	while ((ctx = nni_list_first(&s->send_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->send_queue, ctx);
		aio       = ctx->saio;
		ctx->saio = NULL;
		msg       = nni_aio_get_msg(aio);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		nni_msg_free(msg);
	}
	while ((ctx = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, ctx);
		aio       = ctx->raio;
		ctx->raio = NULL;
		// there should be no msg waiting
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	nni_id_map_foreach(&s->sent_unack, mqtt_close_unack_aio_cb);
}

static void
mqtt_sock_send(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s = arg;
	mqtt_ctx_send(&s->master, aio);
}

static void
mqtt_sock_recv(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s = arg;
	mqtt_ctx_recv(&s->master, aio);
}

/******************************************************************************
 *                              Pipe Implementation                           *
 ******************************************************************************/

static int
mqtt_pipe_init(void *arg, nni_pipe *pipe, void *s)
{
	mqtt_pipe_t *p = arg;

	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, true);
	p->pipe      = pipe;
	p->mqtt_sock = s;
	p->rid       = 1;
	p->pingcnt   = 0;
	p->pingmsg   = NULL;
	nni_msg_alloc(&p->pingmsg, 0);
	if (!p->pingmsg) {
		log_error("Error in create a pingmsg");
		return NNG_ENOMEM;
	} else {
		uint8_t buf[2];
		buf[0] = 0xC0;
		buf[1] = 0x00;
		nni_msg_header_append(p->pingmsg, buf, 2);
	}

	nni_aio_init(&p->send_aio, mqtt_send_cb, p);
	nni_aio_init(&p->recv_aio, mqtt_recv_cb, p);
	nni_aio_init(&p->time_aio, mqtt_timer_cb, p);
	// Packet IDs are 16 bits
	// We start at a random point, to minimize likelihood of
	// accidental collision across restarts.
	nni_id_map_init(&p->recv_unack, 0x0000u, 0xffffu, true);
	nni_lmq_init(&p->recv_messages, NNG_MAX_RECV_LMQ);
	nni_lmq_init(&p->send_messages, NNG_MAX_SEND_LMQ);

	return (0);
}

static void
mqtt_pipe_fini(void *arg)
{
	mqtt_pipe_t *p = arg;
	nni_msg *    msg;

	if ((msg = nni_aio_get_msg(&p->recv_aio)) != NULL) {
		nni_aio_set_msg(&p->recv_aio, NULL);
		nni_msg_free(msg);
	}
	if ((msg = nni_aio_get_prov_data(&p->recv_aio)) != NULL) {
		nni_aio_set_prov_data(&p->recv_aio, NULL);
		nni_msg_free(msg);
	}
	if ((msg = nni_aio_get_msg(&p->send_aio)) != NULL) {
		nni_aio_set_msg(&p->send_aio, NULL);
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&p->mqtt_sock->msg_send_drop, 1);
#endif
	}

	if (p->pingmsg)
		nni_msg_free(p->pingmsg);

	nni_aio_fini(&p->send_aio);
	nni_aio_fini(&p->recv_aio);
	nni_aio_fini(&p->time_aio);

	nni_id_map_fini(&p->recv_unack);
	nni_lmq_fini(&p->recv_messages);
	nni_lmq_fini(&p->send_messages);
}

static inline int
mqtt_pipe_recv_msgq_putq(mqtt_pipe_t *p, nni_msg *msg)
{
	int rv = nni_lmq_put(&p->recv_messages, msg);
	if (rv != 0) {
		// resize to ensure we do not lost messages or just let it go?
		// add option to drop messages
		// if (0 !=
		//     nni_lmq_resize(&p->recv_messages,
		//         nni_lmq_len(&p->recv_messages) * 2)) {
		// 	// drop the message when no memory available
		// 	nni_msg_free(msg);
		// 	return;
		// }
		// nni_lmq_put(&p->recv_messages, msg);
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&p->mqtt_sock->msg_recv_drop, 1);
#endif
	}
	return rv;
}

// Should be called with mutex lock hold. and it will unlock mtx.
// flag indicates if need to skip msg in sqlite 1: check sqlite 0: only aio
static inline void
mqtt_send_msg(nni_aio *aio, mqtt_ctx_t *arg)
{
	mqtt_ctx_t *     ctx   = arg;
	mqtt_sock_t *    s     = ctx->mqtt_sock;
	mqtt_pipe_t *    p     = s->mqtt_pipe;
	uint16_t         ptype = 0, packet_id = 0;
	uint8_t          qos   = 0;
	nni_msg *        msg   = NULL;
	nni_msg *        tmsg  = NULL;
	nni_aio *        taio  = NULL;

	if (p == NULL || nni_atomic_get_bool(&p->closed) || aio == NULL) {
		//pipe closed, should never gets here
		// sending msg on a closed pipe
		goto out;
	}
	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		// start sending cached msg
#if defined(NNG_SUPP_SQLITE)
		nni_mqtt_sqlite_option *sqlite =
		    mqtt_sock_get_sqlite_option(s);
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
	case NNG_MQTT_PINGREQ:
	case NNG_MQTT_DISCONNECT:
		break;

	case NNG_MQTT_PUBLISH:
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (0 == qos) {
			break; // QoS 0 need no packet id
		}
		// FALLTHROUGH
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id = nni_mqtt_msg_get_packet_id(msg);
		taio = nni_id_get(&s->sent_unack, packet_id);
		// pass proto_data to cached aio, either it is freed in ack or in cancel
		nni_aio_set_prov_data(aio, nni_msg_get_proto_data(msg));
		if (taio != NULL) {
			log_warn("Warning : msg %d lost due to "
			                "packetID duplicated!", packet_id);
			nni_id_remove(&s->sent_unack, packet_id);
			nni_aio_finish_error(taio, NNG_ECANCELED);
			nni_msg_free(nni_aio_get_msg(taio));
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
			nni_aio_set_msg(taio, NULL);
		}
		if (0 != nni_id_set(&s->sent_unack, packet_id, aio)) {
			log_warn("QoS aio caching failed. Proceed to send directly");
			nni_aio_finish_error(aio, NNG_ECANCELED);
		} else {
			int rv;
			if ((rv = nni_aio_schedule(aio, mqtt_ctx_cancel_send, ctx)) != 0) {
				log_warn("Cancel_Func scheduling failed, send abort!");
				nni_id_remove(&s->sent_unack, packet_id);
				if (ptype == NNG_MQTT_SUBSCRIBE) {
					nni_aio_set_msg(aio, msg);	// only preserve subscribe msg
				} else {
					nni_aio_set_msg(aio, NULL);
					nni_msg_free(msg);	// User need to realloc this msg again
				}
				nni_mtx_unlock(&s->mtx);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
				if (ptype == NNG_MQTT_SUBSCRIBE)
					nni_aio_finish_error(aio, rv);
				else
					nni_aio_finish(aio, rv, 0);
				return;
			}
			// pass proto_data to cached aio, either it is freed in ack or in cancel
			nni_aio_set_prov_data(aio, nni_msg_get_proto_data(msg));
			nni_msg_clone(msg);
		}
		break;

	default:
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
		log_warn("Malformed msg dropped!");
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	if (!p->busy) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		nni_aio_bump_count(
		    aio, nni_msg_header_len(msg) + nni_msg_len(msg));
		nni_pipe_send(p->pipe, &p->send_aio);
		nni_mtx_unlock(&s->mtx);
		if (0 == qos && ptype != NNG_MQTT_SUBSCRIBE &&
		    ptype != NNG_MQTT_UNSUBSCRIBE) {
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish(aio, 0, 0);
		}
		return;
	}
	if (s->retry_qos_0 || qos > 0) {
		if (nni_lmq_full(&p->send_messages)) {
			log_warn("Cached Message lost! pipe is busy and lmq is full\n");
			(void) nni_lmq_get(&p->send_messages, &tmsg);
			nni_msg_free(tmsg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
			log_info("inflight window size %d", nni_lmq_len(&p->send_messages));
#endif
		}
		if (0 != nni_lmq_put(&p->send_messages, msg)) {
			nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
			log_warn("Message lost while enqueing");
		}
	} else {
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_msg_free(msg);
	}
out:
	nni_mtx_unlock(&s->mtx);
	if (0 == qos && ptype != NNG_MQTT_SUBSCRIBE &&
	    ptype != NNG_MQTT_UNSUBSCRIBE) {
		nni_aio_finish(aio, 0, 0);
	}
	return;
}

static int
mqtt_pipe_start(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	mqtt_ctx_t  *c = NULL;

	nni_mtx_lock(&s->mtx);
	nni_atomic_set_bool(&p->closed, false);
	s->mqtt_pipe       = p;
	s->disconnect_code = SUCCESS;
	s->dis_prop        = NULL;
	p->busy			   = false;

	if ((c = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, c);
		nni_pipe_recv(p->pipe, &p->recv_aio);
		mqtt_send_msg(c->saio, c);
		c->saio = NULL;
		nni_sleep_aio(s->retry, &p->time_aio);
		return (0);
	}
	nni_pipe_recv(p->pipe, &p->recv_aio);
	nni_mtx_unlock(&s->mtx);
	// initiate the global resend timer
	nni_sleep_aio(s->retry, &p->time_aio);
#ifdef NNG_ENABLE_STATS
	nni_stat_inc(&s->mqtt_reconnect, 1);
#endif
	return (0);
}

static void
mqtt_pipe_stop(void *arg)
{
	mqtt_pipe_t *p = arg;
	nni_aio_stop(&p->send_aio);
	nni_aio_stop(&p->recv_aio);
	nni_aio_stop(&p->time_aio);
}

static int
mqtt_pipe_close(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	nni_mtx_lock(&s->mtx);
	nni_atomic_set_bool(&p->closed, true);
	s->mqtt_pipe = NULL;
	nni_aio_close(&p->send_aio);
	nni_aio_close(&p->recv_aio);
	nni_aio_close(&p->time_aio);

#if defined(NNG_SUPP_SQLITE)
	// flush to disk
	if (!nni_lmq_empty(&p->send_messages)) {
		log_info("cached msg into sqlite");
		sqlite_flush_lmq(
		    mqtt_sock_get_sqlite_option(s), &p->send_messages);
	}
#endif
	nni_lmq_flush(&p->send_messages);

	nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);
	nni_mtx_unlock(&s->mtx);

	return 0;
}

// Timer callback, we use it for retransmitting.
static void
mqtt_timer_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	if (nng_aio_result(&p->time_aio) != 0) {
		log_info("Timer aio error!");
		return;
	}
	nni_mtx_lock(&s->mtx);
	if (NULL == p || nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
		return;
	}

	if (p->pingcnt >= s->timeout_backoff) {	// expose it
		log_warn("MQTT Timeout and disconnect");
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->pipe);
		return;
	}

	// Update left time to send pingreq
	s->timeleft -= s->retry;

	if (!p->busy && p->pingmsg && s->timeleft <= 0) {
		p->busy = true;
		s->timeleft = s->keepalive;
		// send pingreq
		nni_msg_clone(p->pingmsg);
		nni_aio_set_msg(&p->send_aio, p->pingmsg);
		nni_pipe_send(p->pipe, &p->send_aio);
		p->pingcnt ++;
		nni_mtx_unlock(&s->mtx);
		log_debug("Send pingreq (sock%p)(%dms)", s, s->keepalive);
		nni_sleep_aio(s->retry, &p->time_aio);
		return;
	}

	// start message resending
	nni_aio *taio = NULL;
	uint16_t pid;
	taio = nni_id_get_min(&s->sent_unack, &pid);
	if (taio != NULL) {
		uint16_t ptype;
		nni_msg *msg  = nni_aio_get_msg(taio);
		nni_time now  = nni_clock();
		nni_time time = now - nni_msg_get_timestamp(msg);
		if (time > s->retry_wait && msg != NULL) {
			ptype = nni_mqtt_msg_get_packet_type(msg);
			if (ptype == NNG_MQTT_PUBLISH) {		
				uint8_t *header = nni_msg_header(msg);
				*header |= 0X08;
			}
			log_debug("trying to resend QoS msg %d", pid);
			nni_msg_clone(msg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_resend, 1);
#endif
			if (!p->busy) {
				p->busy = true;
				nni_aio_set_msg(&p->send_aio, msg);
				log_info("resending QoS msg %d", pid);
				nni_pipe_send(p->pipe, &p->send_aio);
				nni_msg_set_timestamp(msg, now);
				nni_mtx_unlock(&s->mtx);
				nni_sleep_aio(s->retry, &p->time_aio);
				return;
			} else {
				if (nni_lmq_put(&p->send_messages, msg) != 0) {
					log_info("%d resend canceld due to full lmq", pid);
					nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
					nni_stat_inc(&s->msg_send_drop, 1);
#endif
				}
			}
		}
	}
#if defined(NNG_SUPP_SQLITE)
	if (!p->busy) {
		nni_msg     *msg = NULL;
		nni_mqtt_sqlite_option *sqlite =
		    mqtt_sock_get_sqlite_option(s);
		if (sqlite_is_enabled(sqlite)) {
			if (!nni_lmq_empty(&sqlite->offline_cache)) {
				sqlite_flush_offline_cache(sqlite);
			}
			if (NULL != (msg = sqlite_get_cache_msg(sqlite))) {
				p->busy = true;
				nni_aio_set_msg(&p->send_aio, msg);
				nni_pipe_send(p->pipe, &p->send_aio);
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

static void
mqtt_send_cb(void *arg)
{
	mqtt_pipe_t *p   = arg;
	mqtt_sock_t *s   = p->mqtt_sock;
	mqtt_ctx_t * c   = NULL;
	nni_msg *    msg = NULL;
	int          rv;

	if ((rv = nni_aio_result(&p->send_aio)) != 0) {
		// We failed to send... clean up and deal with it.
		nni_msg_free(nni_aio_get_msg(&p->send_aio));
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_aio_set_msg(&p->send_aio, NULL);
		log_warn("MQTT client send error %d!", rv);
		s->disconnect_code = 0x8B; // TODO hardcode
		nni_pipe_close(p->pipe);
		return;
	}
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		// This occurs if the mqtt_pipe_close has been called.
		// In that case we don't want any more processing.
		return;
	}
	nni_mtx_lock(&s->mtx);
	p->busy     = false;

	// Check cached ctx in nni_list first
	// these ctxs are triggered before the pipe is established
	if ((c = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, c);
		mqtt_send_msg(c->saio, c);
		c->saio = NULL;
		return;
	}
	if (nni_lmq_get(&p->send_messages, &msg) == 0) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		nni_pipe_send(p->pipe, &p->send_aio);
		nni_mtx_unlock(&s->mtx);
		return;
	}

	p->busy = false;
	nni_mtx_unlock(&s->mtx);
	return;
}

static void
mqtt_recv_cb(void *arg)
{
	int          rv;
	mqtt_pipe_t *p          = arg;
	mqtt_sock_t *s          = p->mqtt_sock;
	nni_aio     *user_aio   = NULL;
	nni_msg     *cached_msg = NULL;
	mqtt_ctx_t  *ctx;

	if ((rv = nni_aio_result(&p->recv_aio)) != 0) {
		log_warn("MQTT client recv error %d!", rv);
		s->disconnect_code = 0x8B; // TODO hardcode, Different with code in v5
		nni_pipe_close(p->pipe);
		return;
	}

	nni_mtx_lock(&s->mtx);
	nni_msg *msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		// free msg and dont return data when pipe is closed.
		if (msg) {
			nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_recv_drop, 1);
#endif
		}
		nni_mtx_unlock(&s->mtx);
		return;
	}
	nni_msg *ack_msg = NULL;
	if ((ack_msg = nni_aio_get_prov_data(&p->recv_aio)) != NULL) {
		nni_aio_set_prov_data(&p->recv_aio, NULL);
		if (!p->busy) {
			p->busy = true;
			nni_aio_set_msg(&p->send_aio, ack_msg);
			nni_pipe_send(p->pipe, &p->send_aio);
		} else {
			if (0 != nni_lmq_put(&p->send_messages, ack_msg)) {
				log_warn("Warning! ack msg lost due to busy socket");
				nni_msg_free(ack_msg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
			}
		}
	}
	nni_msg_set_pipe(msg, nni_pipe_id(p->pipe));
	nni_mqtt_msg_proto_data_alloc(msg);
	if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v311) {
		rv = nni_mqtt_msg_decode(msg);
		if (rv != MQTT_SUCCESS) {
			log_warn("MQTT client decode error %d!", rv);
			nni_mtx_unlock(&s->mtx);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_recv_drop, 1);
#endif
			nni_msg_free(msg);
			// close pipe directly, no DISCONNECT for MQTTv3.1.1
			nni_pipe_close(p->pipe);
			return;
		}
	} else if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
		rv = nni_mqttv5_msg_decode(msg);
		if (rv != MQTT_SUCCESS) {
			// Msg should be clear if decode failed. We reuse it to send disconnect.
			// Or it would encode a malformed packet.
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_recv_drop, 1);
#endif
			nni_mqtt_msg_set_packet_type(msg, NNG_MQTT_DISCONNECT);
			nni_mqtt_msg_set_disconnect_reason_code(msg, rv);
			nni_mqtt_msg_set_disconnect_property(msg, NULL);
			// Composed a disconnect msg
			if ((rv = nni_mqttv5_msg_encode(msg)) != MQTT_SUCCESS) {
				log_error("Error in encoding disconnect.\n");
				nni_msg_free(msg);
				nni_mtx_unlock(&s->mtx);
				nni_pipe_close(p->pipe);
				return;
			}
			if (!p->busy) {
				p->busy = true;
				nni_aio_set_msg(&p->send_aio, msg);
				nni_pipe_send(p->pipe, &p->send_aio);
				nni_mtx_unlock(&s->mtx);
				return;
			}
			if (nni_lmq_full(&p->send_messages)) {
				nni_msg *tmsg;
				(void) nni_lmq_get(&p->send_messages, &tmsg);
				nni_msg_free(tmsg);
			}
			if (0 != nni_lmq_put(&p->send_messages, msg)) {
				nni_msg_free(msg);
				log_warn("Warning! DISCONNECT msg lost due to busy socket");
			}
			nni_mtx_unlock(&s->mtx);
			return;
		}
	} else {
		rv = PROTOCOL_ERROR;
		nni_plat_println("Invalid mqtt version");
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
		// close pipe directly
		nni_pipe_close(p->pipe);
		return;
	}

	packet_type_t packet_type = nni_mqtt_msg_get_packet_type(msg);
	int32_t       packet_id;
	uint8_t       qos;

	// schedule another receive
	nni_pipe_recv(p->pipe, &p->recv_aio);

	// reset ping state
	p->pingcnt = 0;
	s->timeleft = s->keepalive;

	// state transitions
	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		// never reach here
		nni_mtx_unlock(&s->mtx);
		return;
	case NNG_MQTT_PUBACK:
		// we have received a PUBACK, successful delivery of a QoS 1
		// FALLTHROUGH
	case NNG_MQTT_PUBCOMP:
		// we have received a PUBCOMP, successful delivery of a QoS 2
		// FALLTHROUGH
	case NNG_MQTT_SUBACK:
		// we have received a SUBACK, successful subscription
		// FALLTHROUGH
	case NNG_MQTT_UNSUBACK:
		// we have received a UNSUBACK, successful unsubscription
		packet_id  = nni_mqtt_msg_get_packet_id(msg);
		p->rid ++;
		user_aio = nni_id_get(&s->sent_unack, packet_id);
		if (user_aio != NULL) {
			nni_id_remove(&s->sent_unack, packet_id);
			// in case data race in cancel
			nni_aio_set_prov_data(user_aio, NULL);
			nni_msg_free(nni_aio_get_msg(user_aio));
			nni_aio_set_msg(user_aio, NULL);
			if (packet_type == NNG_MQTT_SUBACK ||
			    packet_type == NNG_MQTT_UNSUBACK) {
				nni_msg_clone(msg);
				nni_aio_set_msg(user_aio, msg);
			}
		} else
			log_warn("QoS msg ack failed %d", packet_id);
		nni_msg_free(msg);
		break;

	case NNG_MQTT_PINGRESP:
		// free msg
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		return;

	case NNG_MQTT_PUBREC:
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
		return;

	case NNG_MQTT_PUBREL:
		packet_id  = nni_mqtt_msg_get_pubrel_packet_id(msg);
		cached_msg = nni_id_get(&p->recv_unack, packet_id);
		nni_msg_free(msg);
		if (cached_msg == NULL) {
			log_warn("ERROR! packet id %d not found\n", packet_id);
			break;
		}
		nni_id_remove(&p->recv_unack, packet_id);
		// return msg to user APP
		if ((ctx = nni_list_first(&s->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			if (mqtt_pipe_recv_msgq_putq(p, cached_msg) != 0) {
				log_warn("ERROR: no ctx found! msg queue full! QoS2 msg lost!");
			}
			nni_mtx_unlock(&s->mtx);
			return;
		}

		nni_list_remove(&s->recv_queue, ctx);
		user_aio  = ctx->raio;
		ctx->raio = NULL;
		nni_aio_set_msg(user_aio, cached_msg);
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish(user_aio, 0, 0);
		return;

	case NNG_MQTT_PUBLISH:
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);
		nni_msg_set_cmd_type(msg, CMD_PUBLISH);
		if (2 > qos) {
			// QoS 0, successful receipt
			// QoS 1, the transport handled sending a PUBACK
			if ((ctx = nni_list_first(&s->recv_queue)) == NULL) {
				// No one waiting to receive yet, putting msg into lmq
				if (mqtt_pipe_recv_msgq_putq(p, msg) != 0) {
					log_warn("Warning: no ctx found!! PUB msg lost!");
				}
				nni_mtx_unlock(&s->mtx);
				return;
			}
			nni_list_remove(&s->recv_queue, ctx);
			user_aio  = ctx->raio;
			ctx->raio = NULL;
			nni_aio_set_msg(user_aio, msg);
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish(user_aio, 0, 0);
			return;
		} else {
			packet_id = nni_mqtt_msg_get_publish_packet_id(msg);
			if ((cached_msg = nni_id_get(
			         &p->recv_unack, packet_id)) != NULL) {
				// packetid already exists.
				// sth wrong with the broker replace old with new
				log_error("packet id %d duplicates, old msg lost", packet_id);
				nni_msg_free(cached_msg);
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_recv_drop, 1);
#endif
				// nni_id_remove(&pipe->nano_qos_db, pid);
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
		}
		break;
	case NNG_MQTT_DISCONNECT:
		if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v311) {
			s->disconnect_code = NORMAL_DISCONNECTION;
		} else if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
			s->disconnect_code = *(uint8_t *)nni_msg_body(msg);
		} else {
			log_error("Invalid mqtt version");
		}
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		nni_pipe_close(p->pipe);
		return;
	default:
		// unexpected packet type, server misbehaviour
		nni_mtx_unlock(&s->mtx);
		if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v311) {
			s->disconnect_code = PROTOCOL_ERROR;
		} else if (s->mqtt_ver == MQTT_PROTOCOL_VERSION_v5) {
			s->disconnect_code = MALFORMED_PACKET;
		} else {
			log_error("Invalid mqtt version");
		}
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_recv_drop, 1);
#endif
		nni_pipe_close(p->pipe);
		return;
	}

	nni_mtx_unlock(&s->mtx);
	if (user_aio) {
		nni_aio_finish(user_aio, 0, 0);
	}

	return;
}

/******************************************************************************
 *                           Context Implementation                           *
 ******************************************************************************/

static void
mqtt_ctx_init(void *arg, void *sock)
{
	mqtt_ctx_t * ctx = arg;
	mqtt_sock_t *s   = sock;

	ctx->mqtt_sock = s;
	ctx->raio      = NULL;
	ctx->saio      = NULL;
	NNI_LIST_NODE_INIT(&ctx->sqnode);
	NNI_LIST_NODE_INIT(&ctx->rqnode);
}

static void
mqtt_ctx_fini(void *arg)
{
	mqtt_ctx_t * ctx = arg;
	mqtt_sock_t *s   = ctx->mqtt_sock;
	nni_aio *    aio;

	nni_mtx_lock(&s->mtx);
	if (nni_list_active(&s->send_queue, ctx)) {
		if ((aio = ctx->saio) != NULL) {
			ctx->saio = NULL;
			nni_list_remove(&s->send_queue, ctx);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
	} else if (nni_list_active(&s->recv_queue, ctx)) {
		if ((aio = ctx->raio) != NULL) {
			ctx->raio = NULL;
			nni_list_remove(&s->recv_queue, ctx);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
	}
	nni_mtx_unlock(&s->mtx);
}


static void
mqtt_ctx_cancel_send(nni_aio *aio, void *arg, int rv)
{
	uint16_t             packet_id = 1;
	mqtt_ctx_t          *ctx = arg;
	mqtt_sock_t         *s   = ctx->mqtt_sock;
	mqtt_pipe_t         *p;
	nni_mqtt_proto_data *proto_data;

	// if (rv != NNG_ETIMEDOUT)
	// 	return;
	NNI_ARG_UNUSED(rv);
	nni_mtx_lock(&s->mtx);
	if (nni_list_active(&s->send_queue, ctx)) {
		nni_list_remove(&s->send_queue, ctx);
		nni_list_node_remove(&ctx->sqnode);
	}

	ctx->saio = NULL;
	// deal with canceld QoS msg
	proto_data = nni_aio_get_prov_data(aio);
	if (proto_data) {
		uint8_t type = proto_data->fixed_header.common.packet_type;
		if (type == NNG_MQTT_PUBLISH)
			packet_id = proto_data->var_header.publish.packet_id;
		else if (type == NNG_MQTT_SUBSCRIBE)
			packet_id = proto_data->var_header.subscribe.packet_id;
		else if (type == NNG_MQTT_UNSUBSCRIBE)
			packet_id = proto_data->var_header.unsubscribe.packet_id;
		else
			log_error("Canceling a non QoS msg!");
		p = s->mqtt_pipe;
		if (p != NULL) {
			nni_aio *taio;
			taio = nni_id_get(&s->sent_unack, packet_id);
			if (taio != NULL) {
				log_warn("Warning : QoS action of msg %d is canceled due to "
								"timeout!", packet_id);
				nni_id_remove(&s->sent_unack, packet_id);
				nni_msg_free(nni_aio_get_msg(taio));
#ifdef NNG_ENABLE_STATS
				nni_stat_inc(&s->msg_send_drop, 1);
#endif
				nni_aio_set_msg(taio, NULL);
				nni_aio_set_prov_data(taio, NULL);
			}
			if (taio == aio)
				nni_aio_finish_error(aio, NNG_ECANCELED);
			else
				log_error("canceling wrong aio!");
		}
	}

	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
	}
	nni_mtx_unlock(&s->mtx);
}

static void
mqtt_ctx_send(void *arg, nni_aio *aio)
{
	int          rv;
	mqtt_ctx_t * ctx = arg;
	mqtt_sock_t *s   = ctx->mqtt_sock;
	mqtt_pipe_t *p;
	nni_msg     *msg;
	uint8_t      qos;
	uint16_t     packet_id;

	if (nni_aio_begin(aio) != 0) {
		return;
	}

	if (nni_atomic_get_bool(&s->closed)) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	nni_mtx_lock(&s->mtx);
	msg   = nni_aio_get_msg(aio);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	//set pid
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
		log_error("MQTT client encoding msg failed%d!", rv);
		nni_msg_free(msg);
#ifdef NNG_ENABLE_STATS
		nni_stat_inc(&s->msg_send_drop, 1);
#endif
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	p = s->mqtt_pipe;
	if (p == NULL) {
		// connection is lost or not established yet
#if defined(NNG_SUPP_SQLITE)
		nni_mqtt_sqlite_option *sqlite =
		    mqtt_sock_get_sqlite_option(s);
		if (sqlite_is_enabled(sqlite)
				&& (qos > 0 || s->retry_qos_0)) {
			// the msg order is exactly as same as the ctx
			// in send_queue
			nni_lmq_put(&sqlite->offline_cache, msg);
			if (nni_lmq_full(&sqlite->offline_cache)) {
				sqlite_flush_offline_cache(sqlite);
			}
			nni_mtx_unlock(&s->mtx);
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish_error(aio, NNG_ECLOSED);
			return;
		}
#endif
		if (!nni_list_active(&s->send_queue, ctx)) {
			// cache ctx
			ctx->saio = aio;
			nni_list_append(&s->send_queue, ctx);
			nni_mtx_unlock(&s->mtx);
			log_warn("client sending msg while disconnected! cached");
		} else {
			nni_msg_free(msg);
			nni_mtx_unlock(&s->mtx);
#ifdef NNG_ENABLE_STATS
			nni_stat_inc(&s->msg_send_drop, 1);
#endif
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish_error(aio, NNG_ECLOSED);
			log_warn("ctx is already cached! drop msg");
			log_info("Commercial ver assures you no msg lost!");
		}
		return;
	}
	mqtt_send_msg(aio, ctx);
	log_trace("client sending msg now");
	return;
}

static void
mqtt_ctx_recv(void *arg, nni_aio *aio)
{
	mqtt_ctx_t  *ctx = arg;
	mqtt_sock_t *s   = ctx->mqtt_sock;
	mqtt_pipe_t *p;
	nni_msg     *msg = NULL;

	if (nni_aio_begin(aio) != 0) {
		log_error("aio begin failed");
		return;
	}

	nni_mtx_lock(&s->mtx);
	p = s->mqtt_pipe;
	if (p == NULL) {
		goto wait;
	}
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	if (nni_lmq_get(&p->recv_messages, &msg) == 0) {
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&s->mtx);
		// let user gets a quick reply
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}

	// no open pipe or msg waiting
wait:
	if (ctx->raio != NULL) {
		nni_mtx_unlock(&s->mtx);
		log_error("ERROR! former aio not finished!");
		nni_aio_finish_error(aio, NNG_ESTATE);
		return;
	}
	ctx->raio = aio;
	nni_list_append(&s->recv_queue, ctx);
	nni_mtx_unlock(&s->mtx);
	return;
}

static nni_proto_pipe_ops mqtt_pipe_ops = {
	.pipe_size  = sizeof(mqtt_pipe_t),
	.pipe_init  = mqtt_pipe_init,
	.pipe_fini  = mqtt_pipe_fini,
	.pipe_start = mqtt_pipe_start,
	.pipe_close = mqtt_pipe_close,
	.pipe_stop  = mqtt_pipe_stop,
};

static nni_option mqtt_ctx_options[] = {
	{
	    .o_name = NULL,
	},
};

static nni_proto_ctx_ops mqtt_ctx_ops = {
	.ctx_size    = sizeof(mqtt_ctx_t),
	.ctx_init    = mqtt_ctx_init,
	.ctx_fini    = mqtt_ctx_fini,
	.ctx_recv    = mqtt_ctx_recv,
	.ctx_send    = mqtt_ctx_send,
	.ctx_options = mqtt_ctx_options,
};

static nni_option mqtt_sock_options[] = {
	{
	    .o_name = NNG_OPT_MQTT_DISCONNECT_REASON,
	    .o_get  = mqtt_sock_get_disconnect_code,
	},
	{
	    .o_name = NNG_OPT_MQTT_DISCONNECT_PROPERTY,
	    .o_get  = mqtt_sock_get_disconnect_prop,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_INTERVAL,
	    .o_set  = mqtt_sock_set_retry_interval,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_WAIT_TIME,
	    .o_set  = mqtt_sock_set_retry_wait,
	},
	{
	    .o_name = NNG_OPT_MQTT_RETRY_QOS_0,
	    .o_set  = mqtt_sock_set_retry_qos_0,
	},
	{
	    .o_name = NNG_OPT_MQTT_SQLITE,
	    .o_set  = mqtt_sock_set_sqlite_option,
	},
	{
	    .o_name = NNG_OPT_MQTT_CLIENT_PIPEID,
	    .o_get  = mqtt_sock_get_pipeid,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static nni_proto_sock_ops mqtt_sock_ops = {
	.sock_size    = sizeof(mqtt_sock_t),
	.sock_init    = mqtt_sock_init,
	.sock_fini    = mqtt_sock_fini,
	.sock_open    = mqtt_sock_open,
	.sock_close   = mqtt_sock_close,
	.sock_options = mqtt_sock_options,
	.sock_send    = mqtt_sock_send,
	.sock_recv    = mqtt_sock_recv,
};

static nni_proto_sock_ops mqttv5_sock_ops = {
	.sock_size    = sizeof(mqtt_sock_t),
	.sock_init    = mqttv5_sock_init,
	.sock_fini    = mqtt_sock_fini,
	.sock_open    = mqtt_sock_open,
	.sock_close   = mqtt_sock_close,
	.sock_options = mqtt_sock_options,
	.sock_send    = mqtt_sock_send,
	.sock_recv    = mqtt_sock_recv,
};

static nni_proto mqtt_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MQTT_SELF, NNG_MQTT_SELF_NAME },
	.proto_peer     = { NNG_MQTT_PEER, NNG_MQTT_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mqtt_sock_ops,
	.proto_pipe_ops = &mqtt_pipe_ops,
	.proto_ctx_ops  = &mqtt_ctx_ops,
};

static nni_proto mqttv5_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MQTT_SELF, NNG_MQTT_SELF_NAME },
	.proto_peer     = { NNG_MQTT_PEER, NNG_MQTT_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mqttv5_sock_ops,
	.proto_pipe_ops = &mqtt_pipe_ops,
	.proto_ctx_ops  = &mqtt_ctx_ops,
};

int
nng_mqtt_client_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mqtt_proto));
}

int
nng_mqttv5_client_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mqttv5_proto));
}
