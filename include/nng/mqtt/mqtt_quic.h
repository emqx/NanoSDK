#ifndef NNG_MQTT_QUIC_H
#define NNG_MQTT_QUIC_H
#include <nng/mqtt/mqtt_quic_client.h>
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
	bool         qos_first; // legacy hint; does not bypass ordered send queues
	bool         multi_stream;     
	uint64_t     qkeepalive;		//keepalive timeout interval of QUIC transport
	uint64_t     qconnect_timeout;	// HandshakeIdleTimeoutMs of QUIC
	uint32_t     qdiscon_timeout;	// DisconnectTimeoutMs
	uint32_t     qidle_timeout;	    // Disconnect after idle
	uint8_t      qcongestion_control; // congestion control algorithm 1: bbr 0: cubic
};


// Source compatibility for the legacy open(url), then send CONNECT workflow.
// Recompile callers; the new one-argument open ABI is not the old two-argument ABI.
NNG_DECL int nng_mqtt_quic_client_open_url(nng_socket *, const char *);
NNG_DECL int nng_mqttv5_quic_client_open_url(nng_socket *, const char *);
NNG_DECL int nng_mqtt_quic_client_open_conf(nng_socket *, const char *, conf_quic *);
NNG_DECL int nng_mqttv5_quic_client_open_conf(nng_socket *, const char *, conf_quic *);
NNG_DECL int nng_mqtt_quic_ack_callback_set(nng_socket *, void (*)(void *), void *);
NNG_DECL void conf_quic_tls_create(conf_quic **, char *, char *, char *, char *);
// Only frees configurations allocated by conf_quic_tls_create.
NNG_DECL void conf_quic_tls_free(conf_quic *);

#ifndef NNG_MQTT_QUIC_NO_LEGACY_MACROS
#define NNG_MQTT_QUIC_OPEN_SELECT(_1, _2, NAME, ...) NAME
#define nng_mqtt_quic_client_open(...) \
    NNG_MQTT_QUIC_OPEN_SELECT(__VA_ARGS__, nng_mqtt_quic_client_open_url, nng_mqtt_quic_client_open, unused)(__VA_ARGS__)
#define nng_mqttv5_quic_client_open(...) \
    NNG_MQTT_QUIC_OPEN_SELECT(__VA_ARGS__, nng_mqttv5_quic_client_open_url, nng_mqttv5_quic_client_open, unused)(__VA_ARGS__)
#endif
#ifdef __cplusplus
}
#endif
#endif
