//
// Copyright 2022 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_MQTT_QUIC_CLIENT_H
#define NNG_MQTT_QUIC_CLIENT_H

#include <nng/nng.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

NNG_DECL int nng_mqtt_quic_client_open(nng_socket *, const char *url);
NNG_DECL int nng_mqtt_quic_open_keepalive(
    nng_socket *sock, const char *url, uint64_t interval);
NNG_DECL int nng_mqtt_quic_set_connect_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_disconnect_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_msg_recv_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_msg_send_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif // NNG_MQTT_QUIC_CLIENT_H
