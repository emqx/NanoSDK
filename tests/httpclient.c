//
// Copyright 2026 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// Basic HTTP client tests, adapted from upstream NNG #2257:
// https://github.com/nanomsg/nng/commit/9aa948d9aa8c19a3e2d808ae701a9b246dd950ff
// Keep NanoSDK's HTTP v1 API and the nng.httpclient CTest name. All peers
// run on loopback with ephemeral ports; no external web service is required.

#include <nng/nng.h>
#include <nng/supplemental/http/http.h>

#include "testing/nuts.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scripted_server {
	nng_stream_listener *listener;
	nng_thread          *thread;
	const char         **responses;
	int                 *delays;
	size_t               nresponses;
	size_t               requests;
	int                  rv;
	bool                 ignore_send_errors;
	char                 url[128];
};

static int
stream_recv_some(nng_stream *stream, nng_aio *aio, void *buf, size_t len,
    size_t *count)
{
	nng_iov iov;
	int     rv;

	iov.iov_buf = buf;
	iov.iov_len = len;
	if ((rv = nng_aio_set_iov(aio, 1, &iov)) != 0) {
		return (rv);
	}
	nng_stream_recv(stream, aio);
	nng_aio_wait(aio);
	if ((rv = nng_aio_result(aio)) != 0) {
		return (rv);
	}
	*count = nng_aio_count(aio);
	return (0);
}

static int
server_read_request(nng_stream *stream, nng_aio *aio)
{
	char   buf[4097];
	size_t used = 0;
	size_t n;
	int    rv;

	for (;;) {
		if (used >= sizeof(buf) - 1) {
			return (NNG_EMSGSIZE);
		}
		rv = stream_recv_some(stream, aio, buf + used,
		    sizeof(buf) - 1 - used, &n);
		if (rv != 0) {
			return (rv);
		}
		if (n == 0) {
			return (NNG_ECLOSED);
		}
		used += n;
		if (used >= 4) {
			buf[used] = '\0';
			if (strstr(buf, "\r\n\r\n") != NULL) {
				return (0);
			}
		}
	}
}

static int
server_send_all(nng_stream *stream, nng_aio *aio, const char *data)
{
	size_t len = strlen(data);
	size_t off = 0;

	while (off < len) {
		nng_iov iov;
		int     rv;

		iov.iov_buf = (void *) (data + off);
		iov.iov_len = len - off;
		if ((rv = nng_aio_set_iov(aio, 1, &iov)) != 0) {
			return (rv);
		}
		nng_stream_send(stream, aio);
		nng_aio_wait(aio);
		if ((rv = nng_aio_result(aio)) != 0) {
			return (rv);
		}
		if (nng_aio_count(aio) == 0) {
			return (NNG_ECLOSED);
		}
		off += nng_aio_count(aio);
	}
	return (0);
}

static void
scripted_server_main(void *arg)
{
	struct scripted_server *srv = arg;
	nng_aio                *aio = NULL;
	nng_stream             *stream;
	int                     rv;

	if ((rv = nng_aio_alloc(&aio, NULL, NULL)) != 0) {
		srv->rv = rv;
		return;
	}
	nng_aio_set_timeout(aio, 5000);
	nng_stream_listener_accept(srv->listener, aio);
	nng_aio_wait(aio);
	if ((rv = nng_aio_result(aio)) != 0) {
		nng_aio_free(aio);
		srv->rv = rv;
		return;
	}
	stream = nng_aio_get_output(aio, 0);

	for (size_t i = 0; i < srv->nresponses; i++) {
		if ((rv = server_read_request(stream, aio)) != 0) {
			srv->rv = rv;
			break;
		}
		srv->requests++;
		if ((srv->delays != NULL) && (srv->delays[i] > 0)) {
			nng_msleep(srv->delays[i]);
		}
		rv = server_send_all(stream, aio, srv->responses[i]);
		if (rv != 0) {
			if (!srv->ignore_send_errors) {
				srv->rv = rv;
			}
			break;
		}
	}

	nng_stream_close(stream);
	nng_stream_free(stream);
	nng_aio_free(aio);
}

static void
scripted_server_start(struct scripted_server *srv, const char **responses,
    int *delays, size_t nresponses, bool ignore_send_errors)
{
	int port;

	memset(srv, 0, sizeof(*srv));
	srv->responses  = responses;
	srv->delays     = delays;
	srv->nresponses = nresponses;
	srv->ignore_send_errors = ignore_send_errors;

	NUTS_PASS(nng_stream_listener_alloc(
	    &srv->listener, "tcp://127.0.0.1:0"));
	NUTS_PASS(nng_stream_listener_listen(srv->listener));
	NUTS_PASS(
	    nng_stream_listener_get_int(srv->listener, NNG_OPT_TCP_BOUND_PORT, &port));
	snprintf(srv->url, sizeof(srv->url), "http://127.0.0.1:%d", port);
	NUTS_PASS(nng_thread_create(&srv->thread, scripted_server_main, srv));
}

static void
scripted_server_stop(struct scripted_server *srv)
{
	if (srv->listener != NULL) {
		nng_stream_listener_close(srv->listener);
	}
	if (srv->thread != NULL) {
		nng_thread_destroy(srv->thread);
	}
	if (srv->listener != NULL) {
		nng_stream_listener_free(srv->listener);
	}
}

static void
client_alloc(const char *urlstr, nng_url **urlp, nng_aio **aiop,
    nng_http_client **clip)
{
	NUTS_PASS(nng_url_parse(urlp, urlstr));
	NUTS_PASS(nng_aio_alloc(aiop, NULL, NULL));
	nng_aio_set_timeout(*aiop, 5000);
	NUTS_PASS(nng_http_client_alloc(clip, *urlp));
}

static void
client_connect(const char *urlstr, nng_url **urlp, nng_aio **aiop,
    nng_http_client **clip, nng_http_conn **connp)
{
	client_alloc(urlstr, urlp, aiop, clip);
	nng_http_client_connect(*clip, *aiop);
	nng_aio_wait(*aiop);
	NUTS_PASS(nng_aio_result(*aiop));
	*connp = nng_aio_get_output(*aiop, 0);
	NUTS_TRUE(*connp != NULL);
}

static void
client_free(nng_url *url, nng_aio *aio, nng_http_client *cli, nng_http_conn *conn)
{
	if (conn != NULL) {
		nng_http_conn_close(conn);
	}
	if (cli != NULL) {
		nng_http_client_free(cli);
	}
	if (aio != NULL) {
		nng_aio_free(aio);
	}
	if (url != NULL) {
		nng_url_free(url);
	}
}

static void
test_http_client_request_response(void)
{
	static const char *response =
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Type: application/json\r\n"
	    "Content-Length: 14\r\n"
	    "Connection: keep-alive\r\n"
	    "\r\n"
	    "{\"ok\": true}\r\n";
	struct scripted_server srv;
	nng_aio               *aio = NULL;
	nng_http_client       *cli = NULL;
	nng_http_conn         *conn = NULL;
	nng_url               *url = NULL;
	nng_http_req          *req = NULL;
	nng_http_res          *res = NULL;
	char                   body[16];
	nng_iov                iov;
	const char            *ptr;

	scripted_server_start(&srv, &response, NULL, 1, false);
	client_connect(srv.url, &url, &aio, &cli, &conn);
	NUTS_PASS(nng_http_req_alloc(&req, url));
	NUTS_PASS(nng_http_res_alloc(&res));

	NUTS_PASS(nng_http_req_set_uri(req, "/get"));
	nng_http_conn_write_req(conn, req, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));

	nng_http_conn_read_res(conn, res, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_http_res_get_status(res) == NNG_HTTP_STATUS_OK);
	ptr = nng_http_res_get_header(res, "Content-Length");
	NUTS_TRUE(ptr != NULL);
	NUTS_TRUE(atoi(ptr) == 14);

	memset(body, 0, sizeof(body));
	iov.iov_buf = body;
	iov.iov_len = 14;
	NUTS_PASS(nng_aio_set_iov(aio, 1, &iov));
	nng_http_conn_read_all(conn, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_aio_count(aio) == 14);
	NUTS_MATCH(body, "{\"ok\": true}\r\n");

	nng_http_req_free(req);
	nng_http_res_free(res);
	client_free(url, aio, cli, conn);
	scripted_server_stop(&srv);
	NUTS_PASS(srv.rv);
	NUTS_TRUE(srv.requests == 1);
}

static void
test_http_client_transact(void)
{
	static const char *response =
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Type: text/plain\r\n"
	    "Content-Length: 11\r\n"
	    "Connection: keep-alive\r\n"
	    "\r\n"
	    "hello nng\r\n";
	struct scripted_server srv;
	nng_aio               *aio = NULL;
	nng_http_client       *cli = NULL;
	nng_http_conn         *conn = NULL;
	nng_url               *url = NULL;
	nng_http_req          *req = NULL;
	nng_http_res          *res = NULL;
	void                  *data;
	size_t                 len;

	scripted_server_start(&srv, &response, NULL, 1, false);
	client_alloc(srv.url, &url, &aio, &cli);
	NUTS_PASS(nng_http_req_alloc(&req, url));
	NUTS_PASS(nng_http_res_alloc(&res));

	NUTS_PASS(nng_http_req_set_uri(req, "/"));
	nng_http_client_transact(cli, req, res, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_http_res_get_status(res) == NNG_HTTP_STATUS_OK);
	nng_http_res_get_data(res, &data, &len);
	NUTS_TRUE(len == 11);
	NUTS_TRUE(memcmp(data, "hello nng\r\n", len) == 0);

	nng_http_req_free(req);
	nng_http_res_free(res);
	client_free(url, aio, cli, conn);
	scripted_server_stop(&srv);
	NUTS_PASS(srv.rv);
	NUTS_TRUE(srv.requests == 1);
}

static void
test_http_client_reuse(void)
{
	static const char *responses[] = {
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: 5\r\n"
		"Connection: keep-alive\r\n"
		"\r\n"
		"one\r\n",
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: 5\r\n"
		"Connection: keep-alive\r\n"
		"\r\n"
		"two\r\n",
	};
	struct scripted_server srv;
	nng_aio               *aio = NULL;
	nng_http_client       *cli = NULL;
	nng_http_conn         *conn = NULL;
	nng_url               *url = NULL;
	nng_http_req          *req = NULL;
	nng_http_res          *res = NULL;
	void                  *data;
	size_t                 len;

	scripted_server_start(&srv, responses, NULL, 2, false);
	client_connect(srv.url, &url, &aio, &cli, &conn);
	NUTS_PASS(nng_http_req_alloc(&req, url));
	NUTS_PASS(nng_http_res_alloc(&res));

	NUTS_PASS(nng_http_req_set_uri(req, "/"));
	nng_http_conn_transact(conn, req, res, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_http_res_get_status(res) == NNG_HTTP_STATUS_OK);
	nng_http_res_get_data(res, &data, &len);
	NUTS_TRUE(len == 5);
	NUTS_TRUE(memcmp(data, "one\r\n", len) == 0);

	nng_http_res_reset(res);
	NUTS_PASS(nng_http_req_set_uri(req, "/"));
	nng_http_conn_transact(conn, req, res, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_http_res_get_status(res) == NNG_HTTP_STATUS_OK);
	nng_http_res_get_data(res, &data, &len);
	NUTS_TRUE(len == 5);
	NUTS_TRUE(memcmp(data, "two\r\n", len) == 0);

	nng_http_req_free(req);
	nng_http_res_free(res);
	client_free(url, aio, cli, conn);
	scripted_server_stop(&srv);
	NUTS_PASS(srv.rv);
	NUTS_TRUE(srv.requests == 2);
}

static void
test_http_client_timeout(void)
{
	static const char *response =
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Length: 6\r\n"
	    "\r\n"
	    "late\r\n";
	struct scripted_server srv;
	nng_aio               *aio = NULL;
	nng_http_client       *cli = NULL;
	nng_http_conn         *conn = NULL;
	nng_url               *url = NULL;
	nng_http_req          *req = NULL;
	nng_http_res          *res = NULL;
	int                    delay = 200;

	scripted_server_start(&srv, &response, &delay, 1, true);
	client_connect(srv.url, &url, &aio, &cli, &conn);
	NUTS_PASS(nng_http_req_alloc(&req, url));
	NUTS_PASS(nng_http_res_alloc(&res));

	nng_aio_set_timeout(aio, 20);
	NUTS_PASS(nng_http_req_set_uri(req, "/delay"));
	nng_http_conn_transact(conn, req, res, aio);
	nng_aio_wait(aio);
	NUTS_TRUE(nng_aio_result(aio) == NNG_ETIMEDOUT);

	nng_http_req_free(req);
	nng_http_res_free(res);
	client_free(url, aio, cli, conn);
	scripted_server_stop(&srv);
	NUTS_TRUE(srv.requests == 1);
}

static void
test_http_client_chunked(void)
{
	static const char *response =
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Type: text/plain\r\n"
	    "Transfer-Encoding: chunked\r\n"
	    "Connection: keep-alive\r\n"
	    "\r\n"
	    "5\r\nhello\r\n"
	    "6\r\n world\r\n"
	    "0\r\n\r\n";
	struct scripted_server srv;
	nng_aio               *aio = NULL;
	nng_http_client       *cli = NULL;
	nng_http_conn         *conn = NULL;
	nng_url               *url = NULL;
	nng_http_req          *req = NULL;
	nng_http_res          *res = NULL;
	void                  *data;
	size_t                 len;

	scripted_server_start(&srv, &response, NULL, 1, false);
	client_connect(srv.url, &url, &aio, &cli, &conn);
	NUTS_PASS(nng_http_req_alloc(&req, url));
	NUTS_PASS(nng_http_res_alloc(&res));

	NUTS_PASS(nng_http_req_set_uri(req, "/chunked"));
	nng_http_conn_transact(conn, req, res, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_http_res_get_status(res) == NNG_HTTP_STATUS_OK);
	nng_http_res_get_data(res, &data, &len);
	NUTS_TRUE(len == 11);
	NUTS_TRUE(memcmp(data, "hello world", len) == 0);

	nng_http_req_free(req);
	nng_http_res_free(res);
	client_free(url, aio, cli, conn);
	scripted_server_stop(&srv);
	NUTS_PASS(srv.rv);
	NUTS_TRUE(srv.requests == 1);
}

NUTS_TESTS = {
	{ "http client request response", test_http_client_request_response },
	{ "http client transact", test_http_client_transact },
	{ "http client reuse", test_http_client_reuse },
	{ "http client timeout", test_http_client_timeout },
	{ "http client chunked", test_http_client_chunked },
	{ NULL, NULL },
};
