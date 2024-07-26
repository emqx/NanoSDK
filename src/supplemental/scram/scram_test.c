//
// Copyright 2024 NanoMQ Team, Inc. <wangwei@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//


#include <string.h>
#include <stdint.h>

#include "scram.h"

#include <nuts.h>

void
test_client_first_msg(void)
{
	char *username = "admin";
	char *pwd      = "public";

	void *ctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, 0);
	NUTS_ASSERT(NULL != ctx);
	char *client_first_msg = scram_client_first_msg(ctx, username);
	NUTS_ASSERT(NULL != client_first_msg);

	// We don't care about the random
	char expect_first_msg[256];
	sprintf(expect_first_msg, "n,,n=%s,r=", username);
	NUTS_ASSERT(0 == strncmp(client_first_msg, expect_first_msg, strlen(expect_first_msg)));

	printf("first msg:%s\n", client_first_msg);

	scram_ctx_free(ctx);
}

void
test_handle_client_first_msg(void)
{
	char *username  = "admin";
	char *pwd       = "public";
	int   salt      = nng_random();
	char *client_first_msg = "n,,n=admin,r=588996903";

	void *ctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, salt);
	NUTS_ASSERT(NULL != ctx);

	char *server_first_msg =
		scram_handle_client_first_msg(ctx, client_first_msg, strlen(client_first_msg));
	NUTS_ASSERT(NULL != server_first_msg);

	printf("server first msg: %s\n", server_first_msg);

	nng_free(server_first_msg, 0);
	scram_ctx_free(ctx);
	(void)username;
}

void
test_handle_server_first_msg(void)
{
	char *username  = "admin";
	char *pwd       = "public";
	char *server_first_msg = "r=5889969031670468145,s=MTcxMDYxMjE0Mw==,i=4096";

	void *ctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, 0);
	NUTS_ASSERT(NULL != ctx);

	char *client_final_msg =
		scram_handle_server_first_msg(ctx, server_first_msg, strlen(server_first_msg));
	NUTS_ASSERT(NULL != client_final_msg);

	printf("client final msg: %s\n", client_final_msg);

	nng_free(client_final_msg, 0);
	scram_ctx_free(ctx);
	(void)username;
}

void
test_handle_client_final_msg(void)
{
	char *username  = "admin";
	char *pwd       = "public";
	int   salt      = nng_random();
	char *client_final_msg = "c=biws,r=5889969031670468145,p=DtmY/yJcVDnfUT4hDRF+pvsG6ec8dctrlNe1XO7er2c=";

	void *ctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, salt);
	NUTS_ASSERT(NULL != ctx);

	char *server_final_msg =
		scram_handle_client_final_msg(ctx, client_final_msg, strlen(client_final_msg));
	// NUTS_ASSERT(NULL != server_final_msg);
	NUTS_ASSERT(NULL == server_final_msg); // Due to no ctx

	// printf("server final msg: %s\n", server_final_msg);

	nng_free(server_final_msg, 0);
	scram_ctx_free(ctx);
	(void)username;
}

/*
void
test_handle_server_final_msg(void)
{
	char *username  = "admin";
	char *pwd       = "public";
	char *server_final_msg = "c=biws,r=5889969031670468145,p=3f+0DVmROdCQE/nAMo4wIKobP6TolZOEldP7s2wCykM=";

	void *ctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256);
	NUTS_ASSERT(NULL != ctx);

	char *server_final_msg =
		scram_handle_client_final_msg(ctx, client_final_msg, strlen(client_final_msg));
	//NUTS_ASSERT(NULL != server_final_msg);
	NUTS_ASSERT(NULL == server_final_msg); // Due to no ctx

	//printf("server final msg: %s\n", server_final_msg);

	nng_free(server_final_msg, 0);
	scram_ctx_free(ctx);
	(void)username;
}
*/

void
test_full_auth(void)
{
	char *username = "admin";
	char *pwd      = "public";
	int   salt     = nng_random();

	void *cctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, 0);
	NUTS_ASSERT(NULL != cctx);

	void *sctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, salt);
	NUTS_ASSERT(NULL != sctx);

	// client generate first msg
	char *client_first_msg = scram_client_first_msg(cctx, username);
	NUTS_ASSERT(NULL != client_first_msg);
	printf("client first msg: %s\n", client_first_msg);

	// server recv client_first_msg and return server_first_msg
	char *server_first_msg =
		scram_handle_client_first_msg(sctx, client_first_msg, strlen(client_first_msg));
	NUTS_ASSERT(NULL != server_first_msg);
	printf("server first msg: %s\n", server_first_msg);

	// client recv server_first_msg and return client_final_msg
	char *client_final_msg =
		scram_handle_server_first_msg(cctx, server_first_msg, strlen(server_first_msg));
	NUTS_ASSERT(NULL != client_final_msg);
	printf("client final msg: %s\n", client_final_msg);

	// server recv client_final_msg and return server_final_msg
	char *server_final_msg =
		scram_handle_client_final_msg(sctx, client_final_msg, strlen(client_final_msg));
	NUTS_ASSERT(NULL != server_final_msg);
	printf("server final msg: %s\n", server_final_msg);

	// client recv server_final_msg and return result
	char *result =
		scram_handle_server_final_msg(cctx, server_final_msg, strlen(server_final_msg));
	NUTS_ASSERT(NULL != result);

	nng_free(server_first_msg, 0);
	nng_free(client_final_msg, 0);
	nng_free(server_final_msg, 0);

	scram_ctx_free(cctx);
	scram_ctx_free(sctx);
}

void
test_full_auth_unmatch(void)
{
	char *username = "admin";
	char *pwd      = "public";
	char *pwd2     = "closed";
	int   salt     = nng_random();

	void *cctx = scram_ctx_create(pwd, strlen(pwd), 4096, SCRAM_SHA256, 0);
	NUTS_ASSERT(NULL != cctx);

	void *sctx = scram_ctx_create(pwd2, strlen(pwd), 4096, SCRAM_SHA256, salt);
	NUTS_ASSERT(NULL != sctx);

	// client generate first msg
	char *client_first_msg = scram_client_first_msg(cctx, username);
	NUTS_ASSERT(NULL != client_first_msg);
	printf("client first msg: %s\n", client_first_msg);

	// server recv client_first_msg and return server_first_msg
	char *server_first_msg =
		scram_handle_client_first_msg(sctx, client_first_msg, strlen(client_first_msg));
	NUTS_ASSERT(NULL != server_first_msg);
	printf("server first msg: %s\n", server_first_msg);

	// client recv server_first_msg and return client_final_msg
	char *client_final_msg =
		scram_handle_server_first_msg(cctx, server_first_msg, strlen(server_first_msg));
	NUTS_ASSERT(NULL != client_final_msg);
	printf("client final msg: %s\n", client_final_msg);

	// server recv client_final_msg and return server_final_msg
	char *server_final_msg =
		scram_handle_client_final_msg(sctx, client_final_msg, strlen(client_final_msg));
	NUTS_ASSERT(NULL == server_final_msg);
	printf("server final msg: %s\n", server_final_msg);

	/*
	// client recv server_final_msg and return result
	char *result =
		scram_handle_server_final_msg(cctx, server_final_msg, strlen(server_final_msg));
	NUTS_ASSERT(NULL == result);
	*/

	nng_free(server_first_msg, 0);
	nng_free(client_final_msg, 0);
	nng_free(server_final_msg, 0);

	scram_ctx_free(cctx);
	scram_ctx_free(sctx);
}

TEST_LIST = {
	{ "client first msg", test_client_first_msg },
	{ "handle client first msg", test_handle_client_first_msg },
	{ "handle server first msg", test_handle_server_first_msg },
	{ "handle client final msg", test_handle_client_final_msg },
	//{ "handle server final msg", test_handle_server_final_msg },
	{ "full enhanced auth", test_full_auth },
	{ "full enhanced auth with unmatch password", test_full_auth_unmatch },
	{ NULL, NULL },
};
