//
// Copyright 2024 NanoMQ Team, Inc. <wangwei@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/x509v3.h>

// Follow the suggestion from Sliepen. https://stackoverflow.com/questions/69079419/how-i-can-read-more-than-16384-bytes-using-openssl-tls
#define OPEN_BUF_SZ 16000

//#define OPEN_DEBUG 1
//#define OPEN_TRACE 1

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
	// 2 * OPEN_BUF_SZ should be enough to put encrypted data
	char     rbuf[2 * OPEN_BUF_SZ];
	char     wbuf[2 * OPEN_BUF_SZ];
	char    *wnext;
	int      wnsz;
	int      wntcpsz;
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
		log_debug("NNG-TLS-NET-RD" "Read From TCP %ld/%d rv%d", sz, len, rv);
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
	log_debug("NNG-TLS-NET-WR" "Sent To TCP %ld/%d rv%d", sz, len, rv);
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
		log_error("NNG-TLS-CONN-INIT" "error in new SSL connection");
		return (NNG_ENOMEM); // most likely
	}

	log_info("NNG-TLS-CONN-INIT" "%s",
			cfg->mode == NNG_TLS_MODE_SERVER ? "SSL Server Mode":"SSL Client Mode");

	ec->rbio = BIO_new(BIO_s_mem());
	ec->wbio = BIO_new(BIO_s_mem());
	if (!ec->rbio || !ec->wbio) {
		log_error("NNG-TLS-CONN-INIT" "error in new BIO for connection");
		return (NNG_ENOMEM); // most likely
	}
	SSL_set_bio(ec->ssl, ec->rbio, ec->wbio);

	if (cfg->mode == NNG_TLS_MODE_CLIENT)
		SSL_set_connect_state(ec->ssl);
	else
		SSL_set_accept_state(ec->ssl);

	ec->wnext = NULL;

	if (cfg->server_name != NULL) {
		SSL_set_tlsext_host_name(ec->ssl, cfg->server_name);
	}
	trace("end");

	return (0);
}

static void
open_conn_close(nng_tls_engine_conn *ec)
{
	trace("start");
	if (ec->wnext)
		nng_free(ec->wnext, 0);
	SSL_shutdown(ec->ssl);
	trace("end");
}

static int
open_conn_handshake(nng_tls_engine_conn *ec)
{
	int rv;
	print_trace();
	if (ec->ok == 1)
		return 0;
	rv = SSL_do_handshake(ec->ssl);
	if (rv != 0) {
		rv = SSL_get_error(ec->ssl, rv);
		if (rv != 0)
			log_warn("NNG-TLS-CONN-HANDSHAKE"
				"openssl handshake still in process rv%d", rv);
	}
	if (rv == SSL_ERROR_WANT_READ || rv == SSL_ERROR_WANT_WRITE) {
		int ensz, sz;
		while ((ensz = open_net_read(ec->tls, ec->wbuf, OPEN_BUF_SZ)) > 0) {
			sz = BIO_write(ec->rbio, ec->wbuf, ensz);
			log_debug("NNG-TLS-CONN-HANDSHAKE" "BIO write sz%d/%d", sz, ensz);
			if (sz < 0) {
				log_debug("NNG-TLS-CONN-HANDSHAKE"
					"bio write failed %d", sz);
				if (!BIO_should_retry(ec->rbio)) {
					log_warn("NNG-TLS-CONN-HANDSHAKE"
						"openssl BIO write failed rv%d", ensz);
					return NNG_ECRYPTO;
				}
				continue;
			}
			SSL_do_handshake(ec->ssl);
			if (SSL_is_init_finished(ec->ssl)) {
				goto finished;
			}
		}

		while ((ensz = BIO_read(ec->wbio, ec->rbuf, OPEN_BUF_SZ)) > 0) {
			log_debug("NNG-TLS-CONN-HANDSHAKE" "BIO read rv%d", ensz);
			if (ensz < 0) {
				if (!BIO_should_retry(ec->wbio)) {
					log_warn("NNG-TLS-CONN-HANDSHAKE"
						"openssl BIO read failed rv%d", ensz);
					return NNG_ECRYPTO;
				}
				continue;
			}
			sz = open_net_write(ec->tls, ec->rbuf, ensz);
			if (sz == 0 - SSL_ERROR_WANT_READ || sz == 0 - SSL_ERROR_WANT_WRITE)
				return (NNG_EAGAIN);
			else if (sz < 0)
				return (NNG_ECLOSED);
			SSL_do_handshake(ec->ssl);
			if (SSL_is_init_finished(ec->ssl)) {
				goto finished;
			}
		}

		return NNG_EAGAIN;
	}
	if (rv == SSL_ERROR_NONE) {
finished:
		log_warn("NNG-TLS-CONN-HANDSHAKE"
				"openssl do handshake successfully");
		ec->ok = 1;
		return 0;
	}
	return NNG_ECRYPTO;
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

	int written = 0;
	while ((ensz = BIO_write(ec->rbio, ec->wbuf + written, rv - written)) > 0) {
		written += ensz;
		if (written == rv)
			break;
	}
	if (ensz < 0) {
		log_debug("NNG-TLS-CONN-RECV" "bio write result %d", ensz);
		if (!BIO_should_retry(ec->rbio)) {
			log_error("NNG-TLS-CONN-RECV"
				"[%d]openssl BIO write failed rv%d", ensz);
			return (NNG_ECRYPTO);
		}
	}
	log_debug("NNG-TLS-CONN-RECV"
			"recv %d from tcp and written %d to BIO", rv, written);

readopenssl:
	if ((rv = SSL_read(ec->ssl, buf, (int) *szp)) < 0) {
		rv = SSL_get_error(ec->ssl, rv);
		// TODO return codes according openssl documents
		if (rv != SSL_ERROR_WANT_READ) {
			log_error("NNG-TLS-CONN-RECV"
				"openssl read failed rv%d", rv);
			return (NNG_ECRYPTO);
		}
		*szp = 0;
	} else {
		*szp = (size_t) rv;
	}
	print_hex("recv buffer:", (const uint8_t *)buf, *szp);
	if (*szp == 0) {
		trace("end eagain");
		return NNG_EAGAIN;
	}

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
		log_debug("NNG-TLS-CONN-SEND"
			"write last remaining payload first %d", ec->wnsz);
		char *wnext = ec->wnext;
		rv = open_net_write(ec->tls, wnext, ec->wnsz);
		if (rv > 0) {
			ec->wnext = NULL;
			if (rv != ec->wnsz) {
				int dm = ec->wnsz - rv;
				ec->wnext = nng_alloc(sizeof(char) * dm);
				memcpy(ec->wnext, wnext + rv, dm);
				ec->wnsz = dm;
				log_debug("NNG-TLS-CONN-SEND"
					"written%d remain%d bytes to put to kernel", rv, dm);
				nng_free(wnext, 0);
				return NNG_EAGAIN;
			}
			nng_free(wnext, 0);
			written2tcp = ec->wntcpsz;
			log_debug("writing done%d written2tcp%d", ec->wnsz, written2tcp);
			goto end;
		} else if (rv == 0 - SSL_ERROR_WANT_READ || rv == 0 - SSL_ERROR_WANT_WRITE) {
			trace("end3");
			return (NNG_EAGAIN);
		} else {
			return (NNG_ECLOSED);
		}
	}

	print_hex("send buffer:", buf, sz);

	while (written2tcp < sz) {
		log_debug("NNG-TLS-CONN-SEND"
			"written2tcp %d sz %d", written2tcp, sz);
		int remain = sz - written2tcp;
		batchsz = OPEN_BUF_SZ > remain ? remain : OPEN_BUF_SZ;

		if ((rv = SSL_write(ec->ssl, buf + written2tcp, batchsz)) <= 0) {
			// TODO return codes according openssl documents
			rv = SSL_get_error(ec->ssl, rv);
			if (rv != SSL_ERROR_WANT_READ && rv != SSL_ERROR_WANT_WRITE) {
				log_error("NNG-TLS-CONN-SEND" "error in ssl write%d", rv);
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
			log_debug("NNG-TLS-CONN-SEND" "BIO read ensz%d", ensz);
			read2buf += ensz;
			if (read2buf > 2 * OPEN_BUF_SZ) {
				log_error("NNG-TLS-CONN-SEND"
					"BIO read buf over that 2*OPEN_BUF_SZ %d", read2buf);
				return NNG_EINTERNAL;
			}
		}
		if (ensz < 0) {
			//trace("ensz%d", ensz);
			if (!BIO_should_retry(ec->wbio)) {
				log_error("NNG-TLS-CONN-SEND"
					"BIO read failed rv%d", ensz);
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
				log_debug("NNG-TLS-CONN-SEND"
					"tcp%d ssl%d written%d remain%dbytes to put to kernel",
					written2tcp, read2buf, rv, dm);
				// written2tcp += written2ssl; // This may make wnext send after a long time
				// So updated way is as following.
				// Part of block of data sent failed. The return value size will not
				// contains the length of this block. So Upper layer will send again.
				// So the `wnext` will be sent next immediately.
				ec->wntcpsz = written2ssl;
				goto end;
			}
			written2tcp += written2ssl;
			// A special case before handshake finished
			if (written2tcp == 0) {
				goto end;
			}
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
	log_info("NNG-TLS-CONN-VERIFY" "verified result: %ld", rv);
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

	cfg->ctx = SSL_CTX_new(method);
	if (cfg->ctx == NULL) {
		log_error("NNG-TLS-CFG-INIT" "error in new ctx");
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
		log_debug("NNG-TLS-CFG-AUTH" "AUTH MODE: NONE");
		return (0);
	case NNG_TLS_AUTH_MODE_OPTIONAL:
		SSL_CTX_set_verify(cfg->ctx, SSL_VERIFY_PEER, NULL);
		log_debug("NNG-TLS-CFG-AUTH" "AUTH MODE: OPTION");
		return (0);
	case NNG_TLS_AUTH_MODE_REQUIRED:
		SSL_CTX_set_verify(cfg->ctx,
		    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
		log_debug("NNG-TLS-CFG-AUTH" "AUTH MODE: REQUIRE");
		return (0);
	}
	log_error("NNG-TLS-CFG-AUTH" "AUTH MODE: Unknown");
	return (NNG_EINVAL);
}

static int
open_config_ca_chain(
    nng_tls_engine_config *cfg, const char *certs, const char *crl)
{
	size_t len;
	trace("start");
	if (certs == NULL) {
		log_info("open_config_ca_chain" "NULL certs detected!");
	}
	len = strlen(certs);

	BIO *bio = BIO_new_mem_buf(certs, len);
	if (!bio) {
		log_error("NNG-TLS-CFG-CACHAIN" "Failed to create BIO");
		return (NNG_ENOMEM);
	}

	X509 *cert = NULL;
	X509_STORE *store = SSL_CTX_get_cert_store(cfg->ctx);

	while ((cert = PEM_read_bio_X509(bio, NULL, 0, NULL)) != NULL) {
		if (X509_STORE_add_cert(store, cert) == 0) {
			log_error("NNG-TLS-CFG-CACHAIN" "Failed to add certificate to store");
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
	log_warn("CRL is NOT supported yet");
	/* TODO
	len = strlen(crl);
	rv  = wolfSSL_CTX_LoadCRLBuffer(
	    cfg->ctx, (void *) crl, len, SSL_FILETYPE_PEM);
	if (rv != SSL_SUCCESS) {
	        return (NNG_ECRYPTO);
	}
	*/
#else
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
	biocert = BIO_new_mem_buf(cert, len);
	if (!biocert) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to create BIO");
		rv = NNG_ENOMEM;
		goto error;
	}
	xcert = PEM_read_bio_X509(biocert, NULL, 0, NULL);
	if (!xcert) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to load certificate from buffer");
		rv = NNG_EINVAL;
		goto error;
	}
	if (SSL_CTX_use_certificate(cfg->ctx, xcert) <= 0) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to set certificate to SSL_CTX");
		rv = NNG_EINVAL;
		goto error;
	}

	len = strlen(key);
	biokey = BIO_new_mem_buf(key, len);
	if (!biokey) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to create key BIO");
		rv = NNG_ENOMEM;
		goto error;
	}
	pkey = PEM_read_bio_PrivateKey(biokey, NULL, NULL, NULL);
	if (!pkey) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to load key from buffer");
		rv = NNG_EINVAL;
		goto error;
	}
	if (SSL_CTX_use_PrivateKey(cfg->ctx, pkey) <= 0) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to set key to SSL_CTX");
		rv = NNG_EINVAL;
		goto error;
	}

	if (SSL_CTX_check_private_key(cfg->ctx) != 1) {
		log_error("NNG-TLS-CFG-OWNCHAIN" "Failed to check key in SSL_CTX");
		rv = NNG_ECRYPTO;
		goto error;
	}

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
