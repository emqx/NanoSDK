#ifndef NNG_SUPP_QUIC_API_H
#define NNG_SUPP_QUIC_API_H

#include "core/nng_impl.h"
#include "nng/nng.h"

/*
 * Note.
 *
 * qsock is the handle of a quic connection.
 * Which can NOT be used to write or read.
 *
 * qpipe is the handle of a quic stream.
 * All qpipes should be were closed before disconnecting qsock.
 */

// Enable MsQuic
extern void quic_open();
// Disable MsQuic and free
extern void quic_close();

// Enable quic protocol for nng
extern void quic_proto_open(nni_proto *proto);
// Disable quic protocol for nng
extern void quic_proto_close();
// Set global configuration for quic protocol
extern void quic_proto_set_bridge_conf(void *arg);

// Establish a quic connection to target url. Return 0 if success.
// And the handle of connection(qsock) would pass to callback .pipe_init(,qsock,)
// Or the connection is failed in eastablishing.
extern int quic_connect_ipv4(const char *url, nni_sock *sock, uint32_t *index);
// Close connection
extern int quic_disconnect(void *qsock, void *qpipe);

// Create a qpipe and open it
extern int quic_pipe_open(void *qsock, void **qpipe);
// Receive msg from a qpipe
extern int quic_pipe_recv(void *qpipe, nni_aio *raio);
// Send msg to a qpipe
extern int quic_pipe_send(void *qpipe, nni_aio *saio);
extern int quic_aio_send(void *qpipe, nni_aio *aio);
// Close a qpipe and free it
extern int quic_pipe_close(void *qpipe, uint8_t *code);

// APIs for NanoSDK
extern void quic_proto_set_keepalive(uint64_t interval);

#endif
