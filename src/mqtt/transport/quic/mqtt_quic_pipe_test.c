// Exercise the real protocol -> nni_pipe -> transport -> nng_stream boundary.
// The fake byte stream makes completion order and backpressure deterministic.
#include "nuts.h"
#include "mqtt_quic.c"
#include "../../protocol/mqtt/mqtt_quic_client.c"

typedef struct {
	nng_stream stream;
	nni_mtx mtx;
	nni_list sends;
	nni_list recvs;
	bool closed;
} test_stream;
typedef struct {
	test_stream stream;
	nni_pipe pipe;
	mqtt_quictran_pipe *tran;
	mqtt_sock_t sock;
	mqtt_pipe_t proto;
	bool protocol;
} fixture;

static unsigned channel(nni_aio *aio)
{
	int *flags = nni_aio_get_prov_data(aio);
	return flags ? *flags & QUIC_MULTISTREAM_FLAGS : 0;
}
static void fake_cancel(nni_aio *aio, void *arg, int rv)
{
	test_stream *s = arg;
	nni_mtx_lock(&s->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&s->mtx);
}
static void fake_io(test_stream *s, nni_aio *aio, bool send)
{
	if (nni_aio_begin(aio) != 0) return;
	nni_mtx_lock(&s->mtx);
	int rv = s->closed ? NNG_ECLOSED : nni_aio_schedule(aio, fake_cancel, s);
	if (rv) nni_aio_finish_error(aio, rv);
	else nni_aio_list_append(send ? &s->sends : &s->recvs, aio);
	nni_mtx_unlock(&s->mtx);
}
static void fake_send(void *arg, nni_aio *aio) { fake_io(arg, aio, true); }
static void fake_recv(void *arg, nni_aio *aio) { fake_io(arg, aio, false); }
static void fake_close(void *arg)
{
	test_stream *s = arg;
	nni_aio *aio;
	nni_mtx_lock(&s->mtx);
	s->closed = true;
	while ((aio = nni_list_first(&s->sends)) != NULL) {
		nni_aio_list_remove(aio); nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	while ((aio = nni_list_first(&s->recvs)) != NULL) {
		nni_aio_list_remove(aio); nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	nni_mtx_unlock(&s->mtx);
}
static int fake_get(void *arg, const char *name, void *buf, size_t *sz, nni_type t)
{
	(void)arg;
	if (strcmp(name, NNG_OPT_QUIC_ENABLE_MULTISTREAM) == 0)
		return nni_copyout_bool(true, buf, sz, t);
	return NNG_ENOTSUP;
}
static void fake_free(void *arg) { (void)arg; }
static nni_aio *find_io(nni_list *q, unsigned id)
{
	nni_aio *aio;
	NNI_LIST_FOREACH(q, aio) { if (channel(aio) == id) return aio; }
	return NULL;
}
static bool pending(fixture *f, unsigned id, bool send)
{
	nni_mtx_lock(&f->stream.mtx);
	bool found = find_io(send ? &f->stream.sends : &f->stream.recvs, id) != NULL;
	nni_mtx_unlock(&f->stream.mtx);
	return found;
}
static void await_io(fixture *f, unsigned id, bool send)
{
	for (unsigned i = 0; i < 1000 && !pending(f, id, send); i++) nni_msleep(1);
	NUTS_TRUE(pending(f, id, send));
}
static void complete(fixture *f, unsigned id, size_t limit, int rv)
{
	await_io(f, id, true);
	nni_mtx_lock(&f->stream.mtx);
	nni_aio *aio = find_io(&f->stream.sends, id);
	NUTS_TRUE(aio != NULL);
	if (aio) {
		size_t size = nni_aio_iov_count(aio);
		if (limit && size > limit) size = limit;
		nni_aio_list_remove(aio);
		nni_aio_finish(aio, rv, rv ? 0 : size);
	}
	nni_mtx_unlock(&f->stream.mtx);
}
static void feed(fixture *f, unsigned id, const uint8_t *data, size_t size)
{
	while (size) {
		await_io(f, id, false);
		nni_mtx_lock(&f->stream.mtx);
		nni_aio *aio = find_io(&f->stream.recvs, id);
		NUTS_TRUE(aio != NULL);
		if (!aio) { nni_mtx_unlock(&f->stream.mtx); return; }
		nni_iov *iov; unsigned niov;
		nni_aio_get_iov(aio, &niov, &iov);
		size_t copied = 0;
		for (unsigned i = 0; i < niov && size; i++) {
			size_t n = iov[i].iov_len < size ? iov[i].iov_len : size;
			memcpy(iov[i].iov_buf, data, n); data += n; size -= n; copied += n;
		}
		nni_aio_list_remove(aio);
		nni_aio_finish(aio, 0, copied);
		nni_mtx_unlock(&f->stream.mtx);
	}
}
static void setup_fixture(fixture *f, bool protocol, int version)
{
	memset(f, 0, sizeof(*f));
	NUTS_PASS(nni_init());
	f->stream.stream = (nng_stream) {
		.s_free = fake_free, .s_close = fake_close, .s_send = fake_send, .s_recv = fake_recv, .s_get = fake_get,
	};
	nni_mtx_init(&f->stream.mtx);
	nni_aio_list_init(&f->stream.sends); nni_aio_list_init(&f->stream.recvs);
	NUTS_PASS(mqtt_quictran_pipe_alloc(&f->tran));
	f->tran->conn = &f->stream.stream;
	f->tran->multi_stream = true; f->tran->proto = version;
	f->tran->rcvmax = NANO_MAX_PACKET_SIZE; f->tran->sndmax = 65535;
	f->pipe.p_tran_ops = mqtt_quictran_pipe_ops;
	f->pipe.p_tran_data = f->tran;
	// This fixture owns finalization; do not enqueue this stack pipe for reaping.
	nni_atomic_init_bool(&f->pipe.p_closed); nni_atomic_set_bool(&f->pipe.p_closed, true);
	NUTS_PASS(mqtt_quictran_pipe_init(f->tran, &f->pipe));
	for (unsigned i = 0; i <= QUIC_SUB_STREAM_NUM; i++) {
		quic_substream *sub = i ? &f->tran->substreams[i - 1] : NULL;
		nni_aio *aio = sub ? &sub->raio : f->tran->rxaio;
		nni_iov iov = { sub ? sub->rxlen : f->tran->rxlen, 2 };
		nni_aio_set_iov(aio, 1, &iov); nng_stream_recv(f->tran->conn, aio);
	}
	f->protocol = protocol;
	if (protocol) {
		mqtt_sock_t *s = &f->sock;
		nni_mtx_init(&s->mtx); nni_atomic_init_bool(&s->closed);
		nni_atomic_init(&s->next_packet_id); nni_atomic_set(&s->next_packet_id, 1);
		nni_id_map_init(&s->sent_unack, 1, 65535, false);
		nni_aio_list_init(&s->send_queue);
		NNI_LIST_INIT(&s->send_contexts, mqtt_quic_ctx, sqnode);
		NNI_LIST_INIT(&s->recv_queue, mqtt_quic_ctx, rqnode);
		nni_lmq_init(&s->legacy_acks, 16);
		s->mqtt_ver = version;
		mqtt_quic_ctx_init(&s->master, s);
		NUTS_PASS(mqtt_quic_pipe_init(&f->proto, &f->pipe, s));
		s->pipe = &f->proto; f->proto.ready = true;
		nni_atomic_set_bool(&f->proto.closed, false);
		mqtt_quic_recv_start(&f->proto);
	}
}
static void teardown_fixture(fixture *f)
{
	if (f->protocol) mqtt_quic_pipe_close(&f->proto);
	mqtt_quictran_pipe_close(f->tran);
	if (f->protocol) mqtt_quic_pipe_stop(&f->proto);
	mqtt_quictran_pipe_stop(f->tran);
	if (f->protocol) {
		mqtt_quic_pipe_fini(&f->proto);
		nni_id_map_fini(&f->sock.sent_unack); nni_lmq_fini(&f->sock.legacy_acks);
		nni_mtx_fini(&f->sock.mtx);
	}
	mqtt_quictran_pipe_fini(f->tran);
	nni_mtx_fini(&f->stream.mtx);
}
static nng_aio *send_msg(fixture *f, unsigned stream, unsigned qos, uint16_t pid)
{
	nng_msg *m; nng_aio *aio;
	NUTS_PASS(nng_mqtt_msg_alloc(&m, 0));
	nng_mqtt_msg_set_packet_type(m, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_topic(m, "pipe/test");
	nng_mqtt_msg_set_publish_payload(m, (uint8_t *)"data", 4);
	nng_mqtt_msg_set_publish_qos(m, qos); nni_mqtt_msg_set_publish_packet_id(m, pid);
	NUTS_PASS(nng_mqtt_quic_msg_set_stream(m, stream));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL)); nng_aio_set_timeout(aio, 2000);
	nng_aio_set_msg(aio, m);
	if (f->protocol) mqtt_quic_ctx_send(&f->sock.master, aio);
	else {
		NUTS_PASS(f->tran->proto == 5 ? nng_mqttv5_msg_encode(m) : nng_mqtt_msg_encode(m));
		nni_pipe_send(&f->pipe, aio);
	}
	return aio;
}
static void finish_user(nng_aio *aio, int rv)
{
	nng_aio_wait(aio); NUTS_TRUE(nng_aio_result(aio) == rv);
	nng_msg_free(nng_aio_get_msg(aio)); nng_aio_set_msg(aio, NULL); nng_aio_free(aio);
}
static void test_parallel(void)
{
	fixture f; setup_fixture(&f, true, 4);
	nng_aio *a = send_msg(&f, 3, 0, 0), *b = send_msg(&f, 3, 0, 0);
	nng_aio *c = send_msg(&f, 4, 0, 0);
	NUTS_TRUE(pending(&f, 3, true) && pending(&f, 4, true));
	complete(&f, 4, 1, 0); // force a short stream write
	complete(&f, 4, 0, 0); finish_user(c, 0);
	NUTS_TRUE(nni_aio_busy(a) && nni_aio_busy(b));
	complete(&f, 3, 0, 0); finish_user(a, 0);
	complete(&f, 3, 0, 0); finish_user(b, 0);
	teardown_fixture(&f);
}
static void test_cancel_and_close(void)
{
	fixture f; setup_fixture(&f, true, 4);
	nng_aio *a = send_msg(&f, 3, 0, 0), *b = send_msg(&f, 3, 0, 0);
	nng_aio *c = send_msg(&f, 4, 1, 0);
	nng_msg *original = nng_aio_get_msg(b);
	nng_aio_abort(b, NNG_ECANCELED); nng_aio_wait(b);
	NUTS_TRUE(nng_aio_get_msg(b) == original); finish_user(b, NNG_ECANCELED);
	mqtt_quic_pipe_close(&f.proto); mqtt_quictran_pipe_close(f.tran);
	finish_user(a, NNG_ECLOSED); finish_user(c, NNG_ECLOSED);
	nng_aio *r; NUTS_PASS(nng_aio_alloc(&r, NULL, NULL));
	nng_aio_set_timeout(r, 1000); nni_pipe_recv(&f.pipe, r); finish_user(r, NNG_ECLOSED);
	f.protocol = false;
	a = send_msg(&f, 3, 0, 0); finish_user(a, NNG_ECLOSED);
	f.protocol = true;
	teardown_fixture(&f);
}
static void test_ack_before_send_complete(void)
{
	fixture f; setup_fixture(&f, true, 4);
	nng_aio *a = send_msg(&f, 3, 1, 0);
	uint16_t pid = nng_mqtt_msg_get_publish_packet_id(nng_aio_get_msg(a));
	uint8_t ack[] = {0x40, 2, pid >> 8, pid & 255};
	feed(&f, 3, ack, sizeof(ack)); finish_user(a, 0);
	// ACK released the user/cache references. The byte-send still owns one.
	complete(&f, 3, 0, 0);
	teardown_fixture(&f);
}
static void test_credit(void)
{
	fixture f; setup_fixture(&f, false, 5); f.tran->sndmax = 1;
	nng_aio *a = send_msg(&f, 3, 1, 10); complete(&f, 3, 0, 0); finish_user(a, 0);
	// A retransmission consumes no additional credit.
	a = send_msg(&f, 3, 1, 10); complete(&f, 3, 0, 0); finish_user(a, 0);
	nng_aio *b = send_msg(&f, 4, 1, 11);
	NUTS_TRUE(!pending(&f, 4, true));
	uint8_t ack[] = {0x40, 2, 0, 10}; feed(&f, 3, ack, sizeof(ack));
	complete(&f, 4, 0, 0); finish_user(b, 0);
	feed(&f, 3, ack, sizeof(ack)); await_io(&f, 3, false);
	a = send_msg(&f, 3, 1, 12); NUTS_TRUE(!pending(&f, 3, true));
	nng_aio_abort(a, NNG_ECANCELED); finish_user(a, NNG_ECANCELED);
	teardown_fixture(&f);
}
static void test_backpressure(void)
{
	fixture f; setup_fixture(&f, true, 4);
	NUTS_PASS(nni_lmq_resize(&f.proto.recv_messages, 2));
	NUTS_PASS(nni_lmq_resize(&f.tran->rxlmq, 2));
	uint8_t pub[] = {0x30, 4, 0, 1, 't', 0};
	for (unsigned i = 0; i < 5; i++) { pub[5] = i; feed(&f, 3, pub, sizeof(pub)); }
	for (unsigned i = 0; i < 1000; i++) {
		nni_mtx_lock(&f.tran->mtx); bool paused = f.tran->substreams[2].pending_rx != NULL;
		nni_mtx_unlock(&f.tran->mtx); if (paused) break; nni_msleep(1);
	}
	NUTS_TRUE(!pending(&f, 3, false));
	for (unsigned i = 0; i < 5; i++) {
		nng_aio *aio; NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
		nng_aio_set_timeout(aio, 1000); mqtt_quic_ctx_recv(&f.sock.master, aio);
		nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
		uint32_t len; uint8_t *payload = nng_mqtt_msg_get_publish_payload(nng_aio_get_msg(aio), &len);
		NUTS_TRUE(len == 1 && payload[0] == i); finish_user(aio, 0);
	}
	await_io(&f, 3, false); teardown_fixture(&f);
}
static void test_subscribe_priority(void)
{
	fixture f; setup_fixture(&f, true, 4);
	f.sock.qos_first = true; f.sock.retry_qos_0 = false;
	nng_aio *blocked = send_msg(&f, 3, 0, 0);
	nng_msg *msg; nng_aio *sub;
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0));
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);
	nng_mqtt_topic_qos *topics = nng_mqtt_topic_qos_array_create(1);
	nng_mqtt_topic_qos_array_set(topics, 0, "pipe/test", 9, 1, 0, 0, 0);
	nng_mqtt_msg_set_subscribe_topics(msg, topics, 1);
	nng_mqtt_topic_qos_array_free(topics, 1);
	NUTS_PASS(nng_aio_alloc(&sub, NULL, NULL)); nng_aio_set_timeout(sub, 1000);
	nng_aio_set_msg(sub, msg); mqtt_quic_ctx_send(&f.sock.master, sub);
	uint16_t pid = nni_mqtt_msg_get_packet_id(msg);
	NUTS_TRUE(pending(&f, 0, true) && pending(&f, 3, true));
	complete(&f, 0, 0, 0);
	uint8_t ack[] = {0x90, 3, pid >> 8, pid & 255, 1}; feed(&f, 0, ack, sizeof(ack));
	finish_user(sub, 0); NUTS_TRUE(nni_aio_busy(blocked));
	complete(&f, 3, 0, 0); finish_user(blocked, 0);
	teardown_fixture(&f);
}
static void test_packet_size_credit(void)
{
	fixture f; setup_fixture(&f, false, 5); f.tran->sndmax = 1; f.tran->packmax = 2;
	nng_aio *a = send_msg(&f, 3, 1, 10); finish_user(a, NNG_EMSGSIZE);
	NUTS_TRUE(f.tran->sndmax == 1);
	f.tran->packmax = 1024; a = send_msg(&f, 3, 1, 11);
	complete(&f, 3, 0, 0); finish_user(a, 0); teardown_fixture(&f);
}
static void test_invalid_client_packet(void)
{
	fixture f; setup_fixture(&f, true, 4);
	nng_msg *msg; nng_aio *aio;
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL)); nng_aio_set_timeout(aio, 1000);
	NUTS_PASS(nng_msg_alloc(&msg, 0)); nng_aio_set_msg(aio, msg);
	mqtt_quic_ctx_send(&f.sock.master, aio); nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_EINVAL); nng_msg_free(msg);
	NUTS_PASS(nng_mqtt_msg_alloc(&msg, 0)); nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNACK);
	nng_aio_set_msg(aio, msg); mqtt_quic_ctx_send(&f.sock.master, aio);
	finish_user(aio, NNG_EPROTO); teardown_fixture(&f);
}
TEST_LIST = {
	{"invalid outgoing client packets preserve caller ownership", test_invalid_client_packet},
	{"qos_first subscription bypasses blocked publication", test_subscribe_priority},
	{"oversized packet does not consume MQTT5 credit", test_packet_size_credit},
	{"protocol pipe independent streams and short writes", test_parallel},
	{"protocol cancellation and pipe close complete all AIOs", test_cancel_and_close},
	{"MQTT ACK before byte send completion preserves lifetime", test_ack_before_send_complete},
	{"MQTT5 receive maximum retransmit and duplicate ACK", test_credit},
	{"slow application backpressures streams without message loss", test_backpressure},
	{NULL, NULL},
};
