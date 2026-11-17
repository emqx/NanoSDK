// MQTT framing for the byte stream delivered by MsQuic.
#ifndef NNG_SUPP_QUIC_MQTT_FRAME_H
#define NNG_SUPP_QUIC_MQTT_FRAME_H

#include "core/nng_impl.h"

static int
mqtt_quic_frame(const uint8_t *buf, size_t len, size_t *header, size_t *body)
{
	uint32_t remaining = 0;
	if (len < 2) {
		return NNG_EAGAIN;
	}
	for (size_t i = 1; i <= 4; ++i) {
		if (i >= len) {
			return NNG_EAGAIN;
		}
		remaining |= (uint32_t) (buf[i] & 0x7f) << (7 * (i - 1));
		if ((buf[i] & 0x80) == 0) {
			if (i > 1 && buf[i] == 0) {
				return NNG_EPROTO;
			}
			*header = i + 1;
			*body = remaining;
			return len - *header < remaining ? NNG_EAGAIN : 0;
		}
	}
	return NNG_EPROTO;
}
#endif
