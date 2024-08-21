#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "core/nng_impl.h"
#include "nng/nng.h"
#include "nng/supplemental/tls/tls.h"
#include <nng/supplemental/tls/engine.h>

struct nng_tls_engine_conn {
	void    *tls; // parent conn
	SSL_CTX *ctx;
	SSL     *ssl;
};

struct nng_tls_engine_config {
	SSL_CTX     *ctx;
	nng_tls_mode mode;
	char        *pass;
	char        *server_name;
	int          auth_mode;
	nni_list     psks;
};

/************************* SSL Connection ***********************/

static void
open_conn_fini(nng_tls_engine_conn *ec)
{
}

static int
open_conn_init(nng_tls_engine_conn *ec, void *tls, nng_tls_engine_config *cfg)
{
	return (0);
}

static void
open_conn_close(nng_tls_engine_conn *ec)
{
}

static int
open_conn_recv(nng_tls_engine_conn *ec, uint8_t *buf, size_t *szp)
{
	return (0);
}

static int
open_conn_send(nng_tls_engine_conn *ec, const uint8_t *buf, size_t *szp)
{
	return (0);
}

static int
open_conn_handshake(nng_tls_engine_conn *ec)
{
	return (0);
}

static bool
open_conn_verified(nng_tls_engine_conn *ec)
{
}

/************************* SSL Configuration ***********************/

static void
open_config_fini(nng_tls_engine_config *cfg)
{
	SSL_CTX_free(cfg->ctx);
	if (cfg->server_name != NULL) {
		nng_strfree(cfg->server_name);
	}
	if (cfg->pass != NULL) {
		nng_strfree(cfg->pass);
	}
}

static int
open_config_init(nng_tls_engine_config *cfg, enum nng_tls_mode mode)
{
	int               auth_mode;
	int               nng_auth;
	const SSL_METHOD *method;

	cfg->mode = mode;
	// TODO NNI_LIST_INIT(&cfg->psks, psk, node);
	if (mode == NNG_TLS_MODE_SERVER) {
		method    = SSLv23_server_method();
		auth_mode = SSL_VERIFY_NONE;
		nng_auth  = NNG_TLS_AUTH_MODE_NONE;
	} else {
		method    = SSLv23_client_method();
		auth_mode = SSL_VERIFY_PEER;
		nng_auth  = NNG_TLS_AUTH_MODE_REQUIRED;
	}

	cfg->ctx = SSL_CTX_new(method);
	if (cfg->ctx == NULL) {
		return (NNG_ENOMEM);
	}
	// Set max/min version TODO

	cfg->auth_mode = nng_auth;
	return (0);
}

static int
open_config_server(nng_tls_engine_config *cfg, const char *name)
{
}

static int
open_config_psk(nng_tls_engine_config *cfg, const char *identity,
    const uint8_t *key, size_t key_len)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(identity);
	NNI_ARG_UNUSED(key);
	NNI_ARG_UNUSED(key_len);
	return (0);
}

static int
open_config_auth_mode(nng_tls_engine_config *cfg, nng_tls_auth_mode mode)
{
	return (0);
}

static int
open_config_ca_chain(
    nng_tls_engine_config *cfg, const char *certs, const char *crl)
{
	return (0);
}

#if NNG_OPENSSL_HAVE_PASSWORD
static int
open_get_password(char *passwd, int size, int rw, void *ctx)
{
	return 0;
}
#endif

static int
open_config_own_cert(nng_tls_engine_config *cfg, const char *cert,
    const char *key, const char *pass)
{
	return 0;
}

static int
open_config_version(nng_tls_engine_config *cfg, nng_tls_version min_ver,
    nng_tls_version max_ver)
{
	return (0);
}


static nng_tls_engine_config_ops open_config_ops = {
	.init     = open_config_init,
	.fini     = open_config_fini,
	.size     = sizeof(nng_tls_engine_config),
	.auth     = open_config_auth_mode,
	.ca_chain = open_config_ca_chain,
	.own_cert = open_config_own_cert,
	.server   = open_config_server,
	.psk      = open_config_psk,
	.version  = open_config_version,
};

static nng_tls_engine_conn_ops open_conn_ops = {
	.size      = sizeof(nng_tls_engine_conn),
	.init      = open_conn_init,
	.fini      = open_conn_fini,
	.close     = open_conn_close,
	.recv      = open_conn_recv,
	.send      = open_conn_send,
	.handshake = open_conn_handshake,
	.verified  = open_conn_verified,
};

static nng_tls_engine open_engine = {
	.version     = NNG_TLS_ENGINE_VERSION,
	.config_ops  = &open_config_ops,
	.conn_ops    = &open_conn_ops,
	.name        = "open",
	.description = "OpenSSL 1.1.1",
	.fips_mode   = false, // commercial users only
};

int
nng_tls_engine_init_open(void)
{
	int rv;
	SSL_load_error_strings();
	rv = OpenSSL_add_ssl_algorithms();
	switch (rv) {
	case 1:
		break;
	default:
		// Best guess...
		EVP_cleanup();
		return (NNG_EINTERNAL);
	}
	return (nng_tls_engine_register(&open_engine));
}

void
nng_tls_engine_fini_open(void)
{
	EVP_cleanup();
}
