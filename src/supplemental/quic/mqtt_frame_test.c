#include "mqtt_frame.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "nuts.h"

static void
test_short_frames(void)
{
	// DISCONNECT with reason only, followed by PINGRESP.
	uint8_t buf[] = { 0xe0, 1, 0, 0xd0, 0 };
	size_t h, b;
	NUTS_TRUE(mqtt_quic_frame(buf, 0, &h, &b) == NNG_EAGAIN);
	NUTS_TRUE(mqtt_quic_frame(buf, 1, &h, &b) == NNG_EAGAIN);
	NUTS_TRUE(mqtt_quic_frame(buf, 2, &h, &b) == NNG_EAGAIN);
	NUTS_PASS(mqtt_quic_frame(buf, sizeof(buf), &h, &b));
	NUTS_TRUE(h == 2 && b == 1);
	NUTS_PASS(mqtt_quic_frame(buf + 3, 2, &h, &b));
	NUTS_TRUE(h == 2 && b == 0);
}

static void
test_fragmented_frame(void)
{
	uint8_t buf[131] = { 0x30, 0x80, 1 };
	size_t h, b;
	for (size_t i = 0; i < sizeof(buf); ++i) {
		NUTS_TRUE(mqtt_quic_frame(buf, i, &h, &b) == NNG_EAGAIN);
	}
	NUTS_PASS(mqtt_quic_frame(buf, sizeof(buf), &h, &b));
	NUTS_TRUE(h == 3 && b == 128);
}

static void
test_malformed_length(void)
{
	uint8_t long_header[] = { 0x30, 0x80, 0x80, 0x80, 0x80 };
	uint8_t nonminimal[] = { 0x30, 0x80, 0 };
	size_t h, b;
	NUTS_TRUE(mqtt_quic_frame(long_header, 5, &h, &b) == NNG_EPROTO);
	NUTS_TRUE(mqtt_quic_frame(nonminimal, 3, &h, &b) == NNG_EPROTO);
}

static void
test_remaining_length_bounds(void)
{
	uint8_t byte = 0x30;
	uint32_t remaining = 0;
	uint8_t used = 0;
	NUTS_TRUE(mqtt_get_remaining_length(&byte, 1, &remaining, &used) != 0);
	uint8_t truncated[] = { 0x30, 0x80 };
	NUTS_TRUE(mqtt_get_remaining_length(truncated, 2, &remaining, &used) != 0);
}

static void
test_publish_packet_id_bounds(void)
{
	nng_msg *msg;
	NUTS_PASS(nng_msg_alloc(&msg, 0));
	NUTS_TRUE(nni_msg_get_pub_pid(msg) == 0);
	uint8_t truncated[] = { 0, 1, 'a', 0 };
	NUTS_PASS(nng_msg_append(msg, truncated, sizeof(truncated)));
	NUTS_TRUE(nni_msg_get_pub_pid(msg) == 0);
	nng_msg_clear(msg);
	// A topic longer than 255 bytes must retain both length bytes.
	uint8_t body[260] = { 1, 0 };
	body[258] = 0x12;
	body[259] = 0x34;
	NUTS_PASS(nng_msg_append(msg, body, sizeof(body)));
	NUTS_TRUE(nni_msg_get_pub_pid(msg) == 0x1234);
	nng_msg_free(msg);
}

NUTS_TESTS = {
	{ "PUBLISH packet id bounds and long topic", test_publish_packet_id_bounds },
	{ "short and coalesced frames", test_short_frames },
	{ "fragmented frame at every boundary", test_fragmented_frame },
	{ "malformed remaining length", test_malformed_length },
	{ "remaining length input bounds", test_remaining_length_bounds },
	{ NULL, NULL },
};
