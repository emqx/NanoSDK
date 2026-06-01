//
// Copyright 2026 NanoMQ Team, Inc.
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "mqtt_full.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NNG_SUPP_TLS)
#include <nng/supplemental/tls/tls.h>
#endif

#if defined(SUPP_QUIC)
#include <nng/mqtt/mqtt_quic_client.h>
#endif

/**
 * @brief Tracks subscription callback state for one sub command run.
 */
typedef struct suback_state {
	bool suback_seen;
	bool event_verbose;
} suback_state;

/**
 * @brief Applies small send/recv buffers suitable for this CLI tool.
 * @param sock Target socket.
 */
static void
set_low_memory_socket_opts(nng_socket sock)
{
	(void) nng_socket_set_int(sock, NNG_OPT_SENDBUF, 1);
	(void) nng_socket_set_int(sock, NNG_OPT_RECVBUF, 1);
}

/**
 * @brief Opens an MQTT client socket for the requested transport and version.
 * @param t Transport kind.
 * @param proto MQTT protocol version.
 * @param sock Output socket handle.
 * @return 0 on success, otherwise an nng error code.
 */
static int
open_socket_for_transport(enum transport_kind t, uint8_t proto, nng_socket *sock)
{
	switch (t) {
	case TRANSPORT_MQTT_TCP:
	case TRANSPORT_MQTT_TLS_TCP:
		return (proto == MQTT_PROTOCOL_VERSION_v5)
		    ? nng_mqttv5_client_open(sock)
		    : nng_mqtt_client_open(sock);
	case TRANSPORT_MQTT_QUIC:
#if defined(SUPP_QUIC)
		return (proto == MQTT_PROTOCOL_VERSION_v5)
		    ? nng_mqttv5_quic_client_open(sock)
		    : nng_mqtt_quic_client_open(sock);
#else
		fprintf(stderr,
		    "mqtt-quic is not supported by this build. Rebuild/link with QUIC-enabled nng.\n");
		return (NNG_ENOTSUP);
#endif
	default:
		return (NNG_EADDRINVAL);
	}
}

/**
 * @brief Builds and encodes one MQTT CONNECT message.
 * @param msgp Output CONNECT message pointer.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
static int
make_connect_msg(nng_msg **msgp, const app_config *cfg)
{
	nng_msg *msg = NULL;
	int      rv;

	if ((rv = nng_mqtt_msg_alloc(&msg, 0)) != 0) {
		return (rv);
	}

	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(msg, cfg->proto_ver);
	nng_mqtt_msg_set_connect_keep_alive(msg, (uint16_t) cfg->keepalive);
	nng_mqtt_msg_set_connect_clean_session(msg, true);

	if (cfg->client_id != NULL) {
		nng_mqtt_msg_set_connect_client_id(msg, cfg->client_id);
	}
	if (cfg->username != NULL) {
		nng_mqtt_msg_set_connect_user_name(msg, cfg->username);
	}
	if (cfg->password != NULL) {
		nng_mqtt_msg_set_connect_password(msg, cfg->password);
	}

	rv = (cfg->proto_ver == MQTT_PROTOCOL_VERSION_v5) ? nng_mqttv5_msg_encode(msg)
	                                                   : nng_mqtt_msg_encode(msg);
	if (rv != 0) {
		nng_msg_free(msg);
		return (rv);
	}

	*msgp = msg;
	return (0);
}

#if defined(NNG_SUPP_TLS)
/**
 * @brief Reads a text file into a NUL-terminated heap buffer.
 * @param path Input file path.
 * @param out Output buffer pointer.
 * @return 0 on success, otherwise an nng error code.
 */
static int
load_text_file(const char *path, char **out)
{
	FILE * fp;
	long   size;
	size_t readn;
	char * buf;

	if ((path == NULL) || (out == NULL)) {
		return (NNG_EINVAL);
	}

	fp = fopen(path, "rb");
	if (fp == NULL) {
		return (NNG_ENOENT);
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return (NNG_EINVAL);
	}
	size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		return (NNG_EINVAL);
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return (NNG_EINVAL);
	}

	buf = malloc((size_t) size + 1);
	if (buf == NULL) {
		fclose(fp);
		return (NNG_ENOMEM);
	}

	readn = fread(buf, 1, (size_t) size, fp);
	fclose(fp);
	if (readn != (size_t) size) {
		free(buf);
		return (NNG_EINVAL);
	}

	buf[size] = '\0';
	*out      = buf;
	return (0);
}

/**
 * @brief Applies TLS-over-TCP dialer options from config.
 * @param d Target dialer.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
static int
apply_tcp_tls_dialer_opts(nng_dialer d, const app_config *cfg)
{
	nng_tls_config *tls_cfg = NULL;
	nng_tls_auth_mode mode;
	char *cert_data = NULL;
	char *key_data  = NULL;
	int   rv;

	if (cfg->transport != TRANSPORT_MQTT_TLS_TCP) {
		return (0);
	}

	if ((rv = nng_tls_config_alloc(&tls_cfg, NNG_TLS_MODE_CLIENT)) != 0) {
		return (rv);
	}

	switch (cfg->tls_verify) {
	case TLS_VERIFY_NONE:
		mode = NNG_TLS_AUTH_MODE_NONE;
		break;
	case TLS_VERIFY_OPTIONAL:
		mode = NNG_TLS_AUTH_MODE_OPTIONAL;
		break;
	case TLS_VERIFY_REQUIRED:
	default:
		mode = NNG_TLS_AUTH_MODE_REQUIRED;
		break;
	}

	if ((rv = nng_tls_config_auth_mode(tls_cfg, mode)) != 0) {
		goto done;
	}

	if (cfg->tls_server_name != NULL) {
		if ((rv = nng_tls_config_server_name(tls_cfg, cfg->tls_server_name)) != 0) {
			goto done;
		}
	}

	if (cfg->cafile != NULL) {
		if ((rv = nng_tls_config_ca_file(tls_cfg, cfg->cafile)) != 0) {
			goto done;
		}
	}

	if ((cfg->cert != NULL) && (cfg->key != NULL)) {
		if ((rv = load_text_file(cfg->cert, &cert_data)) != 0) {
			goto done;
		}
		if ((rv = load_text_file(cfg->key, &key_data)) != 0) {
			goto done;
		}
		rv = nng_tls_config_own_cert(
		    tls_cfg, cert_data, key_data, cfg->key_password);
		if (rv != 0) {
			goto done;
		}
	}

	rv = nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, tls_cfg);

done:
	free(cert_data);
	free(key_data);
	if (tls_cfg != NULL) {
		nng_tls_config_free(tls_cfg);
	}
	return (rv);
}
#else
/**
 * @brief Returns a clear error when TLS support is unavailable.
 * @param d Unused dialer handle.
 * @param cfg Parsed application config.
 * @return 0 for non-TLS transport, otherwise NNG_ENOTSUP.
 */
static int
apply_tcp_tls_dialer_opts(nng_dialer d, const app_config *cfg)
{
	(void) d;
	if (cfg->transport == TRANSPORT_MQTT_TLS_TCP) {
		fprintf(stderr, "TLS is not supported by this build\n");
		return (NNG_ENOTSUP);
	}
	return (0);
}
#endif

/**
 * @brief Applies QUIC TLS dialer options when explicitly configured.
 * @param d Target dialer.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
static int
apply_quic_tls_dialer_opts(nng_dialer d, const app_config *cfg)
{
	int rv;

	if (cfg->transport != TRANSPORT_MQTT_QUIC) {
		return (0);
	}

	if (!cfg->quic_tls_opts_seen) {
		return (0);
	}

	if (cfg->quic_tls_cert_path != NULL) {
		rv = nng_dialer_set_string(
		    d, NNG_OPT_QUIC_TLS_CACERT_PATH, cfg->quic_tls_cert_path);
		if (rv != 0) {
			return (rv);
		}
	}
	if (cfg->quic_tls_key_path != NULL) {
		rv = nng_dialer_set_string(
		    d, NNG_OPT_QUIC_TLS_KEY_PATH, cfg->quic_tls_key_path);
		if (rv != 0) {
			return (rv);
		}
	}
	if (cfg->quic_tls_key_password != NULL) {
		rv = nng_dialer_set_string(
		    d, NNG_OPT_QUIC_TLS_KEY_PASSWORD, cfg->quic_tls_key_password);
		if (rv != 0) {
			return (rv);
		}
	}
	if (cfg->quic_tls_ca_path != NULL) {
		rv = nng_dialer_set_string(
		    d, NNG_OPT_QUIC_TLS_CA_PATH, cfg->quic_tls_ca_path);
		if (rv != 0) {
			return (rv);
		}
	}
	if (cfg->quic_tls_verify_peer_set) {
		rv = nng_dialer_set_bool(
		    d, NNG_OPT_QUIC_TLS_VERIFY_PEER, cfg->quic_tls_verify_peer);
		if (rv != 0) {
			return (rv);
		}
	}

	return (0);
}

/**
 * @brief Applies MQTT retry-related socket options.
 * @param sock Target socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
static int
apply_retry_socket_opts(nng_socket sock, const app_config *cfg)
{
	int rv;

	rv = nng_socket_set_bool(sock, NNG_OPT_MQTT_RETRY_QOS_0, cfg->retry_qos0);
	if (rv != 0) {
		return (rv);
	}
	rv = nng_socket_set_ms(
	    sock, NNG_OPT_MQTT_RETRY_INTERVAL, (nng_duration) cfg->retry_interval_ms);
	if (rv != 0) {
		return (rv);
	}
	rv = nng_socket_set_uint64(
	    sock, NNG_OPT_MQTT_RETRY_WAIT_TIME, (uint64_t) cfg->retry_wait_ms);
	if (rv != 0) {
		return (rv);
	}
	return (0);
}

/**
 * @brief Enables and configures optional SQLite persistence.
 * @param sock Target socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
static int
apply_sqlite_socket_opts(nng_socket sock, app_config *cfg)
{
	int rv;

	if (!cfg->sqlite_enable) {
		return (0);
	}

#if !defined(NNG_SUPP_SQLITE)
	fprintf(stderr, "SQLite is not supported by this build\n");
	return (NNG_ENOTSUP);
#else
	if ((rv = nng_mqtt_alloc_sqlite_opt(&cfg->sqlite_opt)) != 0) {
		return (rv);
	}

	nng_mqtt_set_sqlite_enable(cfg->sqlite_opt, true);
	nng_mqtt_set_sqlite_db_dir(cfg->sqlite_opt, cfg->sqlite_db_dir);
	nng_mqtt_set_sqlite_max_rows(cfg->sqlite_opt, (size_t) cfg->sqlite_max_rows);
	nng_mqtt_set_sqlite_flush_threshold(
	    cfg->sqlite_opt, (size_t) cfg->sqlite_flush_threshold);
	nng_mqtt_sqlite_db_init(cfg->sqlite_opt, cfg->sqlite_db_name, cfg->proto_ver);

	rv = nng_socket_set_ptr(sock, NNG_OPT_MQTT_SQLITE, cfg->sqlite_opt);
	if (rv != 0) {
		return (rv);
	}

	return (0);
#endif
}

/**
 * @brief Generic MQTT connect callback.
 * @param p Event pipe.
 * @param ev Event kind.
 * @param arg Runtime state pointer.
 */
static void
mqtt_connect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	runtime_state *rt = arg;
	int            reason = -1;

	(void) ev;
	if (nng_pipe_get_int(p, NNG_OPT_MQTT_CONNECT_REASON, &reason) != 0) {
		reason = -1;
	}

	rt->connect_reason = reason;
	rt->connected      = (reason == 0);
	fprintf(stderr, "[connect-cb] reason=%d\n", reason);
}

/**
 * @brief Generic MQTT disconnect callback.
 * @param p Event pipe.
 * @param ev Event kind.
 * @param arg Runtime state pointer.
 */
static void
mqtt_disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	runtime_state *rt = arg;
	int            reason = -1;

	(void) ev;
	if (nng_pipe_get_int(p, NNG_OPT_MQTT_DISCONNECT_REASON, &reason) != 0) {
		reason = -1;
	}

	rt->disconnect_reason = reason;
	rt->disconnected      = true;
	fprintf(stderr, "[disconnect-cb] reason=%d\n", reason);
}

#if defined(SUPP_QUIC)
/**
 * @brief QUIC-specific connect callback bridge.
 * @param msg Callback payload.
 * @param arg Runtime state pointer.
 * @return 0.
 */
static int
quic_connect_cb(void *msg, void *arg)
{
	runtime_state *rt = arg;
	(void) msg;
	rt->connected      = true;
	rt->connect_reason = 0;
	fprintf(stderr, "[connect-cb] quic connected\n");
	return (0);
}

/**
 * @brief QUIC-specific disconnect callback bridge.
 * @param msg Callback payload.
 * @param arg Runtime state pointer.
 * @return 0.
 */
static int
quic_disconnect_cb(void *msg, void *arg)
{
	runtime_state *rt = arg;
	(void) msg;
	rt->disconnected      = true;
	rt->disconnect_reason = 0;
	fprintf(stderr, "[disconnect-cb] quic disconnected\n");
	return (0);
}
#endif

/**
 * @brief Async send callback used to inspect SUBACK responses.
 * @param client MQTT async client.
 * @param msg Message delivered to callback.
 * @param arg Subscription state pointer.
 */
static void
suback_send_callback(nng_mqtt_client *client, nng_msg *msg, void *arg)
{
	suback_state *state = arg;
	uint8_t       ptype;
	uint32_t      count;
	uint8_t *     codes;
	size_t        i;

	(void) client;
	if (msg == NULL) {
		return;
	}

	ptype = nng_mqtt_msg_get_packet_type(msg);
	if (ptype == NNG_MQTT_SUBACK) {
		count = 0;
		codes = nng_mqtt_msg_get_suback_return_codes(msg, &count);
		fprintf(stderr, "[suback-cb] reason codes:");
		if ((codes != NULL) && (count > 0)) {
			for (i = 0; i < count; i++) {
				fprintf(stderr, " %u", (unsigned) codes[i]);
			}
		} else {
			fprintf(stderr, " (empty)");
		}
		fprintf(stderr, "\n");
		state->suback_seen = true;
	} else if (state->event_verbose) {
		fprintf(stderr, "[send-cb] packet type=%u\n", (unsigned) ptype);
	}

	nng_msg_free(msg);
}

/**
 * @brief Registers runtime callbacks for connect/disconnect events.
 * @param sock Target socket.
 * @param cfg Parsed application config.
 * @param rt Runtime state storage.
 * @return Always 0.
 */
static int
register_callbacks(nng_socket sock, const app_config *cfg, runtime_state *rt)
{
	int connect_ok = 0;
	int disconnect_ok = 0;
	int rv;

	rv = nng_mqtt_set_connect_cb(sock, mqtt_connect_cb, rt);
	if (rv == 0) {
		connect_ok = 1;
	}
	rv = nng_mqtt_set_disconnect_cb(sock, mqtt_disconnect_cb, rt);
	if (rv == 0) {
		disconnect_ok = 1;
	}

#if defined(SUPP_QUIC)
	if (cfg->transport == TRANSPORT_MQTT_QUIC) {
		rv = nng_mqtt_quic_set_connect_cb(&sock, quic_connect_cb, rt);
		if (rv == 0) {
			connect_ok = 1;
		}
		rv = nng_mqtt_quic_set_disconnect_cb(&sock, quic_disconnect_cb, rt);
		if (rv == 0) {
			disconnect_ok = 1;
		}
	}
#endif

	if (!connect_ok || !disconnect_ok) {
		if (cfg->event_verbose) {
			fprintf(stderr,
			    "warning: failed to register one or more callbacks\n");
		}
	}

	return (0);
}

/**
 * @brief Builds and sends one PUBLISH packet.
 * @param sock Target socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
static int
publish_once(nng_socket sock, const app_config *cfg)
{
	nng_msg *msg = NULL;
	int      rv;

	if ((rv = nng_mqtt_msg_alloc(&msg, 0)) != 0) {
		return (rv);
	}

	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_dup(msg, 0);
	nng_mqtt_msg_set_publish_retain(msg, 0);
	nng_mqtt_msg_set_publish_qos(msg, (uint8_t) cfg->qos);

	if ((rv = nng_mqtt_msg_set_publish_topic(msg, cfg->topics.items[0])) != 0) {
		nng_msg_free(msg);
		return (rv);
	}

	rv = nng_mqtt_msg_set_publish_payload(
	    msg, (uint8_t *) cfg->payload, cfg->payload_len);
	if (rv != 0) {
		nng_msg_free(msg);
		return (rv);
	}

	/*
	 * Use blocking send so single-shot publish exits only after send
	 * completion on the CLI path.
	 */
	rv = nng_sendmsg(sock, msg, 0);
	if (rv != 0) {
		nng_msg_free(msg);
		return (rv);
	}

	return (0);
}

/**
 * @brief Sleeps for ms when ms is positive.
 * @param ms Sleep duration in milliseconds.
 */
static void
wait_ms(int ms)
{
	if (ms > 0) {
		nng_msleep((nng_duration) ms);
	}
}

/**
 * @brief Opens socket, configures it, and starts the dialer session.
 * @param cfg Parsed application config.
 * @param rt Runtime state storage.
 * @param sock Output socket handle.
 * @param sock_open Output flag set true after socket open succeeds.
 * @return 0 on success, otherwise an nng error code.
 */
int
start_session(app_config *cfg, runtime_state *rt, nng_socket *sock, bool *sock_open)
{
	nng_dialer dialer = { 0 };
	nng_msg *  connmsg = NULL;
	int        rv;
	bool       dialer_created = false;

	*sock_open = false;

	if ((rv = open_socket_for_transport(cfg->transport, cfg->proto_ver, sock)) != 0) {
		return (rv);
	}
	*sock_open = true;

	set_low_memory_socket_opts(*sock);

	if (cfg->retry_opts_set) {
		if ((rv = apply_retry_socket_opts(*sock, cfg)) != 0) {
			return (rv);
		}
	}
	if ((rv = apply_sqlite_socket_opts(*sock, cfg)) != 0) {
		return (rv);
	}

	(void) register_callbacks(*sock, cfg, rt);

	if ((rv = make_connect_msg(&connmsg, cfg)) != 0) {
		return (rv);
	}

	if ((rv = nng_dialer_create(&dialer, *sock, cfg->url)) != 0) {
		nng_msg_free(connmsg);
		return (rv);
	}
	dialer_created = true;

	if ((rv = apply_tcp_tls_dialer_opts(dialer, cfg)) != 0) {
		goto fail;
	}

	if ((rv = apply_quic_tls_dialer_opts(dialer, cfg)) != 0) {
		goto fail;
	}

	if ((rv = nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg)) != 0) {
		goto fail;
	}
	connmsg = NULL;

	if ((rv = nng_dialer_start(dialer, NNG_FLAG_ALLOC)) != 0) {
		goto fail;
	}

	return (0);

fail:
	if (connmsg != NULL) {
		nng_msg_free(connmsg);
		connmsg = NULL;
	}
	if (dialer_created) {
		(void) nng_dialer_close(dialer);
	}
	return (rv);
}

/**
 * @brief Runs the conn command event loop.
 * @param cfg Parsed application config.
 * @param rt Runtime state updated by callbacks.
 * @return 0 on success, otherwise an nng error code.
 */
int
run_conn_command(const app_config *cfg, runtime_state *rt)
{
	(void) cfg;
	bool connected_reported = false;
	bool failed_reported    = false;
	int  poll_ms            = 1000;

	fprintf(stderr, "conn command is running, press Ctrl+C to exit\n");
	while (1) {
		if (rt->connected && !connected_reported) {
			fprintf(stderr, "conn result: connected\n");
			connected_reported = true;
		}
		if ((rt->connect_reason > 0) && !failed_reported) {
			fprintf(stderr, "conn result: failed, reason=%d\n", rt->connect_reason);
			failed_reported = true;
		}
		wait_ms(poll_ms);
	}

	if (!connected_reported && !failed_reported) {
		fprintf(stderr, "conn result: no callback observed before exit\n");
	}
	if (failed_reported && !connected_reported) {
		return (NNG_ECONNREFUSED);
	}
	return (0);
}

/**
 * @brief Runs subscribe flow and prints received publish messages.
 * @param sock Open socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
int
run_sub_command(nng_socket sock, const app_config *cfg)
{
	nng_mqtt_topic_qos *subs = NULL;
	nng_mqtt_client *   client = NULL;
	suback_state        state;
	nng_msg *           msg = NULL;
	size_t              i;
	int                 rv;
	int                 recv_count = 0;
	uint32_t            topic_len;
	uint32_t            payload_len;
	const char *        topic;
	uint8_t *           payload;

	memset(&state, 0, sizeof(state));
	state.event_verbose = cfg->event_verbose;

	subs = nng_mqtt_topic_qos_array_create(cfg->topics.count);
	if (subs == NULL) {
		return (NNG_ENOMEM);
	}
	for (i = 0; i < cfg->topics.count; i++) {
		nng_mqtt_topic_qos_array_set(
		    subs, i, cfg->topics.items[i], (uint8_t) cfg->qos, 0, 0, 0);
	}

	client = nng_mqtt_client_alloc(sock, suback_send_callback, true);
	if (client == NULL) {
		nng_mqtt_topic_qos_array_free(subs, cfg->topics.count);
		return (NNG_ENOMEM);
	}
	client->obj = &state;

	rv = nng_mqtt_subscribe_async(client, subs, cfg->topics.count, NULL);
	if (rv != 0) {
		nng_mqtt_client_free(client, true);
		nng_mqtt_topic_qos_array_free(subs, cfg->topics.count);
		return (rv);
	}

	fprintf(stderr, "subscribed topics:");
	for (i = 0; i < cfg->topics.count; i++) {
		fprintf(stderr, " %s", cfg->topics.items[i]);
	}
	fprintf(stderr, "\n");

	while (1) {
		rv = nng_recvmsg(sock, &msg, 0);
		if (rv != 0) {
			fprintf(stderr, "nng_recvmsg failed: %s\n", nng_strerror(rv));
			continue;
		}

		if (nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_PUBLISH) {
			topic = nng_mqtt_msg_get_publish_topic(msg, &topic_len);
			payload = nng_mqtt_msg_get_publish_payload(msg, &payload_len);
			fprintf(stdout, "recv topic=%.*s payload=%.*s\n",
			    (int) topic_len,
			    (topic != NULL) ? topic : "",
			    (int) payload_len,
			    (payload != NULL) ? (char *) payload : "");
			fflush(stdout);
			recv_count++;
		}

		nng_msg_free(msg);
		msg = NULL;

		if (cfg->count_set && (recv_count >= cfg->count)) {
			break;
		}
	}

	if (msg != NULL) {
		nng_msg_free(msg);
		msg = NULL;
	}

	nng_mqtt_client_free(client, true);
	nng_mqtt_topic_qos_array_free(subs, cfg->topics.count);
	return (0);
}

/**
 * @brief Runs publish flow according to message/count/interval options.
 * @param sock Open socket.
 * @param cfg Parsed application config.
 * @return 0 on success, otherwise an nng error code.
 */
int
run_pub_command(nng_socket sock, const app_config *cfg)
{
	int  rv;
	int  success_count = 0;
	int  target_count = 1;
	bool run_forever = false;
	const int quic_settle_ms = 300;

	wait_ms(100);

	if (cfg->count_set) {
		target_count = cfg->count;
	} else if (cfg->interval_set && (cfg->interval_ms > 1)) {
		run_forever = true;
	}

	while (1) {
		rv = publish_once(sock, cfg);
		if (rv != 0) {
			fprintf(stderr, "publish failed: %s\n", nng_strerror(rv));
			return (rv);
		}

		success_count++;
		if (!run_forever && (success_count >= target_count)) {
			break;
		}
		if (cfg->interval_set && (cfg->interval_ms > 0)) {
			wait_ms(cfg->interval_ms);
		}
	}

	if ((cfg->transport == TRANSPORT_MQTT_QUIC) && (success_count > 0)) {
		/*
		 * QUIC QoS0 completion can be asynchronous. Delay process exit to
		 * avoid dropping the final publish in short-lived CLI runs.
		 */
		wait_ms(quic_settle_ms);
	}

	fprintf(stderr, "publish done: success_count=%d\n", success_count);
	return (0);
}
