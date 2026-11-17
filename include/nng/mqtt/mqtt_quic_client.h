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

// Open only creates the socket. Set CONNECT and QUIC/TLS options on an
// nng_dialer, then call nng_dialer_start. Receive/connect callback messages
// are borrowed for the duration of the callback; duplicate to retain them.
// Optional data-stream selection for PUBLISH/SUBSCRIBE/UNSUBSCRIBE.
// 0 selects automatic routing; 1..4 select a data stream in multistream mode.
// Subscribe and unsubscribe for a topic must use the same stream. Received
// messages report their actual stream (0 is the control stream). Selection
// survives nng_msg_dup and retransmission; it is not encoded in MQTT packets.
NNG_DECL int nng_mqtt_quic_msg_set_stream(nng_msg *, unsigned stream_id);
NNG_DECL unsigned nng_mqtt_quic_msg_get_stream(const nng_msg *);

NNG_DECL int nng_mqttv5_quic_client_open(nng_socket *);
NNG_DECL int nng_mqtt_quic_client_open(nng_socket *);

NNG_DECL int nng_mqtt_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_msg_send_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_msg_recv_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_disconnect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);

NNG_DECL int nng_mqttv5_quic_set_connect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqttv5_quic_set_msg_send_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqttv5_quic_set_msg_recv_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqttv5_quic_set_disconnect_cb(nng_socket *sock, int (*cb)(void *, void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif // NNG_MQTT_QUIC_CLIENT_H
