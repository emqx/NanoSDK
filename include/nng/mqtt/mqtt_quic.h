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

typedef struct conf_tls conf_tls;
typedef struct conf_quic conf_quic;
struct conf_tls {
	bool  enable;
	char *url; // "tls+nmq-tcp://addr:port"
	char *cafile;
	char *certfile;
	char *keyfile;
	char *ca;
	char *cert;
	char *key;
	char *key_password;
	bool  verify_peer;
	bool  set_fail; // fail_if_no_peer_cert
};

struct conf_quic {
	conf_tls     tls;
	bool         qos_first; // send QoS msg in high priority
	bool         multi_stream;     
	uint64_t     qkeepalive;		//keepalive timeout interval of QUIC transport
	uint64_t     qconnect_timeout;	// HandshakeIdleTimeoutMs of QUIC
	uint32_t     qdiscon_timeout;	// DisconnectTimeoutMs
	uint32_t     qidle_timeout;	    // Disconnect after idle
	uint8_t      qcongestion_control; // congestion control algorithm 1: bbr 0: cubic
};

// It is an interface only for ffi.
void conf_quic_tls_create(conf_quic **cqp, char *cafile, char *certfile,
    char *keyfile, char *key_pwd);

NNG_DECL int nng_mqttv5_quic_client_open(nng_socket *, const char *url);
NNG_DECL int nng_mqttv5_quic_client_open_conf(
    nng_socket *sock, const char *url, conf_quic *conf);
NNG_DECL int nng_mqtt_quic_client_open(nng_socket *, const char *url);
NNG_DECL int nng_mqtt_quic_client_open_conf(
    nng_socket *sock, const char *url, conf_quic *conf);
NNG_DECL int nng_mqtt_quic_set_connect_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_disconnect_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_msg_recv_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_set_msg_send_cb(
    nng_socket *, int (*cb)(void *, void *), void *arg);
NNG_DECL int nng_mqtt_quic_ack_callback_set(
    nng_socket *sock, void (*cb)(void *), void *arg);
//NNG_DECL int nng_mqtt_quic_set_config(
//    nng_socket *sock, void *node);

#ifdef __cplusplus
}
#endif

#endif // NNG_MQTT_QUIC_CLIENT_H
