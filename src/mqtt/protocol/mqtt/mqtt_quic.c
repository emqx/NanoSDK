//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <string.h>

#include "nng/mqtt/mqtt_quic.h"
#include "sqlite_handler.h"
#include "core/nng_impl.h"
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

static int mqtt_sub_stream(mqtt_pipe_t *p, nni_msg *msg, uint16_t packet_id, nni_aio *aio);

#if defined(NNG_SUPP_SQLITE)
static void *mqtt_quic_sock_get_sqlite_option(mqtt_sock_t *s);
#endif

#define MQTT_PROTOCOL_DEBUG 0

#if MQTT_PROTOCOL_DEBUG
#define log_info(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_warn(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_debug(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)

#define log_error(fmt, ...)                                                 \
	do {                                                            \
		printf("[%s]: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
	} while (0)
#else
#define qdebug(fmt, ...) do {} while(0)
#define log_debug(fmt, ...) do {} while(0)
#define log_info(fmt, ...) do {} while(0)
#define log_warn(fmt, ...) do {} while(0)
#define log_error(fmt, ...) do {} while(0)
#endif

//default QUIC config for define QUIC transport
static conf_quic config_default = {
	.tls = {
		.enable = false,
		.url    = "", // Depracated
		.cafile = "", // Both of CS ends
		.certfile = "", // For client end
		.keyfile  = "", // For client end
		.ca       = "", // For server end
		.cert     = "", // For server end
		.key      = "", // For server end
		.key_password = "", // for client end
		.verify_peer = true,
		.set_fail = true,
	},
	.multi_stream = false,
	.qos_first  = false,
	.qkeepalive = 30,
	.qconnect_timeout = 60,
	.qdiscon_timeout = 30,
	.qidle_timeout = 30,
	.qcongestion_control = 0, // cubic
};

static uint32_t
DJBHashn(char *str, uint16_t len)
{
	unsigned int hash = 5381;
	uint16_t     i    = 0;
	while (i < len) {
		hash = ((hash << 5) + hash) + (*str++); /* times 33 */
		i++;
	}
	hash &= ~(1U << 31); /* strip the highest bit */
	return hash;
}

static int nng_mqtt_quic_set_config(nng_socket *sock, void *node);

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
	nni_atomic_bool closed;
	nni_atomic_int  next_packet_id; // next packet id to use, shared by multiple pipes
	uint16_t      counter;   // counter for elapsed time
	uint16_t      pingcnt;   // count how many ping msg is lost
	uint16_t      keepalive; // MQTT keepalive
	nni_duration  retry;
	nni_mtx       mtx;        // more fine grained mutual exclusion
	mqtt_quic_ctx master;     // to which we delegate send/recv calls
	nni_list      recv_queue; // aio pending to receive
	nni_list      send_queue; // aio pending to send

	void    *qsock;         // The matrix of quic sock. Which only be allow to use when disconnect.
	                        // Or lock first.
	nni_lmq  send_messages; // send messages queue (only for major stream)
	nni_lmq *ack_lmq;
	nni_id_map  *streams; // pipes, only effective in multi-stream mode
	mqtt_pipe_t *pipe;    // the major pipe (control stream)
	                      // main quic pipe, others needs a map to store the
	                      // relationship between MQTT topics and quic pipes
	nni_aio   time_aio; // timer aio to resend unack msg
	nni_aio  *ack_aio;  // set by user, expose puback/pubcomp
	nni_msg  *ping_msg, *connmsg;
	nni_sock *nsock;

	nni_mqtt_sqlite_option *sqlite_opt;

	struct mqtt_client_cb cb; // user cb
};

// A mqtt_pipe_s is our per-stream protocol private structure.
// equal to self-defined pipe in other protocols
struct mqtt_pipe_s {
	nni_mtx         lk;
	void           *qconnection;
	void           *qsock; // quic socket for MSQUIC/etc transport usage
	void           *qpipe; // each pipe has their own QUIC stream
	nni_atomic_bool closed;
	bool            busy;
	bool            ready;			// mark if QUIC stream is ready
	mqtt_sock_t    *mqtt_sock;
	nni_id_map      sent_unack;    // unacknowledged sent     messages
	nni_id_map      recv_unack;    // unacknowledged received messages
	nni_aio         send_aio;      // send aio to the underlying transport
	nni_aio         recv_aio;      // recv aio to the underlying transport
	nni_aio         rep_aio;       // aio for resending qos msg and PINGREQ
	nni_lmq 		send_inflight; // only used in multi-stream mode
	nni_lmq         recv_messages; // recv messages queue
	uint32_t        stream_id;	   // only for multi-stream
	uint16_t        rid;           // index of resending packet id
	uint8_t         reason_code;   // MQTTV5 reason code
};

// Multi-stream API
/**
 * Create independent & seperated stream for specific topic.
 * Only effective on Publish
 * This stream is unidirecitional by default
*/
static mqtt_pipe_t*
nng_mqtt_quic_open_topic_stream(mqtt_sock_t *mqtt_sock, const char *topic, uint32_t len)
{
	mqtt_pipe_t *p          = mqtt_sock->pipe;
	mqtt_pipe_t *new_pipe   = NULL;
	uint32_t     hash;

	// create a pipe/stream here
	if ((new_pipe = nng_alloc(sizeof(mqtt_pipe_t))) == NULL) {
		log_error("error in alloc pipe.\n");
		return NULL;
	}
	if (0 != quic_mqtt_stream_init(new_pipe, p->qsock, mqtt_sock)) {
		log_warn("Failed in open the topic-stream pair.");
		return NULL;
	}
	hash = DJBHashn((char *) topic, len);
	nni_id_set(mqtt_sock->streams, hash, new_pipe);
	new_pipe->stream_id = hash;
	log_debug("create new pipe %p for topic %.*s", new_pipe, len, topic);

	new_pipe->ready = true;
	nni_atomic_set_bool(&new_pipe->closed, false);
	// there is no aio in send_queue, because this is a newly established stream
	// for now, pub stream is also bidirectional
	quic_pipe_recv(new_pipe->qpipe, &new_pipe->recv_aio);
	return new_pipe;
}

/***
 * create a unidirectional stream and pub msg to it.
 * mapping sub topics (if >1) with the new stream.
*/
// static int
// mqtt_pub_stream(mqtt_pipe_t *p, nni_msg *msg, uint16_t packet_id, nni_aio *aio)

/***
 * create a unidirectional stream and send SUB/UNSUB packet
 * receving msg only from a unique topic
 * mapping sub topics (if >1) with the new stream.
*/
static int
mqtt_sub_stream(mqtt_pipe_t *p, nni_msg *msg, uint16_t packet_id, nni_aio *aio)
{
	uint32_t count, hash;
	nni_msg *tmsg;
	mqtt_sock_t *sock = p->mqtt_sock;
	mqtt_pipe_t *new_pipe   = NULL;
	nni_mqtt_topic_qos *topics;

	// check topic/stream pair exsitence
	topics = nni_mqtt_msg_get_subscribe_topics(msg, &count);
	// there is only one topic in Sub msg if multi-stream is enabled
	for (uint32_t i = 0; i < count; i++) {
		hash = DJBHashn(
		    (char *) topics[i].topic.buf, topics[i].topic.length);
		if ((new_pipe = nni_id_get(sock->streams, hash)) == NULL) {
			// create pipe here & set stream id
			log_debug("topic %s qos %d", topics[i].topic.buf, topics[i].qos);
			// create a pipe/stream here
			if ((new_pipe = nng_alloc(sizeof(mqtt_pipe_t))) == NULL) {
				log_error("error in alloc pipe.\n");
				return -1;
			}
			if (0 != quic_mqtt_stream_init(
			        new_pipe, p->qsock, p->mqtt_sock)) {
				log_warn(
				    "Failed in open the topic-stream pair.");
				return -1;
			}
			nni_id_set(sock->streams, hash, new_pipe);
			new_pipe->stream_id = hash;

			log_debug("create new pipe %p for topic %s", new_pipe,
			    (char *) topics[0].topic.buf);
			new_pipe->ready = true;
			nni_atomic_set_bool(&new_pipe->closed, false);
			// there is no aio in send_queue, because this is a
			// newly established stream
			quic_pipe_recv(new_pipe->qpipe, &new_pipe->recv_aio);
		} else {
			log_info("topic-stream already existed");
		}
	}

	nni_mqtt_msg_set_packet_id(msg, packet_id);
	nni_mqtt_msg_set_aio(msg, aio);
	tmsg = nni_id_get(&new_pipe->sent_unack, packet_id);
	if (tmsg != NULL) {
		log_warn("Warning : msg %d lost due to "
		         "packetID duplicated!",
		    packet_id);
		nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
		if (m_aio && nni_mqtt_msg_get_packet_type(tmsg) != NNG_MQTT_PUBLISH) {
			nni_aio_finish_error(m_aio, UNSPECIFIED_ERROR);
		}
		nni_msg_free(tmsg);
		nni_id_remove(&new_pipe->sent_unack, packet_id);
	}
	nni_msg_clone(msg);
	if (0 != nni_id_set(&new_pipe->sent_unack, packet_id, msg)) {
		nni_println("Warning! Cache QoS msg failed");
		nni_msg_free(msg);
		nni_aio_finish_error(aio, UNSPECIFIED_ERROR);
	}

	if (!new_pipe->busy) {
		nni_aio_set_msg(&new_pipe->send_aio, msg);
		p->busy = true;
		quic_pipe_send(new_pipe->qpipe, &new_pipe->send_aio);
	} else {
		if (nni_lmq_full(&new_pipe->send_inflight)) {
			(void) nni_lmq_get(&new_pipe->send_inflight, &tmsg);
			log_warn("msg lost due to flight window is full");
			nni_msg_free(tmsg);
		}
		if (0 != nni_lmq_put(&new_pipe->send_inflight, msg)) {
			nni_println(
			    "Warning! msg send failed due to busy socket");
		}
	}
	return 0;
}
// end of Multi-stream API

static uint16_t
mqtt_pipe_get_next_packet_id(mqtt_sock_t *s)
{
	int packet_id;
	do {
		packet_id = nni_atomic_get(&s->next_packet_id);
	} while (
	    !nni_atomic_cas(&s->next_packet_id, packet_id, packet_id + 1));
	return packet_id & 0xFFFF;
}



// Should be called with mutex lock hold after pipe is secured
// return rv>0 when aio should be finished (error or successed)
static inline int
mqtt_send_msg(nni_aio *aio, nni_msg *msg, mqtt_sock_t *s)
{
	mqtt_pipe_t *p   = s->pipe;
	nni_msg     *tmsg;
	uint16_t     ptype, packet_id;
	uint32_t     topic_len = 0;
	uint8_t      qos = 0;

	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
		// Free old connect msg if user set a new one
		if (s->connmsg != msg && s->connmsg != NULL) {
			nni_msg_free(s->connmsg);
		}
		// Only send CONNECT once for each pipe otherwise memleak
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
		if (s->multi_stream) {
			// check if topic-stream pair exist
			mqtt_pipe_t *pub_pipe;

			char *topic = (char *) nni_mqtt_msg_get_publish_topic(
			    msg, &topic_len);
			pub_pipe =
			    nni_id_get(s->streams, DJBHashn(topic, topic_len));
			if (pub_pipe == NULL) {
				pub_pipe = nng_mqtt_quic_open_topic_stream(
				    s, topic, topic_len);
			}
			p = pub_pipe;
		}

		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (qos == 0) {
			break; // QoS 0 need no packet id
		}
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id = nni_mqtt_msg_get_packet_id(msg);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_id_get(&p->sent_unack, packet_id);
		if (tmsg != NULL) {
			log_warn("Warning : msg %d lost due to "
			                "packetID duplicated!",
			    packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio && nni_mqtt_msg_get_packet_type(tmsg)
			        != NNG_MQTT_PUBLISH) {
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
	if (s->qos_first)
		if (qos > 0 && ptype == NNG_MQTT_PUBLISH) {
			nni_aio_set_msg(aio, msg);
			quic_aio_send(p->qpipe, aio);
			log_debug("sending highpriority QoS msg in parallel");
			return -1;
		}
	if (!p->busy) {
		nni_aio_set_msg(&p->send_aio, msg);
		p->busy = true;
		quic_pipe_send(p->qpipe, &p->send_aio);
	} else {
		if (nni_lmq_full(&s->send_messages)) {
			(void) nni_lmq_get(&s->send_messages, &tmsg);
			log_warn("msg lost due to flight window is full");
			nni_msg_free(tmsg);
		}
		if (0 != nni_lmq_put(&s->send_messages, msg)) {
			log_warn(
			    "Warning! msg send failed due to busy socket");
			nni_msg_free(msg);
		}
	}
	if (ptype != NNG_MQTT_SUBSCRIBE &&
	    ptype != NNG_MQTT_UNSUBSCRIBE) {
		return 0;
	}
	return -1;
}

// send msg with specific pipe/stream for only Pub
static inline int
mqtt_pipe_send_msg(nni_aio *aio, nni_msg *msg, mqtt_pipe_t *p, uint16_t packet_id)
{
	nni_msg     *tmsg;
	uint16_t     ptype;
	uint8_t      qos = 0;

	ptype = nni_mqtt_msg_get_packet_type(msg);
	switch (ptype) {
	case NNG_MQTT_CONNECT:
		nni_println("Error: wrong type of msg is being sent via data stream!");
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
		nni_mqtt_msg_set_packet_id(msg, packet_id);
		nni_mqtt_msg_set_aio(msg, aio);
		tmsg = nni_id_get(&p->sent_unack, packet_id);
		if (tmsg != NULL) {
			log_warn("Warning : msg %d lost due to "
			                "packetID duplicated!",
			    packet_id);
			nni_aio *m_aio = nni_mqtt_msg_get_aio(tmsg);
			if (m_aio) {
				if (nni_mqtt_msg_get_packet_type(tmsg) == NNG_MQTT_SUBSCRIBE ||
				    nni_mqtt_msg_get_packet_type(tmsg) == NNG_MQTT_UNSUBSCRIBE) {
					nni_aio_finish_error(m_aio, UNSPECIFIED_ERROR);
				}
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
		nni_aio_set_msg(&p->send_aio, msg);
		p->busy = true;
		quic_pipe_send(p->qpipe, &p->send_aio);
	} else {
		if (nni_lmq_full(&p->send_inflight)) {
			(void) nni_lmq_get(&p->send_inflight, &tmsg);
			log_warn("msg lost due to flight window is full");
			nni_msg_free(tmsg);
		}
		if (0 != nni_lmq_put(&p->send_inflight, msg)) {
			nni_println(
			    "Warning! msg send failed due to busy socket");
		}
	}
	if (ptype == NNG_MQTT_PUBLISH ) {
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
		// We failed to send... clean up and deal with it.
		p->busy = false;
		// nni_msg_free(nni_aio_get_msg(&p->send_aio));
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
	s->counter = 0;
	// Check cached aio first in s->send_queue? or p->sendqueue?
	// Check cached msg in lmq
	// this msg is already proessed by mqtt_send_msg
	if (nni_lmq_get(&p->send_inflight, &msg) == 0) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		quic_pipe_send(p->qpipe, &p->send_aio);
		nni_mtx_unlock(&p->lk);
		// TODO set cb in pipe not socket?
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
	int          rv;

	if (nni_aio_result(&p->send_aio) != 0) {
		// We failed to send... clean up and deal with it.
		log_warn("fail to send on aio");
		// msg is already be freed in QUIC transport
		nni_aio_set_msg(&p->send_aio, NULL);
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
	s->counter = 0;
	// Check cached aio first
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg = nni_aio_get_msg(aio);
		int rv = 0;
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
	// Check cached msg in lmq later
	// this msg is already proessed by mqtt_send_msg
	if (nni_lmq_get(&s->send_messages, &msg) == 0) {
		p->busy = true;
		nni_aio_set_msg(&p->send_aio, msg);
		quic_pipe_send(p->qpipe, &p->send_aio);
		nni_mtx_unlock(&s->mtx);
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
			quic_pipe_send(p->qpipe, &p->send_aio);
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

// only publish & suback/unsuback packet is valid
static void
mqtt_quic_data_strm_recv_cb(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_aio * user_aio = NULL;
	nni_msg * cached_msg = NULL;
	nni_aio *aio;

	if (nni_aio_result(&p->recv_aio) != 0) {
		// stream is closed in transport layer
		return;
	}

	nni_mtx_lock(&p->lk);
	nni_msg *msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (msg == NULL) {
		nni_mtx_unlock(&p->lk);
		quic_pipe_recv(p->qpipe, &p->recv_aio);
		return;
	}
	if (nni_atomic_get_bool(&p->closed)) {
		//free msg and dont return data when pipe is closed.
		nni_mtx_unlock(&p->lk);
		if (msg) {
			nni_msg_free(msg);
		}
		return;
	}
	nni_mqtt_msg_proto_data_alloc(msg);
	nni_mqtt_msg_decode(msg);

	packet_type_t packet_type = nni_mqtt_msg_get_packet_type(msg);

	int32_t       packet_id;
	uint8_t       qos;
	nni_msg      *ack;

	// schedule another receive
	quic_pipe_recv(p->qpipe, &p->recv_aio);
    s->counter = 0;

	// Restore pingcnt
	s->pingcnt = 0;
	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		nni_println("ERROR: CONNACK received in data stream!");
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
		p->rid     = packet_id;
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			// should we support sub/unsub cb here?
			if (packet_type == NNG_MQTT_SUBACK ||
			    packet_type == NNG_MQTT_UNSUBACK) {
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

		// return PUBCOMP
		nni_mqtt_msg_alloc(&ack, 0);
		nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBCOMP);
		nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
		nni_mqtt_msg_encode(ack);
		// ignore result of this send ?
		mqtt_pipe_send_msg(NULL, ack, p, mqtt_pipe_get_next_packet_id(p->mqtt_sock));
		// return msg to user
		nni_mtx_lock(&s->mtx);
		if ((aio = nni_list_first(&s->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(&p->recv_messages, cached_msg)) {
				nni_msg_free(cached_msg);
				cached_msg = NULL;
			}
			break;
		}
		nni_list_remove(&s->recv_queue, aio);
		nni_mtx_unlock(&s->mtx);
		user_aio  = aio;
		nni_aio_set_msg(user_aio, cached_msg);
		break;
	case NNG_MQTT_PUBLISH:
		// we have received a PUBLISH
		qos = nni_mqtt_msg_get_publish_qos(msg);
		if (2 > qos) {
			if (qos == 1) {
				// QoS 1 return PUBACK
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
				mqtt_pipe_send_msg(NULL, ack, p, mqtt_pipe_get_next_packet_id(p->mqtt_sock));
			}
			nni_mtx_lock(&s->mtx);
			// TODO aio should be placed in p->recv_queue to achieve parallel
			if ((aio = nni_list_first(&s->recv_queue)) == NULL) {
				if (0 != mqtt_pipe_recv_msgq_putq(&p->recv_messages, msg)) {
					nni_msg_free(msg);
					msg = NULL;
				}
				// nni_println("ERROR: no ctx found!! create
				// more ctxs!");
				break;
			}
			nni_list_remove(&s->recv_queue, aio);
			nni_mtx_unlock(&s->mtx);
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
				log_error(
				    "ERROR: packet id %d duplicates in",
				    packet_id);
				nni_msg_free(cached_msg);
				// nni_id_remove(&pipe->nano_qos_db,
				// pid);
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
			// return PUBREC
			nni_mqtt_msg_alloc(&ack, 0);
			nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBREC);
			nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
			nni_mqtt_msg_encode(ack);
			mqtt_pipe_send_msg(NULL, ack, p, 0);
		}
		break;
	case NNG_MQTT_PINGRESP:
		// PINGRESP is ignored in protocol layer
		// Rely on health checker of Quic stream
		// free msg
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;
	case NNG_MQTT_PUBREC:
		// return PUBREL
		packet_id = nni_mqtt_msg_get_pubrec_packet_id(msg);
		nni_mqtt_msg_alloc(&ack, 0);
		nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBREL);
		nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
		nni_mqtt_msg_encode(ack);
		// ignore result of this send ?
		mqtt_pipe_send_msg(NULL, ack, p, 0);
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		return;
	default:
		// unexpected packet type, server misbehaviour
		nni_msg_free(msg);
		nni_mtx_unlock(&p->lk);
		// close quic stream
		// quic_pipe_close
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
	nni_aio *aio;

	if (nni_aio_result(&p->recv_aio) != 0) {
		// stream is closed in transport layer
		return;
	}

	nni_mtx_lock(&s->mtx);
	nni_msg *msg = nni_aio_get_msg(&p->recv_aio);
	nni_aio_set_msg(&p->recv_aio, NULL);
	if (msg == NULL) {
		nni_mtx_unlock(&s->mtx);
		quic_pipe_recv(p->qpipe, &p->recv_aio);
		return;
	}
	if (nni_atomic_get_bool(&s->closed) ||
	    nni_atomic_get_bool(&p->closed)) {
		//free msg and dont return data when pipe is closed.
		nni_mtx_unlock(&s->mtx);
		if (msg) {
			nni_msg_free(msg);
		}
		return;
	}
	// nni_msg_set_pipe(msg, nni_pipe_id(p->pipe));
	nni_mqtt_msg_proto_data_alloc(msg);
	nni_mqtt_msg_decode(msg);

	packet_type_t packet_type = nni_mqtt_msg_get_packet_type(msg);

	int32_t       packet_id;
	uint8_t       qos;
	nni_msg      *ack;

	// schedule another receive
	quic_pipe_recv(p->qpipe, &p->recv_aio);
	s->counter = 0;

	// Restore pingcnt
	s->pingcnt = 0;
	switch (packet_type) {
	case NNG_MQTT_CONNACK:
		// Clone CONNACK for connect_cb & aio_cb
		nni_msg_clone(msg);
		if (s->ack_aio != NULL && !nni_aio_busy(s->ack_aio)) {
			nni_msg_clone(msg);
			nni_aio_finish_msg(s->ack_aio, msg);
		}
		if ((aio = nni_list_first(&s->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(&p->recv_messages, msg)) {
				nni_msg_free(msg);
				msg = NULL;
			}
			break;
		}
		nni_list_remove(&s->recv_queue, aio);
		user_aio = aio;
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
		p->rid     = packet_id;
		cached_msg = nni_id_get(&p->sent_unack, packet_id);
		if (cached_msg != NULL) {
			nni_id_remove(&p->sent_unack, packet_id);
			user_aio = nni_mqtt_msg_get_aio(cached_msg);
			// should we support sub/unsub cb here?
			nni_msg_clone(msg);
			nni_aio_set_msg(user_aio, msg);
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

		// return PUBCOMP
		nni_mqtt_msg_alloc(&ack, 0);
		nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBCOMP);
		nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
		nni_mqtt_msg_encode(ack);
		// ignore result of this send ?
		mqtt_send_msg(NULL, ack, s);
		// return msg to user
		if ((aio = nni_list_first(&s->recv_queue)) == NULL) {
			// No one waiting to receive yet, putting msg
			// into lmq
			if (0 != mqtt_pipe_recv_msgq_putq(&p->recv_messages, cached_msg)) {
				nni_msg_free(cached_msg);
				cached_msg = NULL;
			}
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
				if (s->cb.msg_recv_cb)
					break;
				if (0 != mqtt_pipe_recv_msgq_putq(&p->recv_messages, msg)) {
					nni_msg_free(msg);
					msg = NULL;
				}
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
				log_error(
				    "ERROR: packet id %d duplicates in",
				    packet_id);
				nni_msg_free(cached_msg);
				// nni_id_remove(&pipe->nano_qos_db,
				// pid);
			}
			nni_id_set(&p->recv_unack, packet_id, msg);
			// return PUBREC
			nni_mqtt_msg_alloc(&ack, 0);
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
		// return PUBREL
		packet_id = nni_mqtt_msg_get_pubrec_packet_id(msg);
		nni_mqtt_msg_alloc(&ack, 0);
		nni_mqtt_msg_set_packet_type(ack, NNG_MQTT_PUBREL);
		nni_mqtt_msg_set_puback_packet_id(ack, packet_id);
		nni_mqtt_msg_encode(ack);
		// ignore result of this send ?
		mqtt_send_msg(NULL, ack, s);
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
		return;
	}
	nni_mtx_lock(&s->mtx);

	p = s->pipe;

	if (NULL == p || nni_atomic_get_bool(&p->closed)) {
		// QUIC connection has been shut down
		nni_mtx_unlock(&s->mtx);
		return;
	}

	s->counter += s->retry;
	if (nni_aio_busy(&p->rep_aio)) {
		log_warn("rep_aio busy! stream is in serious congestion");
		nni_aio_abort(&p->rep_aio, NNG_ECANCELED);
	}
	if (s->counter >= s->keepalive) {
		// send PINGREQ
		if (s->pingcnt > 1) {
			log_warn("Close the quic connection due to timeout");
			s->pingcnt = 0; // restore pingcnt
			p->reason_code = KEEP_ALIVE_TIMEOUT;
			quic_disconnect(p->qsock, p->qpipe);
			log_warn("connection shutting down");
			nni_mtx_unlock(&s->mtx);
			return;
		} else if (!nni_aio_busy(&p->rep_aio)){
			nni_aio_set_msg(&p->rep_aio, s->ping_msg);
			nni_msg_clone(s->ping_msg);
			quic_pipe_send(p->qpipe, &p->rep_aio);
			s->counter = 0;
			s->pingcnt ++;
			log_debug("send PINGREQ %d %d", s->counter, s->pingcnt);
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
	mqtt_sock_t *s = arg;
	s->nsock       = sock;

	nni_atomic_init_bool(&s->closed);
	nni_atomic_set_bool(&s->closed, false);

	// this is a pre-defined timer for global timer
	s->retry        = MQTT_QUIC_RETRTY;
	s->counter      = 0;
	s->pingcnt      = 0;
	s->connmsg      = NULL;
	s->sqlite_opt   = NULL;
	s->qos_first    = false;
	s->multi_stream = false;

	nni_mtx_init(&s->mtx);
	mqtt_quic_ctx_init(&s->master, s);
	nni_atomic_set(&s->next_packet_id, 1);

	s->streams = nng_alloc(sizeof(nni_id_map));
	nni_id_map_init(s->streams, 0x0000u, 0xffffu, true);

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
	nni_aio     *aio;
	nni_msg     *msg = NULL;
	size_t       count = 0;
	/*
#if defined(NNG_SUPP_SQLITE) && defined(NNG_HAVE_MQTT_BROKER)
	bool is_sqlite = get_persist(s);
	if (is_sqlite) {
		nni_qos_db_fini_sqlite(s->sqlite_db);
		nni_lmq_fini(&s->offline_cache);
	}
#endif
	*/
	log_debug("mqtt_quic_sock_fini %p", s);
	if (s->connmsg != NULL) {
		nni_msg_free(s->connmsg);
	}

	if (s->ack_aio != NULL) {
		nni_aio_fini(s->ack_aio);
		nng_free(s->ack_aio, sizeof(nni_aio *));
	}

	if (s->ack_lmq != NULL) {
		nni_lmq_fini(s->ack_lmq);
		nng_free(s->ack_lmq, sizeof(nni_lmq));
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
		nni_id_map_fini(s->streams);
		nng_free(s->streams, sizeof(nni_id_map));
	}
	mqtt_quic_ctx_fini(&s->master);
	nni_lmq_fini(&s->send_messages);
	nni_aio_fini(&s->time_aio);
	nni_msg_free(s->ping_msg);
	// potential memleak here. need to adapt to MsQUIC finit
	// quic_close();
}

static void
mqtt_quic_sock_open(void *arg)
{
	mqtt_sock_t *s = arg;
	uint8_t buf[2] = {0xC0,0x00};

	// alloc Ping msg
	nng_msg_alloc(&s->ping_msg, 0);
	nng_msg_header_append(s->ping_msg, buf, 1);
	nng_msg_append(s->ping_msg, buf+1, 1);
}

static void
mqtt_quic_pipe_close(void *key, void *val)
{
	NNI_ARG_UNUSED(key);
	if (val)
		quic_mqtt_stream_stop(val);
}

static void
mqtt_quic_sock_close(void *arg)
{
	nni_aio *aio;
	mqtt_sock_t *s = arg;
	mqtt_pipe_t *p = s->pipe;

	nni_mtx_lock(&s->mtx);
	nni_sock_hold(s->nsock);
	nni_aio_stop(&s->time_aio);
	nni_aio_close(&s->time_aio);
	nni_atomic_set_bool(&s->closed, true);

	while ((aio = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	// transport might win the contest to reconnect before this
	// if transport shutdown happens first.
	if (p != NULL) {
		// need to disconnect connection before sock fini
		quic_disconnect(p->qsock, p->qpipe);
	}
	quic_sock_close(s->qsock);

	if (s->multi_stream) {
		nni_id_map_foreach(s->streams, mqtt_quic_pipe_close);
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
// allocate main stream with pipe
static int
quic_mqtt_stream_init(void *arg, nni_pipe *qsock, void *sock)
{
	bool         major = false;
	mqtt_pipe_t *p     = arg;
	p->qsock           = qsock;
	p->mqtt_sock       = sock;

	if (p->mqtt_sock->pipe == NULL) {
		p->mqtt_sock->pipe = p;
		major = true;
	}
	p->rid = 1;
	p->reason_code = 0;
	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, true);
	p->busy  = false;
	p->ready = false;

	// QUIC stream init
	if (0 != quic_pipe_open(qsock, &p->qpipe, p)) {
		log_warn("Failed in open the main quic pipe.");
		return -1;
	}

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
	if (p->mqtt_sock->multi_stream)
		nni_lmq_init(&p->send_inflight, NNG_MAX_RECV_LMQ);
	nni_mtx_init(&p->lk);

	return (0);
}

static void
quic_mqtt_stream_fini(void *arg)
{
	nni_aio *aio;
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_msg * msg;

	log_warn("quic_mqtt_stream_fini! pipe finit!");
	if ((msg = nni_aio_get_msg(&p->recv_aio)) != NULL) {
		nni_aio_set_msg(&p->recv_aio, NULL);
		nni_msg_free(msg);
	}
	if ((msg = nni_aio_get_msg(&p->send_aio)) != NULL) {
		nni_aio_set_msg(&p->send_aio, NULL);
		nni_msg_free(msg);
	}
	// hold nni_sock twice for thread safety
	nni_sock_hold(s->nsock);
	nni_sock_hold(s->nsock);
	nni_mtx_lock(&s->mtx);
	nni_aio_fini(&p->send_aio);
	nni_aio_fini(&p->recv_aio);
	nni_aio_fini(&p->rep_aio);
	nni_aio_abort(&s->time_aio, 0);
	/*
#if defined(NNG_HAVE_MQTT_BROKER) && defined(NNG_SUPP_SQLITE)
	nni_id_map_fini(&p->sent_unack);
#endif
	*/
	nni_id_map_fini(&p->recv_unack);
	nni_id_map_fini(&p->sent_unack);
	if(s->multi_stream)
		nni_lmq_fini(&p->send_inflight);
	nni_lmq_fini(&p->recv_messages);
	nni_mtx_fini(&p->lk);

	// only report disconnect when major pipe is closed
	if (s->pipe == p && s->cb.disconnect_cb != NULL) {
		s->cb.disconnect_cb(NULL, s->cb.discarg);
	}

	uint16_t count = 0;
	// connect failed also triggered stream finit, ignore it

	p->reason_code == 0
	    ? p->reason_code = quic_sock_disconnect_code(p->qsock)
	    : p->reason_code;

	// emulate disconnect notify msg as a normal publish
	while ((aio = nni_list_first(&s->recv_queue)) != NULL) {
		// Pipe was closed.  just push an error back to the
		// entire socket, because we only have one pipe
		nni_list_remove(&s->recv_queue, aio);
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

	// Free the mqtt_pipe
	if (p == s->pipe) {
		// sock closed
		s->pipe = NULL;
	}

	// FIX: potential unsafe free
	nng_free(p, sizeof(p));
	nni_mtx_unlock(&s->mtx);
	nni_sock_rele(s->nsock);
	nni_sock_rele(s->nsock);
}

// only work for main stream
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

	if (!nni_aio_list_active(&s->time_aio))
		nni_sleep_aio(s->retry * NNI_SECOND, &s->time_aio);

	p->ready = true;
	nni_atomic_set_bool(&p->closed, false);
	if ((aio = nni_list_first(&s->send_queue)) != NULL) {
		nni_list_remove(&s->send_queue, aio);
		msg    = nni_aio_get_msg(aio);
		int rv = 0;
		if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
			nni_mtx_unlock(&s->mtx);
			nni_aio_finish(aio, rv, 0);
			quic_pipe_recv(p->qpipe, &p->recv_aio);
			return 0;
		}
	}

	nni_mtx_unlock(&s->mtx);
	quic_pipe_recv(p->qpipe, &p->recv_aio);
	return 0;
}

static void
quic_mqtt_stream_stop(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;
	nni_msg *msg;

	log_info("Stopping MQTT over QUIC Stream");
	if (!nni_atomic_get_bool(&p->closed))
		if (quic_pipe_close(p->qpipe, &p->reason_code) == 0) {
			nni_aio_stop(&p->send_aio);
			nni_aio_stop(&p->recv_aio);
			nni_aio_abort(&p->rep_aio, NNG_ECANCELED);
			nni_aio_finish_error(&p->rep_aio, NNG_ECANCELED);
			nni_aio_stop(&p->rep_aio);
			// nni_aio_stop(&s->time_aio);
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
		nni_id_map_foreach(&p->sent_unack, mqtt_close_unack_msg_cb);
		nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);
		p->qpipe = NULL;
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

		/*
	#if defined(NNG_HAVE_MQTT_BROKER) && defined(NNG_SUPP_SQLITE)
		nni_id_map_fini(&p->sent_unack);
	#endif
		*/
		nni_id_map_fini(&p->recv_unack);
		nni_id_map_fini(&p->sent_unack);
		if (s->multi_stream)
			nni_lmq_fini(&p->send_inflight);
		nni_lmq_fini(&p->recv_messages);
		nni_mtx_fini(&p->lk);

		// Free the mqtt_pipe
		// FIX: potential unsafe free
		nni_id_remove(s->streams, p->stream_id);
		nng_free(p, sizeof(p));

		nni_mtx_unlock(&s->mtx);
	}
}
// main stream close
static void
quic_mqtt_stream_close(void *arg)
{
	mqtt_pipe_t *p = arg;
	mqtt_sock_t *s = p->mqtt_sock;

	nni_atomic_set_bool(&p->closed, true);
	nni_mtx_lock(&s->mtx);
	nni_atomic_set_bool(&s->pipe->closed, true);
	nni_aio_close(&p->send_aio);
	nni_aio_close(&p->recv_aio);
	nni_aio_close(&p->rep_aio);
#if defined(NNG_SUPP_SQLITE)
	if (!nni_lmq_empty(&s->send_messages)) {
		sqlite_flush_lmq(
		    mqtt_quic_sock_get_sqlite_option(s), &s->send_messages);
	}
#endif
	nni_lmq_flush(&p->recv_messages);
	if(s->multi_stream)
		nni_lmq_flush(&p->send_inflight);
	nni_id_map_foreach(&p->sent_unack, mqtt_close_unack_msg_cb);
	nni_id_map_foreach(&p->recv_unack, mqtt_close_unack_msg_cb);
	p->qpipe = NULL;
	p->ready = false;
	nni_mtx_unlock(&s->mtx);
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
	mqtt_pipe_t   *p;
	nni_msg       *msg;
	uint16_t       packet_id = 0;
	uint8_t        qos;
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
	if (nni_atomic_get_bool(&s->closed)) {
		nni_mtx_unlock(&s->mtx);
		nni_msg_free(msg);
		nni_aio_finish_error(aio, NNG_ECLOSED);
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
	case NNG_MQTT_SUBSCRIBE:
	case NNG_MQTT_UNSUBSCRIBE:
		packet_id = mqtt_pipe_get_next_packet_id(s);
		nni_mqtt_msg_set_packet_id(msg, packet_id);
		break;
	default:
		break;
	}
	nni_mqtt_msg_encode(msg);

	if (p == NULL || p->ready == false) {
		// connection is lost or not established yet
#if defined(NNG_SUPP_SQLITE)
		if (nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH) {
			nni_mqtt_sqlite_option *sqlite =
			    mqtt_quic_sock_get_sqlite_option(s);
			if (sqlite_is_enabled(sqlite)) {
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
		}
#endif
		if (nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT &&
		    !nni_list_active(&s->send_queue, aio)) {
			// cache aio
			nni_list_append(&s->send_queue, aio);
			nni_mtx_unlock(&s->mtx);
		} else {
			// aio is already on the list.
			// caching pubmsg in lmq of sock and ignore the
			// result/ack of cb
			if (nni_mqtt_msg_get_packet_type(msg) ==
			    NNG_MQTT_PUBLISH) {
				nni_mqtt_msg_set_publish_qos(msg, 0);
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

	if (s->multi_stream &&
	    ptype == NNG_MQTT_SUBSCRIBE) {
		mqtt_sub_stream(p, msg, packet_id, aio);
	} else if ((rv = mqtt_send_msg(aio, msg, s)) >= 0) {
		nni_mtx_unlock(&s->mtx);
		nni_aio_finish(aio, rv, 0);
		return;
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

	if (nni_aio_begin(aio) != 0) {
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
		log_debug("recv action on closed socket!");
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
	if (nni_lmq_get(&p->recv_messages, &msg) == 0) {
		nni_aio_set_msg(aio, msg);
		nni_mtx_unlock(&s->mtx);
		//let user gets a quick reply
		nni_aio_finish(aio, 0, nni_msg_len(msg));
		return;
	}
	// no open pipe or msg wating
wait:
	// return error or caching aio?
	// nni_plat_printf("connection lost! caching aio \n");
	if (!nni_list_active(&s->recv_queue, aio)) {
		// cache aio
		nni_list_append(&s->recv_queue, aio);
		nni_mtx_unlock(&s->mtx);
	} else {
		nni_mtx_unlock(&s->mtx);
		nni_aio_set_msg(aio, NULL);
		nni_println("ERROR! former aio not finished!");
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
	    .o_name = NNG_OPT_MQTT_SQLITE,
	    .o_set  = mqtt_quic_sock_set_sqlite_option,
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

/**
 * As taking msquic as tranport, we exclude the dialer.
 * Open Quic client with default config parameters
*/
int
nng_mqtt_quic_client_open(nng_socket *sock, const char *url)
{
	return nng_mqtt_quic_client_open_conf(sock, url, &config_default);
}
/**
 * open mqtt quic transport with self-defined conf params
*/
int
nng_mqtt_quic_client_open_conf(nng_socket *sock, const char *url, conf_quic *conf)
{
	int       rv = 0;
	nni_sock *nsock = NULL;
	void     *qsock = NULL;

	mqtt_sock_t *msock = NULL;

	// Quic settings
	if ((rv = nni_proto_open(sock, &mqtt_msquic_proto)) == 0) {
		nni_sock_find(&nsock, sock->id);
		if (nsock) {
			// set client conf
			nng_mqtt_quic_set_config(sock, conf);
			quic_open();
			quic_proto_open(&mqtt_msquic_proto);
			quic_proto_set_sdk_config((void *)conf);
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

/*
 * It is an interface only for ffi.
 */
void conf_quic_tls_create(conf_quic **cqp, char *cafile, char *certfile,
    char *keyfile, char *key_pwd) {
	conf_quic *cq = nng_alloc(sizeof(conf_quic));

	cq->tls.enable = true;

	// Leak here. But on some ffi.
	// The input arguments would be lost when exit current scope.
	cq->tls.cafile = strdup(cafile);
	cq->tls.certfile = strdup(certfile);
	cq->tls.keyfile = strdup(keyfile);
	cq->tls.key_password = strdup(key_pwd);

	cq->tls.verify_peer = true;
	cq->multi_stream = false;
	cq->qos_first = false;
	cq->qkeepalive = 30;
	cq->qconnect_timeout = 60;
	cq->qdiscon_timeout = 30;
	cq->qidle_timeout = 30;

	*cqp = cq;
}

/**
 * init an AIO for Acknoledgement message only, in order to make QoS/connect truly asychrounous
 * For QoS 0 message, we do not care the result of sending
 * valid with Connack + puback + pubcomp
 * return 0 if set callback sucessfully
*/
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

static int
nng_mqtt_quic_set_config(nng_socket *sock, void *node)
{
	nni_sock         *nsock = NULL;
	conf_quic        *conf_node = node;
	mqtt_sock_t      *mqtt_sock;

	nni_sock_find(&nsock, sock->id);
	if (nsock) {
		mqtt_sock              = nni_sock_proto_data(nsock);
		if (node == NULL) {
			mqtt_sock->multi_stream = false;
			mqtt_sock->qos_first    = false;
		} else {
			mqtt_sock->multi_stream = conf_node->multi_stream;
			mqtt_sock->qos_first    = conf_node->qos_first;
			if (mqtt_sock->multi_stream) {
				mqtt_sock->streams =
				    nng_alloc(sizeof(nni_id_map));
				nni_id_map_init(mqtt_sock->streams, 0x0000u,
				    0xffffu, true);
			}
		}
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
