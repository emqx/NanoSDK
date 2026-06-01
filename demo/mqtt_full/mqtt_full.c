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
#include <string.h>

/**
 * @brief Program entry for mqtt_full CLI.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Exit code, where 0 indicates success.
 */
int
main(int argc, char **argv)
{
	app_config         cfg;
	runtime_state      rt;
	nng_socket         sock;
	bool               sock_open = false;
	int                rv;
	int                exit_code = 1;
	enum command_kind  cmd;

	if (argc < 2) {
		usage(argv[0]);
		return (1);
	}

	if ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0)) {
		usage(argv[0]);
		return (0);
	}

	cmd = parse_command(argv[1]);
	if (cmd == CMD_INVALID) {
		usage(argv[0]);
		return (1);
	}

	init_default_config(&cfg);
	cfg.cmd = cmd;

	if ((rv = parse_args(argc, argv, &cfg)) != 0) {
		if (rv != NNG_ECANCELED) {
			usage(argv[0]);
		}
		goto done;
	}

	if ((rv = validate_config(&cfg)) != 0) {
		usage(argv[0]);
		goto done;
	}

	memset(&rt, 0, sizeof(rt));
	rt.event_verbose     = cfg.event_verbose;
	rt.connect_reason    = -1;
	rt.disconnect_reason = -1;

	rv = start_session(&cfg, &rt, &sock, &sock_open);
	if (rv != 0) {
		fprintf(stderr, "start session failed: %s\n", nng_strerror(rv));
		goto done;
	}

	switch (cfg.cmd) {
	case CMD_CONN:
		rv = run_conn_command(&cfg, &rt);
		break;
	case CMD_SUB:
		rv = run_sub_command(sock, &cfg);
		break;
	case CMD_PUB:
		rv = run_pub_command(sock, &cfg);
		break;
	default:
		rv = NNG_EINVAL;
		break;
	}

	if (rv != 0) {
		fprintf(stderr, "command failed: %s\n", nng_strerror(rv));
		goto done;
	}

	exit_code = 0;

done:
	if (sock_open) {
		nng_close(sock);
	}
	free_config(&cfg);
	return (exit_code);
}
