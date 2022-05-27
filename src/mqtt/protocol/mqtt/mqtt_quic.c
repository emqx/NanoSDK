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
typedef struct mqtt_pipe_s mqtt_pipe_t;
typedef nni_mqtt_packet_type packet_type_t;

static void mqtt_quic_sock_init(void *arg, nni_sock *sock);
static void mqtt_quic_sock_fini(void *arg);
static void mqtt_quic_sock_open(void *arg);
static void mqtt_quic_sock_send(void *arg, nni_aio *aio);
static void mqtt_quic_sock_recv(void *arg, nni_aio *aio);
static void mqtt_quic_send_cb(void *arg);
static void mqtt_quic_recv_cb(void *arg);
static void mqtt_timer_cb(void *arg);

struct mqtt_client_cb {
	int (*connect_cb)(void *);
	int (*msg_send_cb)(void *);
	int (*msg_recv_cb)(void *);
};

// A mqtt_sock_s is our per-socket protocol private structure.
struct mqtt_sock_s {
	bool         closed;
	nni_duration retry;
	mqtt_pipe_t *pipe;
	nni_mtx      mtx; // more fine grained mutual exclusion
	// mqtt_ctx_t      master; // to which we delegate send/recv calls
	// mqtt_pipe_t *   mqtt_pipe;
	nni_list recv_queue;    // aio pending to receive
	nni_list send_queue;    // aio pending to send
	nni_lmq  send_messages; // send messages queue

	struct mqtt_client_cb cb; // user cb
};

// A mqtt_pipe_s is our per-pipe protocol private structure.
struct mqtt_pipe_s {
	void        *stream;
	void          *qstream; // nni_pipe
	bool           closed;
	bool           busy;
	int 	       next_packet_id; // next packet id to use
	mqtt_sock_t   *mqtt_sock;
	nni_id_map     sent_unack;    // send messages unacknowledged
	nni_id_map     recv_unack;    // recv messages unacknowledged
	nni_aio        send_aio;      // send aio to the underlying transport
	nni_aio        recv_aio;      // recv aio to the underlying transport
	nni_aio        time_aio;      // timer aio to resend unack msg
	nni_lmq        recv_messages; // recv messages queue
	nni_lmq        ctx_aios;      // awaiting aio of QoS
};

/******************************************************************************
 *                              Sock Implementation                           *
 ******************************************************************************/

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

// Should be called with mutex lock hold after pipe is secured
// return true when aio should be finished
static inline bool
mqtt_send_msg(nni_aio *aio, nni_msg *msg, mqtt_sock_t *s)
{
	mqtt_pipe_t *p   = s->pipe;
	nni_msg *    *tmsg;
	uint16_t     ptype, packet_id;
	uint8_t      qos = 0;
	bool 	     rv = false;

	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
	case NNG_MQTT_PINGREQ:
		break;

	case NNG_MQTT_PUBLISH:
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (0 == qos) {
			break; // QoS 0 need no packet id
		}
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id     = mqtt_pipe_get_next_packet_id(p);
		nni_mqtt_msg_set_packet_id(msg, packet_id);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_qos_db_get_client_msg(
		    p->sent_unack, nni_pipe_id(p->pipe), packet_id);
		if (tmsg != NULL) {
			nni_plat_printf("Warning : msg %d lost due to "
			                "packetID duplicated!",
			    packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio) {
				nni_aio_finish_error(m_aio, NNG_EPROTO);
			}
			nni_msg_free(tmsg);
			nni_qos_db_remove_client_msg(
			    p->sent_unack, nni_pipe_id(p->pipe), packet_id);
		}
		nni_msg_clone(msg);
		if (nni_qos_db_set_client_msg(p->sent_unack,
		        nni_pipe_id(p->pipe), packet_id, msg) != 0) {
			nni_println("Warning! Cache QoS msg failed");
			nni_msg_free(msg);
			//we finished here since there is no second time
			nni_aio_finish_error(aio, MQTT_ERR_NOT_FOUND);
		}
		break;
	default:
		nni_aio_finish_error(aio, NNG_EPROTO);
		return false;
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
			nni_println("Warning! msg lost due to busy socket");
		}
	}
	if (0 == qos && ptype != NNG_MQTT_SUBSCRIBE &&
	    ptype != NNG_MQTT_UNSUBSCRIBE) {
		return true;
	}
	return false;
}
static void
mqtt_quic_send_cb(void *arg)
{
	mqtt_pipe_t *p   = arg;
	mqtt_sock_t *s   = p->mqtt_sock;
	nni_msg *    msg = NULL;
	nni_aio * aio;

	nni_plat_printf("Quic send callback\n");

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
		mqtt_send_msg(aio, msg, s);
		nni_mtx_unlock(&s->mtx);
		return;
	}
	// Check cached msg in lmq later
	if (nni_lmq_get(&s->send_messages, &msg) == 0) {
		p->busy = true;
		nni_mqtt_msg_encode(msg);
		nni_aio_set_msg(&p->send_aio, msg);
		quic_strm_send(p->qstream, &p->send_aio);
	}
	p->busy = false;
	nni_mtx_unlock(&s->mtx);

	if (s->cb.msg_send_cb)
		s->cb.msg_send_cb(NULL);

	return;
}

static void
mqtt_quic_recv_cb(void *arg)
{
	nni_plat_printf("Quic recv callback\n");
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
		quic_strm_recv(p->qstream, &p->recv_aio);
		nni_mtx_unlock(&s->mtx);
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
		cached_msg = nni_qos_db_get_client_msg(
		    p->sent_unack, nni_pipe_id(p->pipe), packet_id);
		if (cached_msg != NULL) {
			nni_qos_db_remove_client_msg(
			    p->sent_unack, nni_pipe_id(p->pipe), packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			nni_msg_free(cached_msg);
		}
		nni_msg_free(msg);
		break;
	case NNG_MQTT_PUBLISH:
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (2 > qos) {
			// QoS 0, successful receipt
			// QoS 1, the transport handled sending a PUBACK
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
			// TODO check if this packetid already there
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
		}
		break;

	default:
		// unexpected packet type, server misbehaviour
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
			s->cb.connect_cb(msg);
	if (packet_type == NNG_MQTT_PUBLISH)
		if (s->cb.msg_recv_cb) // Trigger cb
			s->cb.msg_recv_cb(msg);
}

// Timer callback, we use it for retransmitting.
static void
mqtt_timer_cb(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static void
mqtt_quic_sock_fini(void *arg)
{
	mqtt_sock_t *s = arg;
	nni_plat_printf("sock finit\n");
	nni_lmq_fini(&s->send_messages);
}

static void
mqtt_quic_sock_send(void *arg, nni_aio *aio)
{
	// do not support context for now.
	mqtt_sock_t *s   = arg;
	mqtt_pipe_t *p   = s->pipe;
	nni_msg *    msg, *tmsg;

	if (nni_aio_begin(aio) != 0) {
		return;
	}

	nni_mtx_lock(&s->mtx);

	if (s->closed) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	msg   = nni_aio_get_msg(aio);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		nni_aio_finish_error(aio, NNG_EPROTO);
		return;
	}
	if (p == NULL) {
		nni_plat_printf("connection lost! caching aio \n");
		if (!nni_list_active(&s->send_queue, aio)) {
			// cache aio
			nni_list_append(&s->send_queue, aio);
			nni_mtx_unlock(&s->mtx);
		} else {
			nni_msg_free(msg);
			nni_mtx_unlock(&s->mtx);
			nni_aio_set_msg(aio, NULL);
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
		return;
	}
	if (mqtt_send_msg(aio, msg, s)) {
		nni_mtx_unlock(&s->mtx);
		// nni_aio_set_msg(aio, NULL);
		nni_aio_finish(aio, 0, 0);
		return;
	}
	nni_mtx_unlock(&s->mtx);
	nni_aio_set_msg(aio, NULL);
	return;
}

static void
mqtt_quic_sock_recv(void *arg, nni_aio *aio)
{
	mqtt_sock_t *s   = arg;
	mqtt_pipe_t *p   = s->pipe;
	nni_msg     *msg = NULL;

	nni_plat_printf("sock send!\n");
	if (nni_aio_begin(aio) != 0) {
		return;
	}

	nni_mtx_lock(&s->mtx);
	if ( p == NULL ) {
		goto wait;
	} 

	if (nni_atomic_get_bool(&s->closed) || nni_atomic_get_bool(&p->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	/**/
	if (nni_lmq_get(&p->recv_messages, &msg) == 0) {
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&s->mtx);
		//let user gets a quick reply
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}
	// no open pipe or msg wating
wait:
	nni_plat_printf("connection lost! caching aio \n");
	if (!nni_list_active(&s->recv_queue, aio)) {
		// cache aio
		nni_list_append(&s->recv_queue, aio);
		nni_mtx_unlock(&s->mtx);
	} else {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		// nni_println("ERROR! former aio not finished!");
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	return;
}

static void mqtt_quic_sock_init(void *arg, nni_sock *sock)
{
	NNI_ARG_UNUSED(arg);
	NNI_ARG_UNUSED(sock);
	mqtt_sock_t *s = arg;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);

	// this is "semi random" start for request IDs.
	s->retry = NNI_SECOND * 60;

	nni_mtx_init(&s->mtx);
	// mqtt_ctx_init(&s->master, s);

#ifdef NNG_SUPP_SQLITE
	nni_qos_db_init_sqlite(s->sqlite_db, DB_NAME, false);
	nni_qos_db_reset_client_msg_pipe_id(s->sqlite_db);
#endif
	nni_lmq_init(&s->send_messages, NNG_MAX_SEND_LMQ);
	nni_aio_list_init(&s->send_queue);
	nni_aio_list_init(&s->recv_queue);

	s->pipe = NULL;

	s->cb.connect_cb = NULL;
	s->cb.msg_recv_cb = NULL;
	s->cb.msg_send_cb = NULL;
}

/* Stream EQ Pipe */

static int
quic_mqtt_stream_init(void *arg, void *qstrm, void *sock)
{
	nni_plat_printf("quic_mqtt_stream_init.\n");
	mqtt_pipe_t *p = arg;
	p->qstream = qstrm;
	p->mqtt_sock = sock;
	p->mqtt_sock->pipe = p;

	p->closed = false;
	p->busy   = false;
	p->next_packet_id = 0;
	// p->mqtt_sock = s;
	nni_aio_init(&p->send_aio, mqtt_quic_send_cb, p);
	nni_aio_init(&p->recv_aio, mqtt_quic_recv_cb, p);
	nni_aio_init(&p->time_aio, mqtt_timer_cb, p);
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
	nni_aio_fini(&p->time_aio);
	nni_id_map_fini(&p->sent_unack);
	nni_id_map_fini(&p->recv_unack);
	nni_lmq_fini(&p->recv_messages);
}

static void
quic_mqtt_stream_start(void *arg)
{
	nni_plat_printf("quic_mqtt_stream_start.\n");
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_aio   *aio;
	nni_msg *msg;

	nni_mtx_lock(&s->mtx);
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		if (mqtt_send_msg(aio, msg, s)) {
			nni_aio_finish(aio, 0, 0);
		}
	}
	nni_mtx_unlock(&s->mtx);
	//initiate the global resend timer
	// nni_sleep_aio(s->retry, &p->time_aio);
	quic_strm_recv(p->qstream, &p->recv_aio);

	return;
}

static void
quic_mqtt_stream_stop(void *arg)
{
	nni_plat_printf("quic_mqtt_stream_stop.\n");

}

static void
quic_mqtt_stream_close(void *arg)
{
	nni_plat_printf("quic_mqtt_stream_close.\n");
}

static void
mqtt_quic_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
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
nng_mqtt_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *))
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.connect_cb = cb;
	} else {
		return -1;
	}
	return 0;
}

int
nng_mqtt_quic_set_msg_recv_cb(nng_socket *sock, int (*cb)(void *))
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.msg_recv_cb = cb;
	} else {
		return -1;
	}
	return 0;
}

int
nng_mqtt_quic_set_msg_send_cb(nng_socket *sock, int (*cb)(void *))
{
	nni_sock *nsock = NULL;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock_t *mqtt_sock = nni_sock_proto_data(nsock);
		mqtt_sock->cb.msg_send_cb = cb;
	} else {
		return -1;
	}
	return 0;
}

