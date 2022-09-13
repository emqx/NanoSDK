#ifndef NNG_MQTT_QOS_DB_API_H
#define NNG_MQTT_QOS_DB_API_H

#include "nng/nng.h"
#include "core/nng_impl.h"
#include "mqtt_qos_db.h"

#define nni_qos_db_init_sqlite(db, user_path, db_name, is_broker) \
	nni_mqtt_qos_db_init((sqlite3 **) &(db), user_path, db_name, is_broker)
#define nni_qos_db_fini_sqlite(db) nni_mqtt_qos_db_close((sqlite3 *) (db))

#define nni_qos_db_init_id_hash(db)                              \
	{                                                        \
		db = nng_zalloc(sizeof(nni_id_map));             \
		nni_id_map_init((nni_id_map *) db, 0, 0, false); \
	}
#define nni_qos_db_fini_id_hash(db)                                \
	{                                                          \
		nni_id_map_fini((nni_id_map *) (db));              \
		nni_free((nni_id_map *) (db), sizeof(nni_id_map)); \
	}

#define nni_qos_db_init_id_hash_with_opt(db, lo, hi, randomize)        \
	{                                                              \
		db = nng_zalloc(sizeof(nni_id_map));                   \
		nni_id_map_init((nni_id_map *) db, lo, hi, randomize); \
	}

extern int      nni_qos_db_set_client_msg(bool is_sqlite, void *db,
         uint32_t pipe_id, uint16_t packet_id, nng_msg *msg,
         const char *config_name, uint8_t proto_ver);
extern nng_msg *nni_qos_db_get_client_msg(
    bool is_sqlite, void *db, uint32_t pipe_id, uint16_t packet_id,
    const char *config_name);
extern void nni_qos_db_remove_client_msg(bool is_sqlite, void *db,
    uint32_t pipe_id, uint16_t packet_id, const char *config_name);
extern void nni_qos_db_remove_oldest_client_msg(
    bool is_sqlite, void *db, uint64_t limit, const char *config_name);
extern void nni_qos_db_remove_client_msg_by_id(
    bool is_sqlite, void *db, uint64_t row_id);
extern nng_msg *nni_qos_db_get_one_client_msg(bool is_sqlite, void *db,
    uint64_t *row_id, uint16_t *packet_id, const char *config_name);
extern void nni_qos_db_reset_client_msg_pipe_id(
    bool is_sqlite, void *db, const char *config_name);

#endif
