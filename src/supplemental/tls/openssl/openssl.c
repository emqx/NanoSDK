#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPEN_DEBUG 1

#ifdef OPEN_DEBUG
#include <execinfo.h>
void
print_trace(void)
{
	void  *array[10];
	char **strings;
	int    size, i;

	size    = backtrace(array, 10);
	strings = backtrace_symbols(array, size);
	if (strings != NULL) {
		printf("Obtained %d stack frames.\n", size);
		for (i = 0; i < size; i++)
			printf("%s\n", strings[i]);
	}
	free(strings);
}
#endif

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
	SSL     *ssl;
	BIO     *rbio; /* SSL reads from, we write to. */
	BIO     *wbio; /* SSL writes to, we read from. */
	char     rbuf[4096];
	char     wbuf[4096];
	int      running;
	int      ok;
};

struct nng_tls_engine_config {
	SSL_CTX     *ctx;
	nng_tls_mode mode;
	char        *pass;
	char        *server_name;
	int          auth_mode;
	nni_list     psks;
};

static int open_conn_handshake(nng_tls_engine_conn *ec);

/************************* SSL Connection ***********************/

static void
open_conn_fini(nng_tls_engine_conn *ec)
{
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	SSL_free(ec->ssl);
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
}

static int
open_net_read(void *ctx, char *buf, int len) {
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	size_t sz = len;
	int    rv;

	rv = nng_tls_engine_recv(ctx, (uint8_t *) buf, &sz);
	fprintf(stderr, "[%s] end rv%d sz%ld\n", __FUNCTION__, rv, sz);
	switch (rv) {
	case 0:
		return ((int) sz);
	case NNG_EAGAIN:
		return 0 - (SSL_ERROR_WANT_READ);
		// return (WOLFSSL_CBIO_ERR_WANT_READ);
	case NNG_ECLOSED:
		return 0 - (SSL_ERROR_WANT_CONNECT);
		// return (WOLFSSL_CBIO_ERR_CONN_CLOSE);
	case NNG_ECONNSHUT:
		return 0 - (SSL_ERROR_WANT_CONNECT);
		// return (WOLFSSL_CBIO_ERR_CONN_RST);
	default:
		return 0 - (SSL_ERROR_WANT_CONNECT);
		// return (WOLFSSL_CBIO_ERR_GENERAL);
	}
}

static int
open_net_write(void *ctx, const char *buf, int len) {
	fprintf(stderr, "[%s] start %d\n", __FUNCTION__, len);
	size_t sz = len;
	int    rv;

	rv = nng_tls_engine_send(ctx, (const uint8_t *) buf, &sz);
	fprintf(stderr, "[%s] end rv%d sz%ld\n", __FUNCTION__, rv, sz);
	switch (rv) {
	case 0:
		return ((int) sz);

	case NNG_EAGAIN:
		return 0 - (SSL_ERROR_WANT_WRITE);
		// return (WOLFSSL_CBIO_ERR_WANT_WRITE);
	case NNG_ECLOSED:
		return 0 - (SSL_ERROR_WANT_CONNECT);
		// return (WOLFSSL_CBIO_ERR_CONN_CLOSE);
	case NNG_ECONNSHUT:
		return 0 - (SSL_ERROR_WANT_CONNECT);
		// return (WOLFSSL_CBIO_ERR_CONN_RST);
	default:
		return 0 - (SSL_ERROR_WANT_CONNECT);
		// return (WOLFSSL_CBIO_ERR_GENERAL);
	}
}

static int
open_conn_init(nng_tls_engine_conn *ec, void *tls, nng_tls_engine_config *cfg)
{
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	ec->running = 0;
	ec->ok = 0;
	ec->tls = tls;
	if ((ec->ssl = SSL_new(cfg->ctx)) == NULL) {
		fprintf(stderr, "[%s] error in new SSL connection\n", __FUNCTION__);
		return (NNG_ENOMEM); // most likely
	}

	fprintf(stderr, "[%s] mode %d\n", __FUNCTION__, cfg->mode);
	if (cfg->mode == NNG_TLS_MODE_CLIENT)
		SSL_set_connect_state(ec->ssl);
	else
		SSL_set_accept_state(ec->ssl);

	ec->rbio = BIO_new(BIO_s_mem());
	ec->wbio = BIO_new(BIO_s_mem());
	if (!ec->rbio || !ec->wbio) {
		fprintf(stderr, "[%s] error in new BIO for connection\n", __FUNCTION__);
		return (NNG_ENOMEM); // most likely
	}
	SSL_set_bio(ec->ssl, ec->rbio, ec->wbio);

	if (cfg->server_name != NULL) {
		SSL_set_tlsext_host_name(ec->ssl, cfg->server_name);
	}
	open_conn_handshake(ec);
	fprintf(stderr, "[%s] end\n", __FUNCTION__);

	return (0);
}

static void
open_conn_close(nng_tls_engine_conn *ec)
{
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	SSL_shutdown(ec->ssl);
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
}

static int
open_conn_handshake(nng_tls_engine_conn *ec)
{
	int rv;
	int cnt = 10;
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	if (ec->ok == 1)
		return 0;
#ifdef OPEN_DEBUG
	print_trace();
#endif
	if (ec->running == 1)
		//return 0;
		return NNG_EAGAIN;
	ec->running = 1;

	// TODO more rv handle
	while (cnt != 0) {
		rv = SSL_do_handshake(ec->ssl);
		if (rv != 0)
			rv = SSL_get_error(ec->ssl, rv);
		fprintf(stderr, "[%d]openssl do handshake failed rv%d\n", cnt, rv);
		cnt ++;
		if (rv == SSL_ERROR_WANT_READ || rv == SSL_ERROR_WANT_WRITE) {
			int ensz;
			while ((ensz = BIO_read(ec->wbio, ec->rbuf, 4096)) > 0) {
				fprintf(stderr, "[%s] BIO read rv%d\n", __FUNCTION__, ensz);
				if (ensz < 0) {
					if (!BIO_should_retry(ec->wbio))
						return (NNG_ECRYPTO);
				}
				rv = open_net_write(ec->tls, ec->rbuf, ensz);
				if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE)
					return (NNG_EAGAIN);
				else if (rv < 0)
					return (NNG_ECLOSED);
			}

			while ((ensz = open_net_read(ec->tls, ec->wbuf, 1024)) > 0) {
				ensz = BIO_write(ec->rbio, ec->wbuf, ensz);
				fprintf(stderr, "[%s] BIO write rv%d\n", __FUNCTION__, ensz);
				if (ensz < 0) {
					fprintf(stderr, "bio write failed %d\n", ensz);
					if (!BIO_should_retry(ec->rbio))
						return (NNG_ECRYPTO);
				}
			}
		} else {
			rv = 0;
			ec->ok = 1;
			break;
		}
		nng_msleep(200);
		rv = NNG_EAGAIN;
	}
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
	ec->running = 0;
	return rv;
}

static int
open_conn_recv(nng_tls_engine_conn *ec, uint8_t *buf, size_t *szp)
{
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	int rv;
	int ensz = 4096;

	// Am I ready?
	if (!SSL_is_init_finished(ec->ssl)) {
		fprintf(stderr, "[%s] Not Ready !!!!!!!!!!!!\n", __FUNCTION__);
		return (NNG_EAGAIN);
	}

	rv = open_net_read(ec->tls, ec->wbuf, ensz);
	if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE) {
		rv = NNG_EAGAIN;
		goto readopenssl;
	}
	else if (rv < 0)
		return (NNG_ECLOSED);

	fprintf(stderr, "recv %d from tcp\n", rv);
	ensz = BIO_write(ec->rbio, ec->wbuf, rv);
	if (ensz < 0) {
		fprintf(stderr, "bio write failed %d\n", ensz);
		if (!BIO_should_retry(ec->rbio))
			return (NNG_ECRYPTO);
	}

readopenssl:
	if ((rv = SSL_read(ec->ssl, buf, (int) *szp)) < 0) {
		rv = SSL_get_error(ec->ssl, rv);
		fprintf(stderr, "result in recv %d\n", rv);
		if (rv != SSL_ERROR_WANT_READ) {
			return (NNG_ECRYPTO);
		}
		*szp = 0;
		// TODO return codes according openssl documents
	}

	fprintf(stderr, "recv buffer (%ld): ", *szp);
	for (size_t i=0; i<*szp; ++i) fprintf(stderr, "%x ", buf[i]);
	fprintf(stderr, "\n");
	nng_msleep(200);

	fprintf(stderr, "[%s] end\n", __FUNCTION__);
	// *szp = (size_t) rv;
	return (0);
}

static int
open_conn_send(nng_tls_engine_conn *ec, const uint8_t *buf, size_t *szp)
{
	int rv;
	fprintf(stderr, "[%s] start\n", __FUNCTION__);

	// Am I ready?
	if (!SSL_is_init_finished(ec->ssl)) {
		fprintf(stderr, "[%s] Not Ready !!!!!!!!!!!!\n", __FUNCTION__);
		return (NNG_EAGAIN);
	}

	fprintf(stderr, "send buffer (%ld): ", *szp);
	for (size_t i=0; i<*szp; ++i) fprintf(stderr, "%x ", buf[i]);
	fprintf(stderr, "\n");

	if ((rv = SSL_write(ec->ssl, buf, (int) (*szp))) <= 0) {
		rv = SSL_get_error(ec->ssl, rv);
		fprintf(stderr, "result in send %d\n", rv);
		if (rv != SSL_ERROR_WANT_READ) {
			return (NNG_ECRYPTO);
		}
		// TODO return codes according openssl documents
	}
	int ensz = 4096;
	while ((ensz = BIO_read(ec->wbio, ec->rbuf, 4096)) > 0) {
		fprintf(stderr, "BIO read rv%d\n", ensz);
		if (ensz < 0) {
			if (!BIO_should_retry(ec->wbio))
				return (NNG_ECRYPTO);
		}
		rv = open_net_write(ec->tls, ec->rbuf, ensz);
		if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE)
			return (NNG_EAGAIN);
		else if (rv < 0)
			return (NNG_ECLOSED);
	}
	fprintf(stderr, "[%s] end ensz%d\n", __FUNCTION__, ensz);
	// *szp = (size_t) rv;
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
	fprintf(stderr, "[%s] cfg %p ctx %p start\n", __FUNCTION__, cfg, cfg->ctx);
	SSL_CTX_free(cfg->ctx);
	if (cfg->server_name != NULL) {
		nng_strfree(cfg->server_name);
	}
	if (cfg->pass != NULL) {
		nng_strfree(cfg->pass);
	}
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
}

static int
open_config_init(nng_tls_engine_config *cfg, enum nng_tls_mode mode)
{
	int               auth_mode;
	int               nng_auth;
	const SSL_METHOD *method;
	fprintf(stderr, "[%s] start\n", __FUNCTION__);

	cfg->mode = mode;
	// TODO NNI_LIST_INIT(&cfg->psks, psk, node);
	if (mode == NNG_TLS_MODE_SERVER) {
		method    = SSLv23_server_method();
		auth_mode = SSL_VERIFY_NONE;
		nng_auth  = NNG_TLS_AUTH_MODE_NONE;
		fprintf(stderr, "SSL Server Mode\n");
	} else {
		method    = SSLv23_client_method();
		auth_mode = SSL_VERIFY_PEER;
		nng_auth  = NNG_TLS_AUTH_MODE_REQUIRED;
		fprintf(stderr, "SSL Client Mode\n");
	}

	cfg->ctx = SSL_CTX_new(method);
	//cfg->ctx = SSL_CTX_new(TLS_method());
	if (cfg->ctx == NULL) {
		fprintf(stderr, "error in config init \n");
		return (NNG_ENOMEM);
	}
	// Set max/min version TODO

	//SSL_CTX_set_mode(cfg->ctx, SSL_MODE_AUTO_RETRY);
	//SSL_CTX_set_options(cfg->ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

	fprintf(stderr, "[%s] end %p ctx %p\n", __FUNCTION__, cfg, cfg->ctx);
	cfg->auth_mode = nng_auth;
	return (0);
}

static int
open_config_server(nng_tls_engine_config *cfg, const char *name)
{
	char *dup;
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	if ((dup = nng_strdup(name)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (cfg->server_name) {
		nng_strfree(cfg->server_name);
	}
	cfg->server_name = dup;
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
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
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	// XXX: REMOVE ME
	switch (mode) {
	case NNG_TLS_AUTH_MODE_NONE:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_NONE, NULL);
	fprintf(stderr, "[%s] end1\n", __FUNCTION__);
		return (0);
	case NNG_TLS_AUTH_MODE_OPTIONAL:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_PEER, NULL);
	fprintf(stderr, "[%s] end2\n", __FUNCTION__);
		return (0);
	case NNG_TLS_AUTH_MODE_REQUIRED:
		SSL_CTX_set_verify(cfg->ctx,
		    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	fprintf(stderr, "[%s] end3\n", __FUNCTION__);
		return (0);
	}
	fprintf(stderr, "[%s] wrong end\n", __FUNCTION__);
	return (NNG_EINVAL);
}

static int
open_config_ca_chain(
    nng_tls_engine_config *cfg, const char *certs, const char *crl)
{
	size_t len;
	fprintf(stderr, "[%s] start\n", __FUNCTION__);

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

	BIO_free(bio);

	/* FIXME Should this be done???
	X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    if (!X509_STORE_add_cert(store, cert)) {
	fprintf(stderr, "Failed to add certificate to store\n");
    }
	*/

	if (crl == NULL) {
	fprintf(stderr, "[%s] end1\n", __FUNCTION__);
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
	fprintf(stderr, "[%s] end2\n", __FUNCTION__);

	return (0);
}

#if NNG_OPENSSL_HAVE_PASSWORD
static int
open_get_password(char *passwd, int size, int rw, void *ctx)
{
	// password is *not* NUL terminated in wolf
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
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
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
	return (len);
}
#endif

static int
open_config_own_cert(nng_tls_engine_config *cfg, const char *cert,
    const char *key, const char *pass)
{
	int len;
	fprintf(stderr, "[%s] start\n", __FUNCTION__);

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

	if (SSL_CTX_check_private_key(cfg->ctx) != 1) {
		fprintf(stderr, "SSL_CTX_check_private_key failed\n");
		EVP_PKEY_free(pkey);
		BIO_free(biokey);
		return (NNG_ECRYPTO);
	}

	fprintf(stderr, "[%s] end\n", __FUNCTION__);

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
	SSL_library_init();
	SSL_load_error_strings();
	rv = OpenSSL_add_ssl_algorithms();

#if OPENSSL_VERSION_MAJOR < 3
	ERR_load_BIO_strings(); // deprecated since OpenSSL 3.0
#endif
	ERR_load_crypto_strings();

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
	fprintf(stderr, "[%s] start\n", __FUNCTION__);
	EVP_cleanup();
	fprintf(stderr, "[%s] end\n", __FUNCTION__);
}