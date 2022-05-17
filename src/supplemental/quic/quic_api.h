#ifndef NNG_SUPP_QUIC_API_H
#define NNG_SUPP_QUIC_API_H

extern int quic_connect(const char *url);
extern void quic_proto_open(nni_proto *proto);
extern void quic_open();

extern int quic_strm_recv(void *qstrm, nni_aio *raio);
extern int quic_strm_send(void *qstrm, nni_aio *saio);

#endif
