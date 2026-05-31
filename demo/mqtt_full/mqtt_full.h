//
// Copyright 2026 NanoMQ Team, Inc.
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef MQTT_FULL_H
#define MQTT_FULL_H

#include <stdbool.h>
#include <stdint.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/nng.h>

enum command_kind {
	CMD_INVALID = 0,
	CMD_CONN,
	CMD_SUB,
	CMD_PUB,
};

enum transport_kind {
	TRANSPORT_INVALID = 0,
	TRANSPORT_MQTT_TCP,
	TRANSPORT_MQTT_TLS_TCP,
	TRANSPORT_MQTT_QUIC,
};

enum tls_verify_mode {
	TLS_VERIFY_NONE = 0,
	TLS_VERIFY_OPTIONAL,
	TLS_VERIFY_REQUIRED,
};

typedef struct topic_list {
	char **items;
	size_t count;
	size_t cap;
} topic_list;

typedef struct app_config {
	enum command_kind   cmd;
	enum transport_kind transport;
	const char *        url;
	uint8_t             proto_ver;
	int                 keepalive;
	const char *        client_id;
	const char *        username;
	const char *        password;
	bool                event_verbose;

	int                 qos;
	int                 count;
	bool                count_set;

	const uint8_t *     payload;
	uint32_t            payload_len;
	const char *        message;
	bool                message_set;
	int                 msg_size;
	bool                msg_size_set;
	uint8_t *           generated_payload;
	int                 interval_ms;
	bool                interval_set;

	topic_list          topics;
	int                 wait_ms;

	const char *        cafile;
	const char *        cert;
	const char *        key;
	const char *        key_password;
	const char *        tls_server_name;
	enum tls_verify_mode tls_verify;
	bool                tls_verify_set;
	bool                tcp_tls_opts_seen;

	const char *        quic_tls_cert_path;
	const char *        quic_tls_key_path;
	const char *        quic_tls_key_password;
	const char *        quic_tls_ca_path;
	bool                quic_tls_verify_peer;
	bool                quic_tls_verify_peer_set;
	bool                quic_tls_opts_seen;

	bool                sqlite_enable;
	const char *        sqlite_db_dir;
	const char *        sqlite_db_name;
	int                 sqlite_max_rows;
	int                 sqlite_flush_threshold;
	bool                retry_qos0;
	int                 retry_interval_ms;
	int                 retry_wait_ms;
	bool                retry_opts_set;
#if defined(NNG_SUPP_SQLITE)
	nng_mqtt_sqlite_option *sqlite_opt;
#endif
} app_config;

typedef struct runtime_state {
	bool event_verbose;
	bool connected;
	bool disconnected;
	int  connect_reason;
	int  disconnect_reason;
	nng_msg *connmsg;
} runtime_state;

enum command_kind parse_command(const char *text);
void              usage(const char *prog);
void              set_low_memory_runtime_defaults(void);

void init_default_config(app_config *cfg);
void free_config(app_config *cfg);
int  parse_args(int argc, char **argv, app_config *cfg);
int  validate_config(app_config *cfg);

int  start_session(
     app_config *cfg, runtime_state *rt, nng_socket *sock, bool *sock_open);
int  run_conn_command(const app_config *cfg, runtime_state *rt);
int  run_sub_command(nng_socket sock, const app_config *cfg);
int  run_pub_command(nng_socket sock, const app_config *cfg);

#endif