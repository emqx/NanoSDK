// Exercise edge cases that cannot be forced reliably over a real network.
#include "nuts.h"
#include "msquic_dial.c"

static quic_send *pending;
static bool fail_send;
static bool fail_start = true;
static QUIC_SEND_FLAGS last_send_flags;
static QUIC_STATUS QUIC_API fake_send(HQUIC h, const QUIC_BUFFER *b,
    uint32_t count, QUIC_SEND_FLAGS flags, void *arg)
{
	(void)h; (void)b; (void)count; (void)flags;
	if (fail_send) return QUIC_STATUS_ABORTED;
	pending = arg; last_send_flags = flags;
	return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API fake_enable(HQUIC h, BOOLEAN enabled)
{
	(void)h; (void)enabled;
	return QUIC_STATUS_SUCCESS;
}
static nni_atomic_int destroyed, drain_entered, drain_done;
static void QUIC_API fake_shutdown(HQUIC h, QUIC_CONNECTION_SHUTDOWN_FLAGS f, QUIC_UINT62 e)
{ (void)h; (void)f; (void)e; }
static void QUIC_API fake_close(HQUIC h)
{ (void)h; nni_atomic_inc(&destroyed); }
static unsigned stream_closes;
static QUIC_STATUS QUIC_API fake_stream_open(HQUIC h, QUIC_STREAM_OPEN_FLAGS flags,
    QUIC_STREAM_CALLBACK_HANDLER handler, void *ctx, HQUIC *out)
{ (void)h; (void)flags; (void)handler; *out = (HQUIC)ctx; return QUIC_STATUS_SUCCESS; }
static QUIC_STATUS QUIC_API fake_stream_start(HQUIC h, QUIC_STREAM_START_FLAGS flags)
{ (void)h; (void)flags; return fail_start ? QUIC_STATUS_OUT_OF_MEMORY : QUIC_STATUS_PENDING; }
static void QUIC_API fake_stream_close(HQUIC h)
{ (void)h; ++stream_closes; }
static const QUIC_API_TABLE table = {
    .StreamOpen = fake_stream_open,
    .StreamStart = fake_stream_start,
    .StreamClose = fake_stream_close,
	.StreamSend = fake_send,
    .ConnectionShutdown = fake_shutdown,
    .ConnectionClose = fake_close,
	.StreamReceiveSetEnabled = fake_enable,
};
static void setup(nni_quic_conn *c, nni_quic_dialer *d)
{
	memset(c, 0, sizeof(*c)); memset(d, 0, sizeof(*d));
	nni_mtx_init(&c->mtx); nni_mtx_init(&d->mtx);
	d->api = &table; c->dialer = d; c->channels = 1;
	c->channel[0].conn = c;
	c->channel[0].handle = (HQUIC)c;
	nni_aio_list_init(&c->channel[0].readers);
	NNI_LIST_INIT(&c->channel[0].sends, quic_send, node);
	fail_send = false; pending = NULL; fail_start = true;
}
static void teardown(nni_quic_conn *c, nni_quic_dialer *d)
{
	NUTS_TRUE(nni_list_empty(&c->channel[0].readers));
	NUTS_TRUE(nni_list_empty(&c->channel[0].sends));
	nni_mtx_fini(&c->mtx); nni_mtx_fini(&d->mtx);
}
static void test_stream_start_failure(void)
{
    nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
    setup(&c, &d); stream_closes = 0; c.priority = -1;
    NUTS_PASS(nni_aio_begin(aio)); c.aio = aio;
    QUIC_CONNECTION_EVENT event = { .Type = QUIC_CONNECTION_EVENT_CONNECTED };
    connection_cb(NULL, &c, &event);
    NUTS_TRUE(stream_closes == 1 && c.channel[0].handle == NULL);
    NUTS_TRUE(c.error == NNG_ECONNREFUSED && !c.connected);
    c.aio = NULL; nni_aio_finish_error(aio, NNG_ECONNREFUSED);
    nng_aio_wait(aio); nng_aio_free(aio); teardown(&c, &d);
}
static void test_start_completion(void)
{
    nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL)); setup(&c, &d);
    nni_atomic_init(&c.refs); nni_atomic_set(&c.refs, 1);
    fail_start = false; c.priority = -1;
    NUTS_PASS(nni_aio_begin(aio)); c.aio = aio;
    QUIC_CONNECTION_EVENT connected = {.Type = QUIC_CONNECTION_EVENT_CONNECTED};
    connection_cb(NULL, &c, &connected);
    NUTS_TRUE(c.aio == aio && !c.connected && nni_aio_busy(aio));
    QUIC_STREAM_EVENT started = {.Type = QUIC_STREAM_EVENT_START_COMPLETE};
    started.START_COMPLETE.Status = QUIC_STATUS_SUCCESS;
    stream_cb(c.channel[0].handle, &c.channel[0], &started);
    nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
    NUTS_TRUE(c.aio == NULL && c.connected && nng_aio_get_output(aio, 0) == &c.stream);
    nng_aio_free(aio); teardown(&c, &d);
}
static void test_defaults(void)
{
    nni_quic_dialer *d;
    NUTS_PASS(nni_quic_dialer_init((void **)&d));
    NUTS_TRUE(!d->enable_mltstrm && !d->enable_0rtt);
    NUTS_TRUE(d->settings.IdleTimeoutMs == 90000 && d->settings.KeepAliveIntervalMs == 60000);
    nni_quic_dialer_fini(d);
}
static void test_priority_send(void)
{
    nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL)); setup(&c, &d);
    int flags = QUIC_HIGH_PRIOR_MSG;
    nni_aio_set_prov_data(aio, &flags);
    nng_iov iov = {(void *)"priority", 8};
    NUTS_PASS(nng_aio_set_iov(aio, 1, &iov)); stream_send(&c, aio);
    NUTS_TRUE(last_send_flags & QUIC_SEND_FLAG_PRIORITY_WORK);
    QUIC_STREAM_EVENT done = {.Type = QUIC_STREAM_EVENT_SEND_COMPLETE};
    done.SEND_COMPLETE.ClientContext = pending;
    stream_cb(c.channel[0].handle, &c.channel[0], &done);
    nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
    nng_aio_free(aio); teardown(&c, &d);
}
static void test_close_during_transport_shutdown(void)
{
    nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
    NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL)); setup(&c, &d);
    char byte; nng_iov iov = {&byte, 1};
    NUTS_PASS(nng_aio_set_iov(aio, 1, &iov)); stream_recv(&c, aio);
    QUIC_CONNECTION_EVENT event = {.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER};
    connection_cb(NULL, &c, &event);
    conn_close(&c);
    NUTS_TRUE(!nni_aio_list_active(aio));
    nng_aio_wait(aio); NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);
    nng_aio_free(aio); teardown(&c, &d);
}
static void test_async_start_failure_backoff(void)
{
    nni_quic_conn c; nni_quic_dialer d;
    NUTS_PASS(nni_init()); setup(&c, &d);
    c.channels = 2; c.connected = true;
    quic_channel *ch = &c.channel[1]; ch->conn = &c; ch->handle = (HQUIC)ch;
    nni_aio_list_init(&ch->readers); NNI_LIST_INIT(&ch->sends, quic_send, node);
    nni_aio_init(&c.reopen_aio, channel_retry, &c); c.reopen_initialized = true;
    QUIC_STREAM_EVENT event = {.Type = QUIC_STREAM_EVENT_START_COMPLETE};
    event.START_COMPLETE.Status = QUIC_STATUS_OUT_OF_MEMORY;
    stream_cb(ch->handle, ch, &event);
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    stream_cb(ch->handle, ch, &event);
    NUTS_TRUE(ch->handle == NULL && c.retry_pending);
    nni_aio_stop(&c.reopen_aio); nni_aio_fini(&c.reopen_aio);
    teardown(&c, &d);
}
static void test_receive_buffers(void)
{
	nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	setup(&c, &d);
	uint8_t out[7] = { 0 };
	nng_iov iov[] = { {out, 2}, {out+2, 5} };
	NUTS_PASS(nng_aio_set_iov(aio, 2, iov));
	stream_recv(&c, aio);
	QUIC_BUFFER buffers[] = { {3, (uint8_t *)"abc"}, {4, (uint8_t *)"defg"} };
	QUIC_STREAM_EVENT event = { .Type = QUIC_STREAM_EVENT_RECEIVE };
	event.RECEIVE.Buffers = buffers; event.RECEIVE.BufferCount = 2;
	event.RECEIVE.TotalBufferLength = 7; event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
	NUTS_TRUE(stream_cb((HQUIC)&c, &c.channel[0], &event) == QUIC_STATUS_SUCCESS);
	nng_aio_wait(aio); NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(event.RECEIVE.TotalBufferLength == 7);
	NUTS_TRUE(nng_aio_count(aio) == 7 && memcmp(out, "abcdefg", 7) == 0);
	/* No posted receive: consume nothing, letting MsQuic retain the bytes. */
	event.RECEIVE.TotalBufferLength = 7;
	stream_cb((HQUIC)&c, &c.channel[0], &event);
	NUTS_TRUE(event.RECEIVE.TotalBufferLength == 0);
	nng_aio_free(aio); teardown(&c, &d);
}
static void test_send_cancel(void)
{
	nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	setup(&c, &d);
	uint8_t bytes[] = "payload";
	nng_iov iov = {bytes, 7};
	NUTS_PASS(nng_aio_set_iov(aio, 1, &iov));
	stream_send(&c, aio); NUTS_TRUE(pending != NULL);
	nng_aio_abort(aio, NNG_ETIMEDOUT); nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ETIMEDOUT);
	nng_aio_free(aio);
	memset(bytes, 0, sizeof(bytes));
	NUTS_TRUE(memcmp(pending->buffer.Buffer, "payload", 7) == 0);
	QUIC_STREAM_EVENT event = { .Type = QUIC_STREAM_EVENT_SEND_COMPLETE };
	event.SEND_COMPLETE.ClientContext = pending; event.SEND_COMPLETE.Canceled = TRUE;
	stream_cb((HQUIC)&c, &c.channel[0], &event);
	teardown(&c, &d);
}
static void test_send_failure(void)
{
	nni_quic_conn c; nni_quic_dialer d; nng_aio *aio;
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	setup(&c, &d); fail_send = true;
	nng_iov iov = {(void *)"x", 1};
	NUTS_PASS(nng_aio_set_iov(aio, 1, &iov));
	stream_send(&c, aio); nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECONNSHUT);
	NUTS_TRUE(pending == NULL);
	nng_aio_free(aio); teardown(&c, &d);
}
static void test_ticket_bounds(void)
{
	nni_quic_conn c; nni_quic_dialer d;
	NUTS_PASS(nni_init()); setup(&c, &d);
	uint8_t bytes[4097] = {0};
	d.enable_0rtt = true; d.rticket_sz = 1; d.rticket[0] = 'x';
	QUIC_CONNECTION_EVENT event = { .Type = QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED };
	event.RESUMPTION_TICKET_RECEIVED.ResumptionTicket = bytes;
	event.RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength = sizeof(bytes);
	connection_cb(NULL, &c, &event);
	NUTS_TRUE(d.rticket_sz == 1 && d.rticket[0] == 'x');
	teardown(&c, &d);
}
static void wait_for_reaping(void *arg)
{
    (void)arg;
    nni_atomic_set(&drain_entered, 1);
    nni_reap_drain();
    nni_atomic_set(&drain_done, 1);
}
static void test_late_shutdown_cleanup(void)
{
    NUTS_PASS(nni_init());
    nni_quic_conn *c = NNI_ALLOC_STRUCT(c);
    nni_quic_dialer d;
    NUTS_TRUE(c != NULL); setup(c, &d);
    nni_atomic_init(&c->refs); nni_atomic_set(&c->refs, 2);
    nni_atomic_flag_reset(&c->reaping);
    nni_atomic_init64(&d.ref); nni_atomic_set64(&d.ref, 2);
    nni_atomic_init(&destroyed); nni_atomic_init(&drain_entered); nni_atomic_init(&drain_done);
    c->handle = (HQUIC)c; d.currcon = c;
    conn_free(c); // release caller, deliberately defer MsQuic shutdown event
    nng_thread *thread;
    NUTS_PASS(nng_thread_create(&thread, wait_for_reaping, NULL));
    while (!nni_atomic_get(&drain_entered)) nni_msleep(1);
    nni_msleep(20);
    NUTS_TRUE(nni_atomic_get(&drain_done) == 0);
    NUTS_TRUE(nni_atomic_get(&destroyed) == 0);
    QUIC_CONNECTION_EVENT event = {.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE};
    connection_cb((HQUIC)c, c, &event);
    nng_thread_destroy(thread);
    NUTS_TRUE(nni_atomic_get(&drain_done) == 1);
    NUTS_TRUE(nni_atomic_get(&destroyed) == 1);
    NUTS_TRUE(nni_atomic_get64(&d.ref) == 1 && d.currcon == NULL);
    nni_mtx_fini(&d.mtx);
}

TEST_LIST = {
    {"close completes receives during transport shutdown", test_close_during_transport_shutdown},
    {"async substream start failure retries with backoff", test_async_start_failure_backoff},
    {"upstream default QUIC settings", test_defaults},
    {"dial waits for control stream start", test_start_completion},
    {"priority flag reaches MsQuic", test_priority_send},
    {"synchronous StreamStart failure closes stream", test_stream_start_failure},
    {"global cleanup waits for delayed QUIC shutdown", test_late_shutdown_cleanup},
	{"all receive buffers including FIN", test_receive_buffers},
	{"cancel frees aio before MsQuic completes", test_send_cancel},
	{"immediate StreamSend failure", test_send_failure},
	{"oversized resumption ticket", test_ticket_bounds},
	{NULL, NULL}
};
