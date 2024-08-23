#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/bio.h>
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
	SSL_free(ec->ssl);
}

static int
open_conn_init(nng_tls_engine_conn *ec, void *tls, nng_tls_engine_config *cfg)
{
	ec->tls = tls;
	if ((ec->ssl = SSL_new(cfg->ctx)) == NULL) {
		return (NNG_ENOMEM); // most likely
	}
	if (cfg->server_name != NULL) {
		/*
		if (wolfSSL_check_domain_name(ec->ssl, cfg->server_name) !=
		    WOLFSSL_SUCCESS) {
			wolfSSL_free(ec->ssl);
			ec->ssl = NULL;
			return (NNG_ENOMEM);
		}
		*/
	}

	return (0);
}

static void
open_conn_close(nng_tls_engine_conn *ec)
{
	SSL_shutdown(ec->ssl);
}

static int
open_conn_recv(nng_tls_engine_conn *ec, uint8_t *buf, size_t *szp)
{
	int rv;
	if ((rv = SSL_read(ec->ssl, buf, (int) *szp)) < 0) {
		rv = SSL_get_error(ec->ssl, rv);
		fprintf(stderr, "error in recv %d\n", rv);
		// TODO return codes according openssl documents
		return (NNG_ECRYPTO);
	}
	*szp = (size_t) rv;
	return (0);
}

static int
open_conn_send(nng_tls_engine_conn *ec, const uint8_t *buf, size_t *szp)
{
	int rv;

	if ((rv = SSL_write(ec->ssl, buf, (int) (*szp))) <= 0) {
		rv = SSL_get_error(ec->ssl, rv);
		fprintf(stderr, "error in recv %d\n", rv);
		// TODO return codes according openssl documents
		return (NNG_ECRYPTO);
	}
	*szp = (size_t) rv;
	return (0);
}

static int
open_conn_handshake(nng_tls_engine_conn *ec)
{
	int rv;

	rv = SSL_do_handshake(ec->ssl);
	// TODO more rv handle
	if (rv != 0) {
		fprintf(stderr, "openssl do handshake failed rv%d\n", rv);
		return (NNG_ECRYPTO);
	}
	return (0);
}

static bool
open_conn_verified(nng_tls_engine_conn *ec)
{
	long rv = SSL_get_verify_result(ec->ssl);
	fprintf(stderr, "verified result: %ld\n", rv);
	return (X509_V_OK == rv);
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
	char *dup;
	if ((dup = nng_strdup(name)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (cfg->server_name) {
		nng_strfree(cfg->server_name);
	}
	cfg->server_name = dup;
	return (0);
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
	cfg->auth_mode = mode;
	// XXX: REMOVE ME
	return (0);
	switch (mode) {
	case NNG_TLS_AUTH_MODE_NONE:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_NONE, NULL);
		return (0);
	case NNG_TLS_AUTH_MODE_OPTIONAL:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_PEER, NULL);
		return (0);
	case NNG_TLS_AUTH_MODE_REQUIRED:
		SSL_CTX_set_verify(cfg->ctx,
		    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
		return (0);
	}
	return (NNG_EINVAL);
}

static int
open_config_ca_chain(
    nng_tls_engine_config *cfg, const char *certs, const char *crl)
{
	size_t len;

	// Certs and CRL are in PEM data, with terminating NUL byte.
	len = strlen(certs);

	BIO *bio = BIO_new_mem_buf(certs, len);
	if (!bio) {
		fprintf(stderr, "Failed to create BIO\n");
		return (NNG_ENOMEM);
	}

	X509 *cert = PEM_read_bio_X509(bio, NULL, 0, NULL);
	if (!cert) {
		fprintf(stderr, "Failed to load certificate from buffer\n");
		BIO_free(bio);
		return (NNG_ECRYPTO);
	}

	if (SSL_CTX_use_certificate(cfg->ctx, cert) <= 0) {
		fprintf(stderr, "Failed to set certificate in SSL_CTX\n");
		X509_free(cert);
		BIO_free(bio);
		return (NNG_ECRYPTO);
	}

	/* FIXME Should this be done???
	X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    if (!X509_STORE_add_cert(store, cert)) {
	fprintf(stderr, "Failed to add certificate to store\n");
    }
	*/

	if (crl == NULL) {
		return (0);
	}

#ifdef NNG_OPENSSL_HAVE_CRL
	/* TODO
	len = strlen(crl);
	rv  = wolfSSL_CTX_LoadCRLBuffer(
	    cfg->ctx, (void *) crl, len, SSL_FILETYPE_PEM);
	if (rv != SSL_SUCCESS) {
	        return (NNG_ECRYPTO);
	}
	*/
#endif

	return (0);
}

#if NNG_OPENSSL_HAVE_PASSWORD
static int
open_get_password(char *passwd, int size, int rw, void *ctx)
{
	// password is *not* NUL terminated in wolf
	nng_tls_engine_config *cfg = ctx;
	size_t                 len;

	(void) rw;

	if (cfg->pass == NULL) {
		return (0);
	}
	len = strlen(cfg->pass); // Our "ctx" is really the password.
	if (len > (size_t) size) {
		len = size;
	}
	memcpy(passwd, cfg->pass, len);
	return (len);
}
#endif

static int
open_config_own_cert(nng_tls_engine_config *cfg, const char *cert,
    const char *key, const char *pass)
{
	int len;

#if NNG_OPENSSL_HAVE_PASSWORD
	char *dup = NULL;
	if (pass != NULL) {
		if ((dup = nng_strdup(pass)) == NULL) {
			return (NNG_ENOMEM);
		}
	}
	if (cfg->pass != NULL) {
		nng_strfree(cfg->pass);
	}
	cfg->pass = dup;
	SSL_CTX_set_default_passwd_cb_userdata(cfg->ctx, cfg);
	SSL_CTX_set_default_passwd_cb(cfg->ctx, open_get_password);
#else
	(void) pass;
#endif

	len = strlen(cert);
	BIO *biocert = BIO_new_mem_buf(cert, len);
	if (!biocert) {
		fprintf(stderr, "Failed to create BIO\n");
		return (NNG_ENOMEM);
	}
	X509 *xcert = PEM_read_bio_X509(biocert, NULL, 0, NULL);
	if (!xcert) {
		fprintf(stderr, "Failed to load certificate from buffer\n");
		BIO_free(biocert);
		return (NNG_ECRYPTO);
	}
	if (SSL_CTX_use_certificate(cfg->ctx, xcert) <= 0) {
		fprintf(stderr, "Failed to set certificate in SSL_CTX\n");
		X509_free(xcert);
		BIO_free(biocert);
		return (NNG_EINVAL);
	}

	len = strlen(key);
	BIO *biokey = BIO_new_mem_buf(key, len);
	if (!biokey) {
		fprintf(stderr, "Failed to create BIO\n");
		return (NNG_ENOMEM);
	}
	EVP_PKEY *pkey = PEM_read_bio_PrivateKey(biokey, NULL, NULL, NULL);
	if (!pkey) {
		fprintf(stderr, "Failed to load certificate from buffer\n");
		BIO_free(biokey);
		return (NNG_ECRYPTO);
	}
	if (SSL_CTX_use_PrivateKey(cfg->ctx, pkey) <= 0) {
		fprintf(stderr, "Failed to set certificate in SSL_CTX\n");
		EVP_PKEY_free(pkey);
		BIO_free(biokey);
		return (NNG_EINVAL);
	}

	return 0;
}

static int
open_config_version(nng_tls_engine_config *cfg, nng_tls_version min_ver,
    nng_tls_version max_ver)
{
	if ((min_ver > max_ver) || (max_ver > NNG_TLS_1_3)) {
		return (NNG_ENOTSUP);
	}
	// TODO
	(void) cfg;

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
