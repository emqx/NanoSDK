//
// Copyright 2026 NanoMQ Team, Inc.
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "mqtt_full.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nng/supplemental/util/options.h>

enum options {
	OPT_HELP = 1,
	OPT_URL,
	OPT_TOPIC,
	OPT_QOS,
	OPT_MESSAGE,
	OPT_MSG_SIZE,
	OPT_INTERVAL_MS,
	OPT_COUNT,
	OPT_VERSION,
	OPT_CLIENT_ID,
	OPT_USERNAME,
	OPT_PASSWORD,
	OPT_KEEPALIVE,
	OPT_EVENT_VERBOSE,
	OPT_CAFILE,
	OPT_CERT,
	OPT_KEY,
	OPT_KEY_PASSWORD,
	OPT_TLS_SERVER_NAME,
	OPT_TLS_VERIFY,
	OPT_QUIC_TLS_CERT_PATH,
	OPT_QUIC_TLS_KEY_PATH,
	OPT_QUIC_TLS_KEY_PASSWORD,
	OPT_QUIC_TLS_VERIFY_PEER,
	OPT_QUIC_TLS_CA_PATH,
	OPT_SQLITE_ENABLE,
	OPT_SQLITE_DB_DIR,
	OPT_SQLITE_DB_NAME,
	OPT_SQLITE_MAX_ROWS,
	OPT_SQLITE_FLUSH_THRESHOLD,
	OPT_RETRY_QOS0,
	OPT_RETRY_INTERVAL_MS,
	OPT_RETRY_WAIT_MS,
};

static nng_optspec opts[] = {
	{ .o_name = "help", .o_short = 'h', .o_val = OPT_HELP },
	{ .o_name = "url", .o_short = 'u', .o_val = OPT_URL, .o_arg = true },
	{ .o_name = "topic", .o_short = 't', .o_val = OPT_TOPIC, .o_arg = true },
	{ .o_name = "qos", .o_short = 'q', .o_val = OPT_QOS, .o_arg = true },
	{ .o_name = "message", .o_short = 'm', .o_val = OPT_MESSAGE, .o_arg = true },
	{ .o_name = "msg_size", .o_val = OPT_MSG_SIZE, .o_arg = true },
	{ .o_name = "interval-ms", .o_short = 'i', .o_val = OPT_INTERVAL_MS, .o_arg = true },
	{ .o_name = "count", .o_short = 'c', .o_val = OPT_COUNT, .o_arg = true },
	{ .o_name = "version", .o_short = 'V', .o_val = OPT_VERSION, .o_arg = true },
	{ .o_name = "client-id", .o_val = OPT_CLIENT_ID, .o_arg = true },
	{ .o_name = "username", .o_val = OPT_USERNAME, .o_arg = true },
	{ .o_name = "password", .o_val = OPT_PASSWORD, .o_arg = true },
	{ .o_name = "keepalive", .o_val = OPT_KEEPALIVE, .o_arg = true },
	{ .o_name = "event-verbose", .o_val = OPT_EVENT_VERBOSE },
	{ .o_name = "cafile", .o_val = OPT_CAFILE, .o_arg = true },
	{ .o_name = "cert", .o_val = OPT_CERT, .o_arg = true },
	{ .o_name = "key", .o_val = OPT_KEY, .o_arg = true },
	{ .o_name = "key-password", .o_val = OPT_KEY_PASSWORD, .o_arg = true },
	{ .o_name = "tls-server-name", .o_val = OPT_TLS_SERVER_NAME, .o_arg = true },
	{ .o_name = "tls-verify", .o_val = OPT_TLS_VERIFY, .o_arg = true },
	{ .o_name = "quic-tls-cert-path", .o_val = OPT_QUIC_TLS_CERT_PATH, .o_arg = true },
	{ .o_name = "quic-tls-key-path", .o_val = OPT_QUIC_TLS_KEY_PATH, .o_arg = true },
	{ .o_name = "quic-tls-key-password", .o_val = OPT_QUIC_TLS_KEY_PASSWORD, .o_arg = true },
	{ .o_name = "quic-tls-verify-peer", .o_val = OPT_QUIC_TLS_VERIFY_PEER, .o_arg = true },
	{ .o_name = "quic-tls-ca-path", .o_val = OPT_QUIC_TLS_CA_PATH, .o_arg = true },
	{ .o_name = "sqlite-enable", .o_val = OPT_SQLITE_ENABLE },
	{ .o_name = "sqlite-db-dir", .o_val = OPT_SQLITE_DB_DIR, .o_arg = true },
	{ .o_name = "sqlite-db-name", .o_val = OPT_SQLITE_DB_NAME, .o_arg = true },
	{ .o_name = "sqlite-max-rows", .o_val = OPT_SQLITE_MAX_ROWS, .o_arg = true },
	{ .o_name = "sqlite-flush-threshold", .o_val = OPT_SQLITE_FLUSH_THRESHOLD, .o_arg = true },
	{ .o_name = "retry-qos0", .o_val = OPT_RETRY_QOS0 },
	{ .o_name = "retry-interval-ms", .o_val = OPT_RETRY_INTERVAL_MS, .o_arg = true },
	{ .o_name = "retry-wait-ms", .o_val = OPT_RETRY_WAIT_MS, .o_arg = true },
	{ .o_name = NULL, .o_val = 0 },
};

static bool
starts_with(const char *text, const char *prefix)
{
	if ((text == NULL) || (prefix == NULL)) {
		return (false);
	}
	return (strncmp(text, prefix, strlen(prefix)) == 0);
}

static bool
streq_nocase(const char *a, const char *b)
{
	size_t i;

	if ((a == NULL) || (b == NULL)) {
		return (false);
	}

	for (i = 0; (a[i] != '\0') || (b[i] != '\0'); i++) {
		if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) {
			return (false);
		}
	}
	return (true);
}

static bool
contains_line_break(const char *text)
{
	if (text == NULL) {
		return (false);
	}
	return (strchr(text, '\n') != NULL) || (strchr(text, '\r') != NULL);
}

static int
validate_readable_path(const char *opt_name, const char *path)
{
	if (path == NULL) {
		return (0);
	}

	if (contains_line_break(path)) {
		fprintf(stderr,
		    "%s contains a newline character; please pass the path on one line\n",
		    opt_name);
		return (NNG_EINVAL);
	}

	if (access(path, R_OK) != 0) {
		fprintf(stderr, "%s is not readable: %s (%s)\n", opt_name, path,
		    strerror(errno));
		return (NNG_EINVAL);
	}

	return (0);
}

static int
parse_int_in_range(const char *text, int min_v, int max_v, int *out)
{
	char *end = NULL;
	long  v;

	errno = 0;
	v     = strtol(text, &end, 10);
	if ((errno != 0) || (end == text) || (*end != '\0') || (v < min_v) ||
	    (v > max_v)) {
		return (NNG_EINVAL);
	}
	*out = (int) v;
	return (0);
}

static int
parse_bool_text(const char *text, bool *out)
{
	if ((text == NULL) || (out == NULL)) {
		return (NNG_EINVAL);
	}

	if (streq_nocase(text, "1") || streq_nocase(text, "true") ||
	    streq_nocase(text, "yes") || streq_nocase(text, "on")) {
		*out = true;
		return (0);
	}

	if (streq_nocase(text, "0") || streq_nocase(text, "false") ||
	    streq_nocase(text, "no") || streq_nocase(text, "off")) {
		*out = false;
		return (0);
	}

	return (NNG_EINVAL);
}

static int
parse_tls_verify_mode(const char *text, enum tls_verify_mode *mode)
{
	if ((text == NULL) || (mode == NULL)) {
		return (NNG_EINVAL);
	}
	if (streq_nocase(text, "none")) {
		*mode = TLS_VERIFY_NONE;
		return (0);
	}
	if (streq_nocase(text, "optional")) {
		*mode = TLS_VERIFY_OPTIONAL;
		return (0);
	}
	if (streq_nocase(text, "required")) {
		*mode = TLS_VERIFY_REQUIRED;
		return (0);
	}
	return (NNG_EINVAL);
}

static int
topic_list_add(topic_list *topics, const char *topic)
{
	char **new_items;
	size_t new_cap;

	if ((topics == NULL) || (topic == NULL) || (*topic == '\0')) {
		return (NNG_EINVAL);
	}

	if (topics->count == topics->cap) {
		new_cap = (topics->cap == 0) ? 4 : (topics->cap * 2);
		new_items = realloc(topics->items, new_cap * sizeof(char *));
		if (new_items == NULL) {
			return (NNG_ENOMEM);
		}
		topics->items = new_items;
		topics->cap   = new_cap;
	}

	topics->items[topics->count++] = (char *) topic;
	return (0);
}

static void
topic_list_free(topic_list *topics)
{
	if (topics == NULL) {
		return;
	}
	free(topics->items);
	topics->items = NULL;
	topics->count = 0;
	topics->cap   = 0;
}

static enum transport_kind
detect_transport(const char *url)
{
	if (starts_with(url, "mqtt-tcp://") || starts_with(url, "mqtt-tcp4://") ||
	    starts_with(url, "mqtt-tcp6://")) {
		return (TRANSPORT_MQTT_TCP);
	}
	if (starts_with(url, "tls+mqtt-tcp://") ||
	    starts_with(url, "tls+mqtt-tcp4://") ||
	    starts_with(url, "tls+mqtt-tcp6://")) {
		return (TRANSPORT_MQTT_TLS_TCP);
	}
	if (starts_with(url, "mqtt-quic://")) {
		return (TRANSPORT_MQTT_QUIC);
	}
	return (TRANSPORT_INVALID);
}

static int
generate_random_payload(int payload_len, uint8_t **out)
{
	static const char charset[] =
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	size_t    i;
	size_t    n;
	uint8_t *payload;
	size_t    charset_len = sizeof(charset) - 1;

	if ((payload_len <= 0) || (out == NULL)) {
		return (NNG_EINVAL);
	}

	n = (size_t) payload_len;
	payload = malloc(n);
	if (payload == NULL) {
		return (NNG_ENOMEM);
	}

	for (i = 0; i < n; i++) {
		payload[i] = (uint8_t) charset[nng_random() % charset_len];
	}

	*out = payload;
	return (0);
}

enum command_kind
parse_command(const char *text)
{
	if (text == NULL) {
		return (CMD_INVALID);
	}
	if (strcmp(text, "conn") == 0) {
		return (CMD_CONN);
	}
	if (strcmp(text, "sub") == 0) {
		return (CMD_SUB);
	}
	if (strcmp(text, "pub") == 0) {
		return (CMD_PUB);
	}
	return (CMD_INVALID);
}

void
init_default_config(app_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->cmd                    = CMD_INVALID;
	cfg->transport              = TRANSPORT_INVALID;
	cfg->proto_ver              = MQTT_PROTOCOL_VERSION_v311;
	cfg->keepalive              = 60;
	cfg->qos                    = 0;
	cfg->message                = "hello";
	cfg->payload                = (const uint8_t *) "hello";
	cfg->payload_len            = (uint32_t) (sizeof("hello") - 1);
	cfg->interval_ms            = 0;
	cfg->tls_verify             = TLS_VERIFY_REQUIRED;
	cfg->quic_tls_verify_peer   = true;
	cfg->sqlite_enable          = false;
	cfg->sqlite_db_dir          = "/tmp/nanomq";
	cfg->sqlite_db_name         = "mqtt_full.db";
	cfg->sqlite_max_rows        = 20;
	cfg->sqlite_flush_threshold = 10;
	cfg->retry_qos0             = false;
	cfg->retry_interval_ms      = 10000;
	cfg->retry_wait_ms          = 1000;
#if defined(NNG_SUPP_SQLITE)
	cfg->sqlite_opt             = NULL;
#endif
}

void
free_config(app_config *cfg)
{
	if (cfg == NULL) {
		return;
	}
	topic_list_free(&cfg->topics);
	free(cfg->generated_payload);
	cfg->generated_payload = NULL;
#if defined(NNG_SUPP_SQLITE)
	if (cfg->sqlite_opt != NULL) {
		nng_mqtt_sqlite_db_fini(cfg->sqlite_opt);
		(void) nng_mqtt_free_sqlite_opt(cfg->sqlite_opt);
		cfg->sqlite_opt = NULL;
	}
#endif
}

void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <conn|sub|pub> [options]\n", prog);
	fprintf(stderr, "\nCommon options:\n");
	fprintf(stderr, "  -u, --url <url>                  Broker URL\n");
	fprintf(stderr, "                                   mqtt-tcp://...\n");
	fprintf(stderr, "                                   tls+mqtt-tcp://...\n");
	fprintf(stderr, "                                   mqtt-quic://...\n");
	fprintf(stderr, "  -V, --version <4|5>              MQTT version (default: 4)\n");
	fprintf(stderr, "      --client-id <id>             MQTT client id\n");
	fprintf(stderr, "      --username <name>            MQTT username\n");
	fprintf(stderr, "      --password <password>        MQTT password\n");
	fprintf(stderr, "      --keepalive <sec>            Keepalive seconds (default: 60)\n");
	fprintf(stderr, "      --event-verbose              Print detailed callback logs\n");
	fprintf(stderr, "\nTLS options (for tls+mqtt-tcp://):\n");
	fprintf(stderr, "      --cafile <path>\n");
	fprintf(stderr, "      --cert <path>\n");
	fprintf(stderr, "      --key <path>\n");
	fprintf(stderr, "      --key-password <password>\n");
	fprintf(stderr, "      --tls-server-name <name>\n");
	fprintf(stderr, "      --tls-verify <none|optional|required>\n");
	fprintf(stderr, "\nQUIC TLS options (for mqtt-quic://):\n");
	fprintf(stderr, "      --quic-tls-cert-path <path>\n");
	fprintf(stderr, "      --quic-tls-key-path <path>\n");
	fprintf(stderr, "      --quic-tls-key-password <password>\n");
	fprintf(stderr, "      --quic-tls-verify-peer <true|false>\n");
	fprintf(stderr, "      --quic-tls-ca-path <path>\n");
	fprintf(stderr, "\nSQLite options:\n");
	fprintf(stderr, "      --sqlite-enable\n");
	fprintf(stderr, "      --sqlite-db-dir <dir>            (default: /tmp/nanomq)\n");
	fprintf(stderr, "      --sqlite-db-name <name>          (default: mqtt_full.db)\n");
	fprintf(stderr, "      --sqlite-max-rows <N>            (default: 20)\n");
	fprintf(stderr, "      --sqlite-flush-threshold <N>     (default: 10)\n");
	fprintf(stderr, "      --retry-qos0\n");
	fprintf(stderr, "      --retry-interval-ms <ms>         (default: 10000)\n");
	fprintf(stderr, "      --retry-wait-ms <ms>             (default: 1000)\n");
	fprintf(stderr, "\nSubcommand: conn\n");
	fprintf(stderr, "      (no command-specific options)\n");
	fprintf(stderr, "\nSubcommand: sub\n");
	fprintf(stderr, "  -t, --topic <topic>              Repeatable, e.g. -t topic1 -t topic2\n");
	fprintf(stderr, "  -q, --qos <0|1|2>                Global QoS for all topics (default: 0)\n");
	fprintf(stderr, "  -c, --count <N>                  Exit after N PUBLISH messages\n");
	fprintf(stderr, "\nSubcommand: pub\n");
	fprintf(stderr, "  -t, --topic <topic>              Publish topic (exactly one)\n");
	fprintf(stderr, "  -q, --qos <0|1|2>                QoS (default: 0)\n");
	fprintf(stderr, "  -m, --message <payload>          Payload (mutually exclusive with --msg_size)\n");
	fprintf(stderr, "      --msg_size <N>               Random payload with length N\n");
	fprintf(stderr, "  -i, --interval-ms <ms>           Publish interval\n");
	fprintf(stderr, "  -c, --count <N>                  Publish count\n");
	fprintf(stderr, "\nExamples:\n");
	fprintf(stderr, "  %s conn -u mqtt-tcp://127.0.0.1:1883 -V 4\n", prog);
	fprintf(stderr, "  %s sub  -u mqtt-tcp://127.0.0.1:1883 -t topic1 -t topic2 -q 1\n", prog);
	fprintf(stderr, "  %s pub  -u mqtt-quic://127.0.0.1:14567 -t topic1 -m hello -V 5\n", prog);
}

int
parse_args(int argc, char **argv, app_config *cfg)
{
	int   idx = 2;
	int   val;
	char *arg;
	int   rv;

	while ((rv = nng_opts_parse(argc, argv, opts, &val, &arg, &idx)) == 0) {
		switch (val) {
		case OPT_HELP:
			usage(argv[0]);
			return (NNG_ECANCELED);
		case OPT_URL:
			cfg->url = arg;
			break;
		case OPT_TOPIC:
			rv = topic_list_add(&cfg->topics, arg);
			if (rv != 0) {
				fprintf(stderr, "invalid topic: %s\n", arg);
				return (rv);
			}
			break;
		case OPT_QOS:
			if ((rv = parse_int_in_range(arg, 0, 2, &cfg->qos)) != 0) {
				fprintf(stderr, "invalid qos: %s\n", arg);
				return (rv);
			}
			break;
		case OPT_MESSAGE:
			if (cfg->msg_size_set) {
				fprintf(stderr,
				    "options --message and --msg_size are mutually exclusive\n");
				return (NNG_EINVAL);
			}
			cfg->message     = arg;
			cfg->message_set = true;
			break;
		case OPT_MSG_SIZE:
			if (cfg->message_set) {
				fprintf(stderr,
				    "options --message and --msg_size are mutually exclusive\n");
				return (NNG_EINVAL);
			}
			if ((rv = parse_int_in_range(arg, 1, INT_MAX, &cfg->msg_size)) != 0) {
				fprintf(stderr, "invalid msg_size: %s\n", arg);
				return (rv);
			}
			cfg->msg_size_set = true;
			break;
		case OPT_INTERVAL_MS:
			if ((rv = parse_int_in_range(arg, 0, INT_MAX, &cfg->interval_ms)) != 0) {
				fprintf(stderr, "invalid interval-ms: %s\n", arg);
				return (rv);
			}
			cfg->interval_set = true;
			break;
		case OPT_COUNT:
			if ((rv = parse_int_in_range(arg, 1, INT_MAX, &cfg->count)) != 0) {
				fprintf(stderr, "invalid count: %s\n", arg);
				return (rv);
			}
			cfg->count_set = true;
			break;
		case OPT_VERSION: {
			int version = 0;
			if ((rv = parse_int_in_range(arg, 4, 5, &version)) != 0 ||
			    ((version != MQTT_PROTOCOL_VERSION_v311) &&
			        (version != MQTT_PROTOCOL_VERSION_v5))) {
				fprintf(stderr, "invalid version: %s (only 4 or 5)\n", arg);
				return (NNG_EINVAL);
			}
			cfg->proto_ver = (uint8_t) version;
			break;
		}
		case OPT_CLIENT_ID:
			cfg->client_id = arg;
			break;
		case OPT_USERNAME:
			cfg->username = arg;
			break;
		case OPT_PASSWORD:
			cfg->password = arg;
			break;
		case OPT_KEEPALIVE:
			if ((rv = parse_int_in_range(arg, 0, 65535, &cfg->keepalive)) != 0) {
				fprintf(stderr, "invalid keepalive: %s\n", arg);
				return (rv);
			}
			break;
		case OPT_EVENT_VERBOSE:
			cfg->event_verbose = true;
			break;
		case OPT_CAFILE:
			cfg->cafile            = arg;
			cfg->tcp_tls_opts_seen = true;
			break;
		case OPT_CERT:
			cfg->cert              = arg;
			cfg->tcp_tls_opts_seen = true;
			break;
		case OPT_KEY:
			cfg->key               = arg;
			cfg->tcp_tls_opts_seen = true;
			break;
		case OPT_KEY_PASSWORD:
			cfg->key_password      = arg;
			cfg->tcp_tls_opts_seen = true;
			break;
		case OPT_TLS_SERVER_NAME:
			cfg->tls_server_name   = arg;
			cfg->tcp_tls_opts_seen = true;
			break;
		case OPT_TLS_VERIFY:
			if ((rv = parse_tls_verify_mode(arg, &cfg->tls_verify)) != 0) {
				fprintf(stderr,
				    "invalid tls-verify: %s (use none|optional|required)\n", arg);
				return (rv);
			}
			cfg->tls_verify_set    = true;
			cfg->tcp_tls_opts_seen = true;
			break;
		case OPT_QUIC_TLS_CERT_PATH:
			cfg->quic_tls_cert_path = arg;
			cfg->quic_tls_opts_seen   = true;
			break;
		case OPT_QUIC_TLS_KEY_PATH:
			cfg->quic_tls_key_path = arg;
			cfg->quic_tls_opts_seen = true;
			break;
		case OPT_QUIC_TLS_KEY_PASSWORD:
			cfg->quic_tls_key_password = arg;
			cfg->quic_tls_opts_seen    = true;
			break;
		case OPT_QUIC_TLS_VERIFY_PEER:
			if ((rv = parse_bool_text(arg, &cfg->quic_tls_verify_peer)) != 0) {
				fprintf(stderr,
				    "invalid quic-tls-verify-peer: %s (use true|false)\n", arg);
				return (rv);
			}
			cfg->quic_tls_verify_peer_set = true;
			cfg->quic_tls_opts_seen       = true;
			break;
		case OPT_QUIC_TLS_CA_PATH:
			cfg->quic_tls_ca_path = arg;
			cfg->quic_tls_opts_seen = true;
			break;
		case OPT_SQLITE_ENABLE:
			cfg->sqlite_enable = true;
			break;
		case OPT_SQLITE_DB_DIR:
			cfg->sqlite_db_dir = arg;
			break;
		case OPT_SQLITE_DB_NAME:
			cfg->sqlite_db_name = arg;
			break;
		case OPT_SQLITE_MAX_ROWS:
			if ((rv = parse_int_in_range(arg, 1, INT_MAX, &cfg->sqlite_max_rows)) != 0) {
				fprintf(stderr, "invalid sqlite-max-rows: %s\n", arg);
				return (rv);
			}
			break;
		case OPT_SQLITE_FLUSH_THRESHOLD:
			if ((rv = parse_int_in_range(
			         arg, 1, INT_MAX, &cfg->sqlite_flush_threshold)) != 0) {
				fprintf(stderr, "invalid sqlite-flush-threshold: %s\n", arg);
				return (rv);
			}
			break;
		case OPT_RETRY_QOS0:
			cfg->retry_qos0 = true;
			cfg->retry_opts_set = true;
			break;
		case OPT_RETRY_INTERVAL_MS:
			if ((rv = parse_int_in_range(arg, 0, INT_MAX, &cfg->retry_interval_ms)) != 0) {
				fprintf(stderr, "invalid retry-interval-ms: %s\n", arg);
				return (rv);
			}
			cfg->retry_opts_set = true;
			break;
		case OPT_RETRY_WAIT_MS:
			if ((rv = parse_int_in_range(arg, 0, INT_MAX, &cfg->retry_wait_ms)) != 0) {
				fprintf(stderr, "invalid retry-wait-ms: %s\n", arg);
				return (rv);
			}
			cfg->retry_opts_set = true;
			break;
		default:
			return (NNG_EINVAL);
		}
	}

	if (rv != -1) {
		fprintf(stderr, "invalid options\n");
		return (rv);
	}

	if (idx < argc) {
		fprintf(stderr, "unexpected argument: %s\n", argv[idx]);
		return (NNG_EINVAL);
	}

	return (0);
}

int
validate_config(app_config *cfg)
{
	int    rv;
	size_t msg_len;

	if (cfg->url == NULL) {
		fprintf(stderr, "url is required\n");
		return (NNG_EINVAL);
	}

	cfg->transport = detect_transport(cfg->url);
	if (cfg->transport == TRANSPORT_INVALID) {
		fprintf(stderr,
		    "unsupported url scheme, use mqtt-tcp://, tls+mqtt-tcp:// or mqtt-quic://\n");
		return (NNG_EADDRINVAL);
	}

	if ((cfg->cert != NULL) != (cfg->key != NULL)) {
		fprintf(stderr, "--cert and --key must be set together\n");
		return (NNG_EINVAL);
	}

	if ((cfg->transport != TRANSPORT_MQTT_TLS_TCP) && cfg->tcp_tls_opts_seen) {
		fprintf(stderr,
		    "TLS options (--cafile/--cert/--key/--tls-verify/--tls-server-name) require tls+mqtt-tcp:// URL\n");
		return (NNG_EINVAL);
	}

	if ((cfg->transport != TRANSPORT_MQTT_QUIC) && cfg->quic_tls_opts_seen) {
		fprintf(stderr,
		    "QUIC TLS options require mqtt-quic:// URL\n");
		return (NNG_EINVAL);
	}

	if (cfg->transport == TRANSPORT_MQTT_QUIC) {
		if (((cfg->quic_tls_cert_path != NULL) && (cfg->quic_tls_key_path == NULL)) ||
		    ((cfg->quic_tls_cert_path == NULL) && (cfg->quic_tls_key_path != NULL))) {
			fprintf(stderr,
			    "--quic-tls-cert-path and --quic-tls-key-path must be set together\n");
			return (NNG_EINVAL);
		}

		if ((rv = validate_readable_path(
		         "--quic-tls-cert-path", cfg->quic_tls_cert_path)) != 0) {
			return (rv);
		}
		if ((rv = validate_readable_path(
		         "--quic-tls-key-path", cfg->quic_tls_key_path)) != 0) {
			return (rv);
		}
		if ((rv = validate_readable_path(
		         "--quic-tls-ca-path", cfg->quic_tls_ca_path)) != 0) {
			return (rv);
		}
	}

	if (cfg->cmd == CMD_CONN) {
		if ((cfg->topics.count > 0) || cfg->message_set || cfg->msg_size_set ||
		    cfg->interval_set || cfg->count_set) {
			fprintf(stderr,
			    "conn command does not accept topic/message/msg_size/interval/count\n");
			return (NNG_EINVAL);
		}
	}

	if (cfg->cmd == CMD_SUB) {
		if (cfg->topics.count == 0) {
			fprintf(stderr, "sub command requires at least one -t/--topic\n");
			return (NNG_EINVAL);
		}
		if (cfg->message_set || cfg->msg_size_set || cfg->interval_set) {
			fprintf(stderr,
			    "sub command does not accept --message/--msg_size/--interval-ms\n");
			return (NNG_EINVAL);
		}
	}

	if (cfg->cmd == CMD_PUB) {
		if (cfg->topics.count != 1) {
			fprintf(stderr, "pub command requires exactly one -t/--topic\n");
			return (NNG_EINVAL);
		}
		if (cfg->msg_size_set) {
			rv = generate_random_payload(cfg->msg_size, &cfg->generated_payload);
			if (rv != 0) {
				fprintf(stderr, "generate payload failed: %s\n", nng_strerror(rv));
				return (rv);
			}
			cfg->payload     = cfg->generated_payload;
			cfg->payload_len = (uint32_t) cfg->msg_size;
		} else {
			msg_len = strlen(cfg->message);
			if (msg_len > UINT32_MAX) {
				fprintf(stderr, "message is too long\n");
				return (NNG_EINVAL);
			}
			cfg->payload     = (const uint8_t *) cfg->message;
			cfg->payload_len = (uint32_t) msg_len;
		}
	}

	return (0);
}

void
set_low_memory_runtime_defaults(void)
{
	// Keep nng system thread pools at their minimum practical sizes.
	nng_init_set_parameter(NNG_INIT_MAX_TASK_THREADS, 2);
	nng_init_set_parameter(NNG_INIT_NUM_TASK_THREADS, 2);
	nng_init_set_parameter(NNG_INIT_MAX_EXPIRE_THREADS, 1);
	nng_init_set_parameter(NNG_INIT_NUM_EXPIRE_THREADS, 1);
	nng_init_set_parameter(NNG_INIT_MAX_POLLER_THREADS, 1);
	nng_init_set_parameter(NNG_INIT_NUM_POLLER_THREADS, 1);
	nng_init_set_parameter(NNG_INIT_NUM_RESOLVER_THREADS, 1);
}
