#ifndef NNG_SUPP_QUIC_API_H
#define NNG_SUPP_QUIC_API_H

#include "core/nng_impl.h"
#include "nng/nng.h"

extern int quic_connect_ipv4(const char *url, nni_sock *sock);
extern int quic_disconnect();

extern void quic_open();
extern void quic_close();

extern void quic_proto_open(nni_proto *proto);
extern void quic_proto_close();

extern void quic_proto_set_keepalive(uint64_t interval);

extern int quic_strm_recv(void *arg, nni_aio *raio);
extern int quic_strm_send(void *arg, nni_aio *saio);
extern int quic_strm_close(void *arg);
extern int quic_pipe_close(uint8_t *code);

extern int quic_pipe_close(uint8_t *code);

#endif
