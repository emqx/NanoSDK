//
// Copyright 2024 NanoMQ Team, Inc. <wangwei@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_SUPP_SCRAM_H
#define NNG_SUPP_SCRAM_H

enum SCRAM_digest {
	SCRAM_SHA1,
	SCRAM_SHA256
};

void *scram_ctx_create(char *pwd, int pwdsz, int it_cnt, enum SCRAM_digest digest, int salt);
void  scram_ctx_free(void *ctx);

char *scram_client_first_msg(void *arg, const char *username);

char *scram_handle_client_first_msg(void *arg, const char *msg, int len);
char *scram_handle_server_first_msg(void *arg, const char *msg, int len);
char *scram_handle_client_final_msg(void *arg, const char *msg, int len);
char *scram_handle_server_final_msg(void *arg, const char *msg, int len);

#endif
