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

/**
 * @brief Supported top-level CLI subcommands.
 */
enum command_kind {
	CMD_INVALID = 0,
	CMD_CONN,
	CMD_SUB,
	CMD_PUB,
};

/**
 * @brief Transport types derived from the broker URL scheme.
 */
enum transport_kind {
	TRANSPORT_INVALID = 0,
	TRANSPORT_MQTT_TCP,
	TRANSPORT_MQTT_TLS_TCP,
	TRANSPORT_MQTT_QUIC,
};

/**
 * @brief Verification policy for TLS over TCP connections.
 */
enum tls_verify_mode {
	TLS_VERIFY_NONE = 0,
	TLS_VERIFY_OPTIONAL,
	TLS_VERIFY_REQUIRED,
};

/**
 * @brief Stores repeated topic arguments without copying option string storage.
 */
typedef struct topic_list {
	char **items;
	size_t count;
	size_t cap;
} topic_list;

/**
 * @brief Aggregates all parsed CLI settings before runtime setup begins.
 */
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

	bool                retry_qos0;
	int                 retry_interval_ms;
	int                 retry_wait_ms;
	bool                retry_opts_set;

	bool                sqlite_enable;
	const char *        sqlite_db_dir;
	const char *        sqlite_db_name;
	int                 sqlite_max_rows;
	int                 sqlite_flush_threshold;
#if defined(NNG_SUPP_SQLITE)
	nng_mqtt_sqlite_option *sqlite_opt;
#endif
} app_config;

/**
 * @brief Records callback-observed connection state for long-running commands.
 */
typedef struct runtime_state {
	bool event_verbose;
	bool connected;
	bool disconnected;
	int  connect_reason;
	int  disconnect_reason;
} runtime_state;

/**
 * @brief Parses the subcommand token after the program name.
 * @param text Subcommand token.
 * @return Parsed command enum value.
 */
enum command_kind parse_command(const char *text);
/**
 * @brief Prints command-line help text for all subcommands.
 * @param prog Program name for usage examples.
 */
void              usage(const char *prog);

/**
 * @brief Initializes all config fields to CLI defaults.
 * @param cfg Output config object.
 */
void init_default_config(app_config *cfg);
/**
 * @brief Releases heap-backed config state.
 * @param cfg Config object to clean.
 */
void free_config(app_config *cfg);
/**
 * @brief Parses CLI options after subcommand into cfg.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param cfg In/out config object.
 * @return 0 on success, otherwise an nng error code.
 */
int  parse_args(int argc, char **argv, app_config *cfg);
/**
 * @brief Validates parsed options and finalizes derived payload settings.
 * @param cfg In/out config object.
 * @return 0 on success, otherwise an nng error code.
 */
int  validate_config(app_config *cfg);

/**
 * @brief Opens socket, applies options, registers callbacks, and starts dialing.
 * @param cfg Parsed application config.
 * @param rt Runtime state storage.
 * @param sock Output socket handle.
 * @param sock_open Output flag set true when socket open succeeds.
 * @return 0 on success, otherwise an nng error code.
 */
int  start_session(
     app_config *cfg, runtime_state *rt, nng_socket *sock, bool *sock_open);
/**
 * @brief Runs the conn subcommand loop.
 * @param cfg Parsed application config.
 * @param rt Runtime state updated by callbacks.
 * @return 0 on success, otherwise an nng error code.
 */
int  run_conn_command(const app_config *cfg, runtime_state *rt);
/**
 * @brief Runs the sub subcommand loop.
 * @param sock Open socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
int  run_sub_command(nng_socket sock, const app_config *cfg);
/**
 * @brief Runs the pub subcommand flow.
 * @param sock Open socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
int  run_pub_command(nng_socket sock, const app_config *cfg);

#endif