#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OPEN_BUF_SZ 4096

#define OPEN_GM 1
//#define OPEN_DEBUG 1
//#define OPEN_TRACE 1

#ifdef OPEN_GM

#define gminfo(format, arg...)                                              \
	do {                                                               \
		fprintf(stderr, "[%s] " format "\n", __FUNCTION__, ##arg); \
	} while (0)

#else

#define gminfo(format, arg...)                                              \
	do {                                                               \
	} while (0)

#endif

#ifdef OPEN_TRACE

#define trace(format, arg...)                                                 \
	do {                                                                  \
		fprintf(stderr, ">>>[%s] " format "\n", __FUNCTION__, ##arg); \
	} while (0)

#else

#define trace(format, arg...)                                                 \
	do {                                                                  \
	} while (0)

#endif

#ifdef OPEN_DEBUG
#include <execinfo.h>
static void
print_trace()
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

static void
print_hex(char *str, const uint8_t *data, size_t len)
{
	if (len == 0)
		return;
	fprintf(stderr, " %s (%ld): ", str, len);
	for (size_t i=0; i<len; ++i) fprintf(stderr, "%x ", data[i]);
	fprintf(stderr, "\n");
}

#define debug(format, arg...)                                              \
	do {                                                               \
		fprintf(stderr, "[%s] " format "\n", __FUNCTION__, ##arg); \
	} while (0)

#else

static void
print_trace()
{
}

static void
print_hex(char *str, const uint8_t *data, size_t len)
{
	(void) str;
	(void) data;
	(void) len;
}
#define debug(format, arg...)                                              \
	do {                                                               \
	} while (0)

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
	// FIXME Not a good way because the length of encrypted payload might over it
	char     rbuf[2 * OPEN_BUF_SZ];
	char     wbuf[2 * OPEN_BUF_SZ];
	char    *wnext;
	int      wnsz;
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
	trace("start");
	SSL_free(ec->ssl);
	trace("end");
}

static int
open_net_read(void *ctx, char *buf, int len) {
	trace("start");
	size_t sz = len;
	int    rv;

	rv = nng_tls_engine_recv(ctx, (uint8_t *) buf, &sz);
	if (rv == 0)
		debug("Read From TCP %ld/%d rv%d", sz, len, rv);
	trace("end");
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
	trace("start %d", len);
	size_t sz = len;
	int    rv;

	rv = nng_tls_engine_send(ctx, (const uint8_t *) buf, &sz);
	debug("Sent To TCP %ld/%d rv%d", sz, len, rv);
	trace("end");
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
	trace("start");
	ec->running = 0;
	ec->ok = 0;
	ec->tls = tls;
	if ((ec->ssl = SSL_new(cfg->ctx)) == NULL) {
		debug("error in new SSL connection\n");
		return (NNG_ENOMEM); // most likely
	}

	debug("%s\n", cfg->mode == NNG_TLS_MODE_SERVER ? "SSL Server Mode":"SSL Client Mode");

	if (cfg->mode == NNG_TLS_MODE_CLIENT)
		SSL_set_connect_state(ec->ssl);
	else
		SSL_set_accept_state(ec->ssl);

	ec->rbio = BIO_new(BIO_s_mem());
	ec->wbio = BIO_new(BIO_s_mem());
	if (!ec->rbio || !ec->wbio) {
		debug("error in new BIO for connection\n");
		return (NNG_ENOMEM); // most likely
	}
	SSL_set_bio(ec->ssl, ec->rbio, ec->wbio);

	ec->wnext = NULL;

	if (cfg->server_name != NULL) {
		SSL_set_tlsext_host_name(ec->ssl, cfg->server_name);
	}
	//open_conn_handshake(ec);
	trace("end");

	return (0);
}

static void
open_conn_close(nng_tls_engine_conn *ec)
{
	trace("start");
	SSL_shutdown(ec->ssl);
	trace("end");
}

static int
open_conn_handshake(nng_tls_engine_conn *ec)
{
	int rv;
	int cnt = 2;
	trace("start");
	if (ec->ok == 1)
		return 0;

	print_trace();

	if (ec->running == 1)
		//return 0;
		return NNG_EAGAIN;
	ec->running = 1;

	// TODO more rv handle
	while (cnt != 0) {
		rv = SSL_do_handshake(ec->ssl);
		if (rv != 0)
			rv = SSL_get_error(ec->ssl, rv);
		debug("[%d]openssl do handshake failed rv%d\n", cnt, rv);
		cnt --;
		if (rv == SSL_ERROR_WANT_READ || rv == SSL_ERROR_WANT_WRITE) {
			int ensz;
			while ((ensz = BIO_read(ec->wbio, ec->rbuf, OPEN_BUF_SZ)) > 0) {
				debug("BIO read rv%d", ensz);
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

			while ((ensz = open_net_read(ec->tls, ec->wbuf, OPEN_BUF_SZ)) > 0) {
				ensz = BIO_write(ec->rbio, ec->wbuf, ensz);
				debug("BIO write rv%d", ensz);
				if (ensz < 0) {
					debug("bio write failed %d\n", ensz);
					if (!BIO_should_retry(ec->rbio))
						return (NNG_ECRYPTO);
				}
			}
		} else if (rv == SSL_ERROR_NONE) {
			rv = 0;
			ec->ok = 1;
			break;
		} else {
		}
		nng_msleep(200);
		rv = NNG_EAGAIN;
	}
	trace("end");
	ec->running = 0;
	return rv;
}

static int
open_conn_recv(nng_tls_engine_conn *ec, uint8_t *buf, size_t *szp)
{
	trace("start");
	int rv;
	int ensz = OPEN_BUF_SZ;

	rv = open_net_read(ec->tls, ec->wbuf, ensz);
	if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE) {
		rv = NNG_EAGAIN;
		goto readopenssl;
	}
	else if (rv < 0)
		return (NNG_ECLOSED);

	debug("recv %d from tcp", rv);
	int written = 0;
	while ((ensz = BIO_write(ec->rbio, ec->wbuf + written, rv - written)) > 0) {
		written += ensz;
		if (written == rv)
			break;
	}
	if (ensz < 0) {
		if (!BIO_should_retry(ec->rbio)) {
			debug("ERROR bio write failed %d", ensz);
			return (NNG_ECRYPTO);
		}
	}

readopenssl:
	if ((rv = SSL_read(ec->ssl, buf, (int) *szp)) < 0) {
		rv = SSL_get_error(ec->ssl, rv);
		// TODO return codes according openssl documents
		if (rv != SSL_ERROR_WANT_READ) {
			debug("ERROR result in openssl read %d", rv);
			return (NNG_ECRYPTO);
		}
		*szp = 0;
	} else {
		*szp = (size_t) rv;
	}
	print_hex("recv buffer:", (const uint8_t *)buf, *szp);
	if (*szp == 0) {
		return NNG_EAGAIN;
	}
	nng_msleep(50);

	trace("end");
	return (0);
}

static int
open_conn_send(nng_tls_engine_conn *ec, const uint8_t *buf, size_t *szp)
{
	int rv;
	int sz = *szp;
	int batchsz = OPEN_BUF_SZ;
	int written2ssl = 0;
	int written2tcp = 0;
	trace("start");

	if (ec->wnext) {
		debug("write last remaining payload first %d", ec->wnsz);
		char *wnext = ec->wnext;
		ec->wnext = NULL;
		rv = open_net_write(ec->tls, wnext, ec->wnsz);
		if (rv > 0) {
			if (rv != ec->wnsz) {
				int dm = ec->wnsz - rv;
				ec->wnext = nng_alloc(sizeof(char) * dm);
				memcpy(ec->wnext, wnext + rv, dm);
				ec->wnsz = dm;
				debug("WARNING still %d bytes not really be put to kernel", dm);
				nng_free(wnext, 0);
				return NNG_EAGAIN;
			}
			nng_free(wnext, 0);
		} else if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE) {
			trace("end3");
			return NNG_EAGAIN;
		} else
			return (NNG_ECLOSED);
	}

	print_hex("send buffer:", buf, sz);

	while (written2tcp < sz) {
		debug("written2tcp %d sz %d", written2tcp, sz);
		int remain = sz - written2tcp;
		batchsz = OPEN_BUF_SZ > remain ? remain : OPEN_BUF_SZ;

		if ((rv = SSL_write(ec->ssl, buf + written2tcp, batchsz)) <= 0) {
			// TODO return codes according openssl documents
			rv = SSL_get_error(ec->ssl, rv);
			if (rv != SSL_ERROR_WANT_READ && rv != SSL_ERROR_WANT_WRITE) {
				debug("ERROR result in send %d", rv);
				return (NNG_ECRYPTO);
			}
			rv = 0;
		}
		// Update the actual length written to ssl
		written2ssl = rv;

		// We would better to read all bufs first then send.
		int ensz;
		int read2buf = 0;
		while ((ensz = BIO_read(ec->wbio, ec->rbuf + read2buf, OPEN_BUF_SZ)) > 0) {
			debug("BIO read ensz%d", ensz);
			read2buf += ensz;
			if (read2buf > 2 * OPEN_BUF_SZ) {
				debug("ERROR BIO read buf over that 2*OPEN_BUF_SZ %d", read2buf);
				return NNG_EINTERNAL;
			}
		}
		if (ensz < 0) {
			//trace("ensz%d", ensz);
			if (!BIO_should_retry(ec->wbio)) {
				debug("ERROR BIO read rv%d", ensz);
				return (NNG_ECRYPTO);
			}
		}

		rv = open_net_write(ec->tls, ec->rbuf, read2buf);
		if (rv > 0) {
			if (rv != read2buf) {
				int dm = read2buf - rv;
				ec->wnext = nng_alloc(sizeof(char) * dm);
				memcpy(ec->wnext, ec->rbuf + rv, dm);
				ec->wnsz = dm;
				debug("WARNING still %d bytes not really be put to kernel", dm);
				written2tcp += written2ssl;
				goto end;
			}
			// A special case for handshake
			written2tcp += written2ssl;
			if (written2tcp == 0)
				goto end;
		} else if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE) {
			trace("end2 read2buf%d written2tcp%d", read2buf, written2tcp);
			if (written2tcp == 0)
				return NNG_EAGAIN;
			*szp = (size_t) written2tcp;
			return 0;
		} else
			return (NNG_ECLOSED);
	}
end:
	trace("end written2tcp%d", written2tcp);
	if (written2tcp == 0)
		return NNG_EAGAIN;
	*szp = (size_t) written2tcp;
	return (0);
}

static bool
open_conn_verified(nng_tls_engine_conn *ec)
{
	long rv = SSL_get_verify_result(ec->ssl);
	debug("verified result: %ld\n", rv);
	return (X509_V_OK == rv);
}

/************************* SSL Configuration ***********************/

static void
open_config_fini(nng_tls_engine_config *cfg)
{
	trace("start cfg %p ctx %p", cfg, cfg->ctx);
	SSL_CTX_free(cfg->ctx);
	if (cfg->server_name != NULL) {
		nng_strfree(cfg->server_name);
	}
	if (cfg->pass != NULL) {
		nng_strfree(cfg->pass);
	}
	trace("end");
}

static int
open_config_init(nng_tls_engine_config *cfg, enum nng_tls_mode mode)
{
	int               auth_mode;
	int               nng_auth;
	const SSL_METHOD *method;
	trace("start");

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

#ifdef OPEN_GM
	method = CNTLS_client_method();
#endif

	cfg->ctx = SSL_CTX_new(method);
	//cfg->ctx = SSL_CTX_new(TLS_method());
	if (cfg->ctx == NULL) {
		debug("error in config init");
		return (NNG_ENOMEM);
	}
	// Set max/min version TODO

	SSL_CTX_set_verify(cfg->ctx, auth_mode, NULL);
	//SSL_CTX_set_mode(cfg->ctx, SSL_MODE_AUTO_RETRY);
	//SSL_CTX_set_options(cfg->ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

	trace("start end %p ctx %p", cfg, cfg->ctx);
	cfg->auth_mode = nng_auth;
	return (0);
}

static int
open_config_server(nng_tls_engine_config *cfg, const char *name)
{
	char *dup;
	trace("start");
	if ((dup = nng_strdup(name)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (cfg->server_name) {
		nng_strfree(cfg->server_name);
	}
	cfg->server_name = dup;
	trace("end");
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
	switch (mode) {
	case NNG_TLS_AUTH_MODE_NONE:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_NONE, NULL);
	debug("AUTH MODE: NONE");
		return (0);
	case NNG_TLS_AUTH_MODE_OPTIONAL:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_PEER, NULL);
	debug("AUTH MODE: OPTION");
		return (0);
	case NNG_TLS_AUTH_MODE_REQUIRED:
		SSL_CTX_set_verify(cfg->ctx,
		    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	debug("AUTH MODE: REQUIRE");
		return (0);
	}
	debug("wrong auth mode!!!!!!!!\n");
	return (NNG_EINVAL);
}

static int
open_config_ca_chain(
    nng_tls_engine_config *cfg, const char *certs, const char *crl)
{
	size_t len;
	trace("start");

	len = strlen(certs);

	BIO *bio = BIO_new_mem_buf(certs, len);
	if (!bio) {
		debug("Failed to create BIO");
		return (NNG_ENOMEM);
	}

	X509 *cert = NULL;
	X509_STORE *store = SSL_CTX_get_cert_store(cfg->ctx);

	while ((cert = PEM_read_bio_X509(bio, NULL, 0, NULL)) != NULL) {
		if (X509_STORE_add_cert(store, cert) == 0) {
			debug("Failed to add certificate to store");
			X509_free(cert);
			BIO_free(bio);
			return (NNG_ECRYPTO);
		}
		X509_free(cert);
	}

	BIO_free(bio);

	if (crl == NULL) {
	trace("end without crl");
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
	trace("end");

	return (0);
}

#if NNG_OPENSSL_HAVE_PASSWORD
static int
open_get_password(char *passwd, int size, int rw, void *ctx)
{
	// password is *not* NUL terminated in wolf
	trace("start");
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
	trace("end");
	return (len);
}
#endif

static int
open_config_own_cert(nng_tls_engine_config *cfg, const char *cert,
    const char *key, const char *pass)
{
	int len;
	int rv = 0;
	BIO *biokey = NULL;
	BIO *biocert = NULL;
	X509 *xcert = NULL;
	EVP_PKEY *pkey = NULL;
	trace("start");

#ifdef OPEN_GM

	if (pass == NULL) {
		gminfo("Please provide GM certificates");
		return NNG_EINVAL;
	}
	char **encerts = pass;
	char *dkey_store = encerts[0];
	char *dkey_private = encerts[1];
	if (dkey_store == NULL || dkey_private == NULL) {
		gminfo("Please provide GM dkey store and dkey private");
		return NNG_EINVAL;
	}
	gminfo("SSL_TLCP start dkeyStore = %s dkey = %s", dkey_store, dkey_private);

#else

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

#endif // OPEN_GM

	len = strlen(cert);
	biocert = BIO_new_mem_buf(cert, len);
	if (!biocert) {
		debug("Failed to create BIO");
		rv = NNG_ENOMEM;
		goto error;
	}
	xcert = PEM_read_bio_X509(biocert, NULL, 0, NULL);
	if (!xcert) {
		debug("Failed to load certificate from buffer");
		rv = NNG_EINVAL;
		goto error;
	}
	if (SSL_CTX_use_certificate(cfg->ctx, xcert) <= 0) {
		debug("Failed to set certificate in SSL_CTX");
		rv = NNG_EINVAL;
		goto error;
	}

	len = strlen(key);
	biokey = BIO_new_mem_buf(key, len);
	if (!biokey) {
		debug("Failed to create BIO");
		rv = NNG_ENOMEM;
		goto error;
	}
	pkey = PEM_read_bio_PrivateKey(biokey, NULL, NULL, NULL);
	if (!pkey) {
		debug("Failed to load certificate from buffer");
		rv = NNG_EINVAL;
		goto error;
	}
	if (SSL_CTX_use_PrivateKey(cfg->ctx, pkey) <= 0) {
		debug("Failed to set certificate in SSL_CTX");
		rv = NNG_EINVAL;
		goto error;
	}

	if (SSL_CTX_check_private_key(cfg->ctx) != 1) {
		debug("SSL_CTX_check_private_key failed");
		rv = NNG_ECRYPTO;
		goto error;
	}

#ifdef OPEN_GM

	// encrypt cert
	if ((rv = SSL_CTX_use_enc_certificate_file(
	         cfg->ctx, dkey_store, SSL_FILETYPE_PEM)) != 1) {
		rv = NNG_EINVAL;
		gminfo("SSL_CTX_use_enc_certificate_file load failed");
		goto error;
	}
	// encrypt private key
	if ((rv = SSL_CTX_use_enc_PrivateKey_file(
	         cfg->ctx, dkey_private, SSL_FILETYPE_PEM)) != 1) {
		rv = NNG_EINVAL;
		gminfo("SSL_CTX_use_enc_PrivateKey_file load failed");
		goto error;
	}
	if ((rv = SSL_CTX_check_enc_private_key(cfg->ctx)) != 1) {
		rv = NNG_ECRYPTO;
		gminfo("SSL_CTX_check_enc_private_key load failed");
		goto error;
	}
	rv = 0;

#endif

error:
	if (xcert)
		X509_free(xcert);
	if (biocert)
		BIO_free(biocert);
	if (pkey)
		EVP_PKEY_free(pkey);
	if (biokey)
		BIO_free(biokey);

	trace("end");
	return rv;
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
	trace("start");
	EVP_cleanup();
	trace("end");
}
