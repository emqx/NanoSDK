//
// Copyright 2022 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "nng/protocol/mqtt/mqtt.h"
#include "supplemental/quic/quic_api.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/mqtt/mqtt_qos_db_api.h"

#define NNG_MQTT_SELF 0
#define NNG_MQTT_SELF_NAME "mqtt-client"
#define NNG_MQTT_PEER 0
#define NNG_MQTT_PEER_NAME "mqtt-server"

#define DB_NAME "mqtt_qos_db_quic.db"

typedef struct mqtt_sock_s mqtt_sock_t;
typedef struct mqtt_pipe_s mqtt_pipe_t;
typedef struct mqtt_quic_ctx mqtt_quic_ctx;
typedef nni_mqtt_packet_type packet_type_t;

static void mqtt_quic_sock_init(void *arg, nni_sock *sock);
static void mqtt_quic_sock_fini(void *arg);
static void mqtt_quic_sock_open(void *arg);
static void mqtt_quic_sock_send(void *arg, nni_aio *aio);
static void mqtt_quic_sock_recv(void *arg, nni_aio *aio);
static void mqtt_quic_send_cb(void *arg);
static void mqtt_quic_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

static int  quic_mqtt_stream_init(void *arg, nni_pipe *qstrm, void *sock);
static void quic_mqtt_stream_fini(void *arg);
static int  quic_mqtt_stream_start(void *arg);
static void quic_mqtt_stream_stop(void *arg);
static void quic_mqtt_stream_close(void *arg);

static void mqtt_quic_ctx_init(void *arg, void *sock);
static void mqtt_quic_ctx_fini(void *arg);
static void mqtt_quic_ctx_recv(void *arg, nni_aio *aio);
static void mqtt_quic_ctx_send(void *arg, nni_aio *aio);

// TODO as same as the mqtt_client mqttv5_client. move to supplemental!
static void  flush_offline_cache(mqtt_sock_t *s);
static nni_msg* get_cache_msg(mqtt_sock_t *s);

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
	nni_atomic_bool closed;
	nni_duration    retry;
	mqtt_pipe_t    *pipe;
#ifdef NNG_SUPP_SQLITE
	sqlite3 *sqlite_db;
	nni_lmq  offline_cache;
#endif
#ifdef NNG_HAVE_MQTT_BROKER
	conf_bridge_node *bridge_conf;
#endif
	nni_mtx mtx; // more fine grained mutual exclusion
	mqtt_quic_ctx master; // to which we delegate send/recv calls
	// mqtt_pipe_t *   mqtt_pipe;
	nni_list recv_queue;    // aio pending to receive
	nni_list send_queue;    // aio pending to send
	nni_lmq  send_messages; // send messages queue
	nni_aio  time_aio;      // timer aio to resend unack msg
	uint16_t counter;
	uint16_t keepalive; // MQTT keepalive
	nni_msg *ping_msg, *connmsg;

	struct mqtt_client_cb cb; // user cb
};

// A mqtt_pipe_s is our per-pipe protocol private structure.
struct mqtt_pipe_s {
	void        *stream;
	void           *qstream; // nni_pipe
	nni_atomic_bool closed;
	bool            busy;
	nni_atomic_int  next_packet_id; // next packet id to use
	mqtt_sock_t *mqtt_sock;
	nni_id_map sent_unack; // send messages unacknowledged
	nni_id_map recv_unack;    // recv messages unacknowledged
	nni_aio    send_aio;      // send aio to the underlying transport
	nni_aio    recv_aio;      // recv aio to the underlying transport
	nni_aio	   rep_aio;	  // aio for resending qos msg and PINGREQ  
	nni_lmq    recv_messages; // recv messages queue
};

static inline void
mqtt_pipe_recv_msgq_putq(mqtt_pipe_t *p, nni_msg *msg)
{
	if (0 != nni_lmq_put(&p->recv_messages, msg)) {
		// resize to ensure we do not lost messages or just lose it?
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
	}
}

static uint16_t
mqtt_pipe_get_next_packet_id(mqtt_pipe_t *p)
{
	int packet_id;
	do {
		packet_id = nni_atomic_get(&p->next_packet_id);
	} while (
	    !nni_atomic_cas(&p->next_packet_id, packet_id, packet_id + 1));
	return packet_id & 0xFFFF;
}


static void
flush_offline_cache(mqtt_sock_t *s)
{
#if defined(NNG_HAVE_MQTT_BROKER) && defined(NNG_SUPP_SQLITE)
	if (s->bridge_conf) {
		char *config_name = get_config_name(s);
		nni_mqtt_qos_db_set_client_offline_msg_batch(s->sqlite_db,
		    &s->offline_cache, config_name,
		    MQTT_PROTOCOL_VERSION_v311);
		nni_mqtt_qos_db_remove_oldest_client_offline_msg(s->sqlite_db,
		    s->bridge_conf->sqlite->disk_cache_size, config_name);
	}
#else
	NNI_ARG_UNUSED(s);
#endif
}

static inline nni_msg *
get_cache_msg(mqtt_sock_t *s)
{
	nni_msg *msg = NULL;
#if defined(NNG_HAVE_MQTT_BROKER)
	if (s->bridge_conf == NULL) {
		return NULL;
	}
	conf_sqlite *sqlite = s->bridge_conf->sqlite;
#if defined(NNG_SUPP_SQLITE)
	if (sqlite->enable) {
		int64_t row_id = 0;

		msg = nni_mqtt_qos_db_get_client_offline_msg(
		    s->sqlite_db, &row_id, get_config_name(s));
		if (!nni_lmq_empty(&s->offline_cache)) {
			flush_offline_cache(s);
		}
		if (msg != NULL) {
			nni_mqtt_qos_db_remove_client_offline_msg(
			    s->sqlite_db, row_id);
		}
	}
#else
	NNI_ARG_UNUSED(sqlite);
	return NULL;
#endif
#else
	return NULL;
#endif
	return msg;
}

// Should be called with mutex lock hold after pipe is secured
// return rv>0 when aio should be finished (error or successed)
static inline int
mqtt_send_msg(nni_aio *aio, nni_msg *msg, mqtt_sock_t *s)
{
	mqtt_pipe_t *p   = s->pipe;
	nni_msg     *tmsg;
	uint16_t     ptype, packet_id;
	uint8_t      qos = 0;

	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
		// TODO : only send CONNECT once
		s->connmsg = msg;
		nni_msg_clone(s->connmsg);
		s->keepalive = nni_mqtt_msg_get_connect_keep_alive(msg);
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
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id     = mqtt_pipe_get_next_packet_id(p);
		nni_mqtt_msg_set_packet_id(msg, packet_id);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_id_get(&p->sent_unack, packet_id);
		if (tmsg != NULL) {
			nni_plat_printf("Warning : msg %d lost due to "
			                "packetID duplicated!",
			    packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio) {
				nni_aio_finish_error(m_aio, UNSPECIFIED_ERROR);
			}
			nni_msg_free(tmsg);
			nni_id_remove(&p->sent_unack, packet_id);
		}
		nni_msg_clone(msg);
		if (0 != nni_id_set(&p->sent_unack, packet_id, msg)) {
			nni_println("Warning! Cache QoS msg failed");
			nni_msg_free(msg);
			nni_aio_finish_error(aio, UNSPECIFIED_ERROR);
		}
		break;
	default:
		return NNG_EPROTO;
	}
	if (!p->busy) {
		nni_mqtt_msg_encode(msg);
		nni_aio_set_msg(&p->send_aio, msg);
		p->busy = true;
		quic_strm_send(p->qstream, &p->send_aio);
	} else {
		if (nni_lmq_full(&s->send_messages)) {
			(void) nni_lmq_get(&s->send_messages, &tmsg);
			nni_msg_free(tmsg);
		}
		if (0 != nni_lmq_put(&s->send_messages, msg)) {
			nni_println(
			    "Warning! msg send failed due to busy socket");
		}
	}
	if (0 == qos && ptype != NNG_MQTT_SUBSCRIBE &&
	    ptype != NNG_MQTT_UNSUBSCRIBE) {
		return 0;
	}
	return -1;
}

// static void
// mqtt_qos_send_cb(void *arg)
// {
// }


static int
quic_sock_set_conf_with_db(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
#ifdef NNG_HAVE_MQTT_BROKER
	mqtt_sock_t *s = arg;
	if (t == NNI_TYPE_OPAQUE) {
		nni_mtx_lock(&s->mtx);
		s->bridge_conf = (conf_bridge_node *) v;

#ifdef NNG_SUPP_SQLITE
		conf_bridge_node *bridge_conf = s->bridge_conf;
		if (bridge_conf != NULL && bridge_conf->sqlite->enable) {
			s->retry = bridge_conf->sqlite->resend_interval;
			nni_lmq_init(&s->offline_cache,
			    bridge_conf->sqlite->flush_mem_threshold);
			nni_qos_db_init_sqlite(s->sqlite_db,
			    bridge_conf->sqlite->mounted_file_path, DB_NAME,
			    false);
			nni_qos_db_reset_client_msg_pipe_id(
			    bridge_conf->sqlite->enable, s->sqlite_db,
			    bridge_conf->name);
			nni_mqtt_qos_db_set_client_info(s->sqlite_db,
			    bridge_conf->name, NULL, "MQTT",
			    bridge_conf->proto_ver);
		}
#endif
		nni_mtx_unlock(&s->mtx);
		return 0;
	}
#else
	NNI_ARG_UNUSED(arg);
	NNI_ARG_UNUSED(v);
	NNI_ARG_UNUSED(t);
#endif
	return NNG_EUNREACHABLE;
}

static void
mqtt_quic_send_cb(void *arg)
{
	mqtt_pipe_t *p   = arg;
	mqtt_sock_t *s   = p->mqtt_sock;
	nni_msg *    msg = NULL;
	nni_aio * aio;


	if (nni_aio_result(&p->send_aio) != 0) {
		// We failed to send... clean up and deal with it.
		nni_msg_free(nni_aio_get_msg(&p->send_aio));
		nni_aio_set_msg(&p->send_aio, NULL);
		// TODO close quic stream
		return;
	}
	nni_mtx_lock(&s->mtx);
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		// This occurs if the mqtt_pipe_close has been called.
		// In that case we don't want any more processing.
		nni_mtx_unlock(&s->mtx);
		return;
	}
	// Check cached aio first
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		int rv = 0;
		if ((rv = mqtt_send_msg(aio, msg, s)) >= 0){
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish(aio, rv, 0);
			return;
		}
		nni_mtx_unlock(&s->mtx);
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}
	// Check cached msg in lmq later
	// this msg is already proessed by mqtt_send_msg
	if (nni_lmq_get(&s->send_messages, &msg) == 0) {
		p->busy = true;
		nni_mqtt_msg_encode(msg);
		nni_aio_set_msg(&p->send_aio, msg);
		quic_strm_send(p->qstream, &p->send_aio);
		nni_mtx_unlock(&s->mtx);
		if (s->cb.msg_send_cb)
			s->cb.msg_send_cb(NULL, s->cb.sendarg);
		return;
	}

	if (NULL != (msg = get_cache_msg(s))) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		quic_strm_send(p->qstream, &p->send_aio);
		nni_mtx_unlock(&s->mtx);
		return;
	}
	p->busy = false;
	nni_mtx_unlock(&s->mtx);

	if (s->cb.msg_send_cb)
		s->cb.msg_send_cb(NULL, s->cb.sendarg);

	return;
}

static void
mqtt_quic_recv_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_aio * user_aio = NULL;
	nni_msg * cached_msg = NULL;
	nni_aio *aio;

	if (nni_aio_result(&p->recv_aio) != 0) {
		// TODO close quic stream
		return;
	}

	nni_mtx_lock(&s->mtx);
	nni_msg *msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		quic_strm_recv(p->qstream, &p->recv_aio);
		return;
	}
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		//free msg and dont return data when pipe is closed.
		if (msg) {
			nni_msg_free(msg);
		}
		nni_mtx_unlock(&s->mtx);
		return;
	}
	// nni_msg_set_pipe(msg, nni_pipe_id(p->pipe));
	nni_mqtt_msg_proto_data_alloc(msg);
	nni_mqtt_msg_decode(msg);

	packet_type_t packet_type = nni_mqtt_msg_get_packet_type(msg);

	int32_t       packet_id;
	uint8_t       qos;

	// schedule another receive
	quic_strm_recv(p->qstream, &p->recv_aio);
	
	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		nni_msg_free(msg);
		break;
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
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			// should we support sub/unsub cb here?
			// if (packet_type == NNG_MQTT_SUBACK ||
			//     packet_type == NNG_MQTT_UNSUBACK) {
			// 	nni_msg_clone(msg);
			// 	nni_aio_set_msg(user_aio, msg);
			// }
			nni_msg_free(cached_msg);
		}
		nni_msg_free(msg);
		break;
	case NNG_MQTT_PUBREL:
		packet_id = nni_mqtt_msg_get_pubrel_packet_id(msg);
		cached_msg = nni_id_get(&p->recv_unack, packet_id);
		nni_msg_free(msg);
		if (cached_msg == NULL) {
			nni_plat_printf("ERROR! packet id %d not found\n", packet_id);
			break;
		}
		nni_id_remove(&p->recv_unack, packet_id);

		// return PUBCOMP
		nni_msg *ack;
		nni_mqtt_msg_alloc(&ack, 0);
		packet_id = nni_mqtt_msg_get_pubrel_packet_id(msg);
		nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBCOMP);
		nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
		nni_mqtt_msg_encode(ack);
		// ignore result of this send ?
		mqtt_send_msg(NULL, ack, s);
		// return msg to user
		if ((aio = nni_list_first(&s->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			mqtt_pipe_recv_msgq_putq(p, cached_msg);
			break;
		}
		nni_list_remove(&s->recv_queue, aio);
		user_aio  = aio;
		nni_aio_set_msg(user_aio, cached_msg);
		break;
	case NNG_MQTT_PUBLISH:
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);

		if (2 > qos) {
			if (qos == 1) {
				// QoS 1 return PUBACK
				nni_msg *ack;
				nni_mqtt_msg_alloc(&ack, 0);
				/*
				uint8_t *payload;
				uint32_t payload_len;
				payload = nng_mqtt_msg_get_publish_payload(msg, &payload_len);
				*/
				packet_id = nni_mqtt_msg_get_publish_packet_id(msg);
				nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBACK);
				nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
				nni_mqtt_msg_encode(ack);
				// ignore result of this send ?
				mqtt_send_msg(NULL, ack, s);
			}
			if ((aio = nni_list_first(&s->recv_queue)) == NULL) {
				// No one waiting to receive yet, putting msg
				// into lmq
				mqtt_pipe_recv_msgq_putq(p, msg);
				// nni_println("ERROR: no ctx found!! create
				// more ctxs!");
				break;
			}
			nni_list_remove(&s->recv_queue, aio);
			user_aio  = aio;
			nni_aio_set_msg(user_aio, msg);
			break;
		} else {
			packet_id = nni_mqtt_msg_get_publish_packet_id(msg);
			if ((cached_msg = nni_id_get(
			         &p->recv_unack, packet_id)) != NULL) {
				// packetid already exists.
				// sth wrong with the broker
				// replace old with new
				nni_plat_printf(
				    "ERROR: packet id %d duplicates in",
				    packet_id);
				nni_msg_free(cached_msg);
				// nni_id_remove(&pipe->nano_qos_db,
				// pid);
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
			// return PUBREC
			nni_msg *ack;
			nni_mqtt_msg_alloc(&ack, 0);
			/*
			uint8_t *payload;
			uint32_t payload_len;
			payload = nng_mqtt_msg_get_publish_payload(
			    msg, &payload_len);
			*/
			nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBREC);
			nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
			nni_mqtt_msg_encode(ack);
			// ignore result of this send ?
			mqtt_send_msg(NULL, ack, s);
		}
		break;
	case NNG_MQTT_PINGRESP:
		// PINGRESP is ignored in protocol layer
		// Rely on health checker of Quic stream
		// free msg
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		return;
	case NNG_MQTT_PUBREC:
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		return;
	default:
		// unexpected packet type, server misbehaviour
		nni_msg_free(msg);
		nni_mtx_unlock(&s->mtx);
		// close quic stream
		// nni_pipe_close(p->pipe);
		return;
	}
	nni_mtx_unlock(&s->mtx);

	if (user_aio) {
		nni_aio_finish(user_aio, 0, 0);
	}

	if (packet_type == NNG_MQTT_CONNACK)
		if (s->cb.connect_cb) // Trigger cb
			s->cb.connect_cb(msg, s->cb.connarg);
	if (packet_type == NNG_MQTT_PUBLISH)
		if (s->cb.msg_recv_cb) // Trigger cb
			s->cb.msg_recv_cb(msg, s->cb.recvarg);
}

// Timer callback, we use it for retransmition.
static void
mqtt_timer_cb(void *arg)
{
	mqtt_sock_t *s = arg;
	mqtt_pipe_t *p = s->pipe;
	nni_msg *  msg;
	nni_aio *  aio;
	uint16_t   pid = 0;

	if (nng_aio_result(&s->time_aio) != 0) {
		return;
	}
	nni_mtx_lock(&s->mtx);
	if (NULL == p || nni_atomic_get_bool(&p->closed)) {
		return;
	}
	s->counter += s->retry;
	if (s->counter > s->keepalive) {
		// send PINGREQ
		nng_aio_wait(&p->rep_aio);
		nni_aio_set_msg(&p->rep_aio, s->ping_msg);
		nni_msg_clone(s->ping_msg);
		quic_strm_send(p->qstream, &p->rep_aio);
		s->counter = 0;
	}

	// start message resending
	msg = nni_id_get_any(&p->sent_unack, &pid);
	if (msg != NULL) {
		uint16_t ptype;
		ptype = nni_mqtt_msg_get_packet_type(msg);
		if (ptype == NNG_MQTT_PUBLISH) {
			nni_mqtt_msg_set_publish_dup(msg, true);
		}
		if (!p->busy) {
			p->busy = true;
			nni_msg_clone(msg);
			nni_mqtt_msg_encode(msg);
			aio = nni_mqtt_msg_get_aio(msg);
			if (aio) {
				nni_aio_bump_count(aio,
				    nni_msg_header_len(msg) +
				        nni_msg_len(msg));
				nni_aio_set_msg(aio, NULL);
			}
			nni_aio_set_msg(&p->send_aio, msg);
			quic_strm_send(p->qstream, &p->send_aio);

			nni_mtx_unlock(&s->mtx);
			nni_sleep_aio(s->retry, &s->time_aio);
			return;
		} else {
			nni_msg_clone(msg);
			nni_lmq_put(&s->send_messages, msg);
		}
	}

	nni_mtx_unlock(&s->mtx);
	nni_sleep_aio(s->retry * NNI_SECOND, &s->time_aio);
	return;
}

/* MQTT over Quic Sock */
/******************************************************************************
 *                            Socket Implementation                           *
 ******************************************************************************/

static void mqtt_quic_sock_init(void *arg, nni_sock *sock)
{
	NNI_ARG_UNUSED(arg);
	NNI_ARG_UNUSED(sock);
	mqtt_sock_t *s = arg;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);

	// this is a pre-defined timer for global timer
	s->retry   = 5;  // 5 seconds as default
	s->counter = 0;
	s->connmsg = NULL;

	nni_mtx_init(&s->mtx);
	mqtt_quic_ctx_init(&s->master, s);

#if defined(NNG_HAVE_MQTT_BROKER) && defined(NNG_SUPP_SQLITE)
	nni_qos_db_init_sqlite(s->sqlite_db,
	    s->bridge_conf->sqlite->mounted_file_path, DB_NAME, false);
	nni_qos_db_reset_client_msg_pipe_id(s->bridge_conf->sqlite->enable,
	    s->sqlite_db, s->bridge_conf->name);
#endif
	nni_lmq_init(&s->send_messages, NNG_MAX_SEND_LMQ);
	nni_aio_list_init(&s->send_queue);
	nni_aio_list_init(&s->recv_queue);
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
	mqtt_sock_t *s = arg;
#if defined(NNG_SUPP_SQLITE) && defined(NNG_HAVE_MQTT_BROKER)
	bool is_sqlite = get_persist(s);
	if (is_sqlite) {
		nni_qos_db_fini_sqlite(s->sqlite_db);
		nni_lmq_fini(&s->offline_cache);
	}
#endif
	mqtt_quic_ctx_fini(&s->master);
	nni_lmq_fini(&s->send_messages);
	nni_aio_fini(&s->time_aio);
}

static void
mqtt_quic_sock_open(void *arg)
{
	mqtt_sock_t *s = arg;
	uint8_t buf[2] = {0xC0,0x00};
	// enable time aio in sock open when utlize 0RTT to unbind MQTT with Gstream
	// nni_sleep_aio(s->retry * NNI_SECOND, &s->time_aio);
	// alloc Ping msg
	nng_msg_alloc(&s->ping_msg, 0);
	nng_msg_header_append(s->ping_msg, buf, 1);
	nng_msg_append(s->ping_msg, buf+1, 1);

	// initiate the global resend timer
	nni_sleep_aio(s->retry * NNI_SECOND, &s->time_aio);
}

static void
mqtt_quic_sock_close(void *arg)
{
	mqtt_sock_t *s = arg;
	nni_msg *msg;
	nni_aio *aio;
	while ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		if (msg != NULL) {
			nni_msg_free(msg);
		}
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	while ((aio = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, aio);
		// there should be no msg waiting
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	nni_aio_stop(&s->time_aio);
	nni_aio_close(&s->time_aio);
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

/******************************************************************************
 *                          Stream(Pipe) Implementation                       *
 ******************************************************************************/

static int
quic_mqtt_stream_init(void *arg,nni_pipe *qstrm, void *sock)
{
	mqtt_pipe_t *p = arg;
	p->qstream = qstrm;
	p->mqtt_sock = sock;
	p->mqtt_sock->pipe = p;

	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, false);
	p->busy   = false;
	nni_atomic_set(&p->next_packet_id, 1);
	nni_aio_init(&p->send_aio, mqtt_quic_send_cb, p);
	nni_aio_init(&p->rep_aio, NULL, p);
	nni_aio_init(&p->recv_aio, mqtt_quic_recv_cb, p);
	// Packet IDs are 16 bits
	// We start at a random point, to minimize likelihood of
	// accidental collision across restarts.
	nni_id_map_init(&p->sent_unack, 0x0000u, 0xffffu, true);
	nni_id_map_init(&p->recv_unack, 0x0000u, 0xffffu, true);
	nni_lmq_init(&p->recv_messages, NNG_MAX_RECV_LMQ);

	return (0);
}

static void
quic_mqtt_stream_fini(void *arg)
{
	nni_plat_printf("quic_mqtt_stream_finit.\n");
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	nni_msg * msg;
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
#if defined(NNG_HAVE_MQTT_BROKER) && defined(NNG_SUPP_SQLITE)
	nni_id_map_fini(&p->sent_unack);
#endif
	nni_id_map_fini(&p->recv_unack);
	nni_lmq_fini(&p->recv_messages);
	if (s->cb.disconnect_cb != NULL) {
		s->cb.disconnect_cb(NULL, s->cb.discarg);
	}
}

static int
quic_mqtt_stream_start(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_aio     *aio;
	nni_msg     *msg;

	nni_mtx_lock(&s->mtx);
	if (s->connmsg!= NULL) {
		nni_msg_clone(s->connmsg);
		mqtt_send_msg(NULL, s->connmsg, s);
	}
	if (NULL != (msg = get_cache_msg(s))) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		quic_strm_send(p->qstream, &p->send_aio);
	}
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		int rv = 0;
		if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish(aio, rv, 0);
			quic_strm_recv(p->qstream, &p->recv_aio);
			return 0;
		}
	}
	nni_mtx_unlock(&s->mtx);
	quic_strm_recv(p->qstream, &p->recv_aio);
	return 0;
}

static void
quic_mqtt_stream_stop(void *arg)
{
	mqtt_pipe_t *p = arg;

	nni_aio_stop(&p->send_aio);
	nni_aio_stop(&p->recv_aio);
	nni_aio_stop(&p->rep_aio);
}

static void
quic_mqtt_stream_close(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	nni_mtx_lock(&s->mtx);
	s->pipe = NULL;
	nni_aio_close(&p->send_aio);
	nni_aio_close(&p->recv_aio);
	nni_aio_close(&p->rep_aio);
	nni_lmq_flush(&p->recv_messages);
	nni_lmq_flush(&s->send_messages);
#if defined(NNG_HAVE_MQTT_BROKER) && defined(NNG_SUPP_SQLITE)
	nni_id_map_foreach(&p->sent_unack, mqtt_close_unack_msg_cb);
#endif
	nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);
	nni_mtx_unlock(&s->mtx);

	nni_atomic_set_bool(&p->closed, true);
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
	mqtt_pipe_t   *p   = s->pipe;
	nni_msg *msg;
	int rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}

	nni_mtx_lock(&s->mtx);

	if (nni_atomic_get_bool(&s->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	msg = nni_aio_get_msg(aio);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}

	if (p == NULL) {
		// connection is lost or not established yet
		if (!nni_list_active(&s->send_queue, aio)) {
			// cache aio
			nni_list_append(&s->send_queue, aio);
			nni_mtx_unlock(&s->mtx);
		} else {
			// aio is already on the list. Wrong behaviour from user
			nni_msg_free(msg);
			nni_mtx_unlock(&s->mtx);
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish_error(aio, NNG_EBUSY);
		}
		return;
	}
	if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
		nni_mtx_unlock(&s->mtx);
		// nni_aio_set_msg(aio, NULL);
		nni_aio_finish(aio, rv, 0);
		return;
	}
	nni_mtx_unlock(&s->mtx);
	nni_aio_set_msg(aio, NULL);
	return;
}

static void
mqtt_quic_ctx_recv(void *arg, nni_aio *aio)
{
	mqtt_quic_ctx *ctx = arg;
	mqtt_sock_t *s   = ctx->mqtt_sock;
	mqtt_pipe_t *p   = s->pipe;
	nni_msg     *msg = NULL;

	if (nni_aio_begin(aio) != 0) {
		return;
	}

	nni_mtx_lock(&s->mtx);
	if (p == NULL) {
		goto wait;
	}

	if (nni_atomic_get_bool(&s->closed) || nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
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
	// no open pipe or msg wating
wait:
	// nni_plat_printf("connection lost! caching aio \n");
	if (!nni_list_active(&s->recv_queue, aio)) {
		// cache aio
		nni_list_append(&s->recv_queue, aio);
		nni_mtx_unlock(&s->mtx);
	} else {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		// nni_println("ERROR! former aio not finished!");
		nni_aio_finish_error(aio, NNG_EBUSY);
	}
	return;
}

static nni_proto_pipe_ops mqtt_quic_pipe_ops = {
	.pipe_size  = sizeof(mqtt_pipe_t),
	.pipe_init  = quic_mqtt_stream_init,
	.pipe_fini  = quic_mqtt_stream_fini,
	.pipe_start = quic_mqtt_stream_start,
	.pipe_close = quic_mqtt_stream_close,
	.pipe_stop  = quic_mqtt_stream_stop,
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
	    .o_name = NANO_CONF,
	    .o_set  = quic_sock_set_conf_with_db,
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
	nni_sock *nsock;
	int rv = 0;
	// Quic settings
	if ((rv = nni_proto_open(sock, &mqtt_msquic_proto)) == 0) {
		// TODO write an independent transport layer for msquic
		nni_sock_find(&nsock, sock->id);
		quic_open();
		quic_proto_open(&mqtt_msquic_proto);
		quic_connect(url, nsock);
	}
	return rv;
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
	return 0;
}

