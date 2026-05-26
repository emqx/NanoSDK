#ifndef NNG_SUPP_QUIC_API_H
#define NNG_SUPP_QUIC_API_H

#define QUIC_HIGH_PRIOR_MSG (0x80)
#define QUIC_MULTISTREAM_FLAGS (0x7F)
#define QUIC_CLOSE_REOPEN_FLAGS (0x100)

#define QUIC_MAIN_STREAM (1)
#define QUIC_SUB_STREAM (0)
#define QUIC_SUB_STREAM_NUM (4)

#define QUIC_SUB_STREAM_TIMEOUT (5000) // 5s
#define QUIC_IDLE_TIMEOUT_DEFAULT (90)
#define QUIC_KEEPALIVE_DEFAULT (60)

#include "core/nng_impl.h"
#include "nng/nng.h"
#include "msquic.h"

typedef struct quic_dialer quic_dialer;
typedef struct quic_listener quic_listener;

extern int nni_quic_dialer_alloc(nng_stream_dialer **, const nni_url *);
extern int nni_quic_listener_alloc(nng_stream_listener **, const nni_url *);

typedef struct nni_quic_dialer nni_quic_dialer;

extern int  nni_quic_dialer_init(void **);
extern void nni_quic_dialer_fini(nni_quic_dialer *d);
extern void nni_quic_dial(void *, const char *, const char *, nni_aio *);
extern void nni_quic_dialer_close(void *);

typedef struct nni_quic_listener nni_quic_listener;

extern int  nni_quic_listener_init(void **);
extern void nni_quic_listener_listen(nni_quic_listener *, const char *, const char *);
extern void nni_quic_listener_accept(nni_quic_listener *, nng_aio *aio);

typedef struct nni_quic_conn nni_quic_conn;
// When multi stream is enabled
typedef struct ex_quic_conn ex_quic_conn;

extern int  nni_msquic_quic_alloc(nni_quic_conn **, nni_quic_dialer *);
extern void nni_msquic_quic_init(nni_quic_conn *);
extern void nni_msquic_quic_start(nni_quic_conn *, int, int);
extern void nni_msquic_quic_dialer_rele(nni_quic_dialer *);

struct nni_quic_dialer {
	nni_aio                *qconaio; // for quic connection
	nni_quic_conn          *currcon; // a var to record conn context for one dial action
	nni_list                connq;   // pending connections/quic streams
	bool                    closed;
	bool                    nodelay;
	bool                    keepalive;
	struct sockaddr_storage src;
	size_t                  srclen;
	nni_mtx                 mtx;
	nni_atomic_u64          ref;
	nni_atomic_bool         fini;

	// MsQuic
	HQUIC    qconn; // quic connection
	bool     enable_0rtt;
	bool     enable_mltstrm;
	uint8_t  reason_code;
	// ResumptionTicket
	char      rticket[4096]; // Ususally it would be within 4096.
	                         // But in msquic. The maximum size is 65535.
	uint16_t  rticket_sz;
	// CertificateFile
	char *    cacert;
	char *    key;
	char *    password;
	bool      verify_peer;
	char *    ca;

	// Quic settings
	uint64_t  qidle_timeout;
	uint32_t  qkeepalive;
	uint64_t  qconnect_timeout;
	uint32_t  qdiscon_timeout;
	uint32_t  qsend_idle_timeout;
	uint32_t  qinitial_rtt_ms;
	uint32_t  qmax_ack_delay_ms;
	int       priority;
	QUIC_SETTINGS settings;
};

struct nni_quic_listener {
	nni_mtx                 mtx;
	nni_atomic_u64          ref;
	nni_atomic_bool         fini;
	bool                    closed;
	bool                    started;
	nni_list                acceptq;
	nni_list                incomings;

	// MsQuic
	HQUIC    ql; // Quic Listener

	// Quic Settings
	bool     enable_0rtt;
	bool     enable_mltstrm;

	QUIC_SETTINGS settings;
};
#endif
