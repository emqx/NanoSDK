#ifndef SQLITE_HANDLER_H
#define SQLITE_HANDLER_H

#include "core/nng_impl.h"
#include "nng/nng.h"
#include "supplemental/mqtt/mqtt_qos_db.h"

extern bool     sqlite_is_enabled(nni_mqtt_sqlite_option *);
extern nni_msg *sqlite_get_cache_msg(nni_mqtt_sqlite_option *);
extern void     sqlite_flush_lmq(nni_mqtt_sqlite_option *, nni_lmq *);
extern void     sqlite_flush_offline_cache(nni_mqtt_sqlite_option *);

#endif
