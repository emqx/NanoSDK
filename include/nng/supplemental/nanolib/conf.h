//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_SUPPLEMENTAL_NANOLIB_CONF_H
#define NNG_SUPPLEMENTAL_NANOLIB_CONF_H

#include <nng/nng.h>
#include <nng/supplemental/nanolib/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal bridge config used by MQTT QUIC client code paths.
typedef struct conf_bridge_node conf_bridge_node;
struct conf_bridge_node {
	size_t       max_recv_queue_len;
	size_t       max_send_queue_len;
	uint8_t      backoff_max;
	nng_duration resend_wait;
	nng_duration resend_interval;
	bool         qos_first;
	bool         retry_qos_0;
	nng_lmq *    ctx_msgs;
};

#ifdef __cplusplus
}
#endif

#endif // NNG_SUPPLEMENTAL_NANOLIB_CONF_H
