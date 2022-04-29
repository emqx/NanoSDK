#ifndef NNG_MQTT_QOS_DB_API_H
#define NNG_MQTT_QOS_DB_API_H

#include "core/nng_impl.h"
#include "mqtt_qos_db.h"

#define nni_qos_db_init_sqlite(db, db_name, is_broker) \
	nni_mqtt_qos_db_init((sqlite3 **) &(db), db_name, is_broker)
#define nni_qos_db_fini_sqlite(db) nni_mqtt_qos_db_close((sqlite3 *) (db))

#define nni_qos_db_init_id_hash(db)                              \
	{                                                        \
		db = nng_alloc(sizeof(nni_id_map));              \
		nni_id_map_init((nni_id_map *) db, 0, 0, false); \
	}
#define nni_qos_db_fini_id_hash(db)                                \
	{                                                          \
		nni_id_map_fini((nni_id_map *) (db));              \
		nni_free((nni_id_map *) (db), sizeof(nni_id_map)); \
	}

#ifdef NNG_SUPP_SQLITE

#define nni_qos_db_set_client_msg(db, pipe_id, packet_id, msg) \
	nni_mqtt_qos_db_set_client_msg((sqlite3 *) db, pipe_id, packet_id, msg)
#define nni_qos_db_get_client_msg(db, pipe_id, packet_id) \
	nni_mqtt_qos_db_get_client_msg((sqlite3 *) db, pipe_id, packet_id)
#define nni_qos_db_remove_client_msg(db, pipe_id, packet_id) \
	nni_mqtt_qos_db_remove_client_msg((sqlite3 *)db, pipe_id, packet_id)
#define nni_qos_db_remove_client_msg_by_id(db, id) \
	nni_mqtt_qos_db_remove_client_msg_by_id((sqlite3 *)db, id)
#define nni_qos_db_get_one_client_msg(db, id, packet_id) \
	nni_mqtt_qos_db_get_one_client_msg((sqlite3 *)db, &id, &packet_id)
#define nni_qos_db_reset_client_msg_pipe_id(db) \
	nni_mqtt_qos_db_reset_client_msg_pipe_id((sqlite3 *) db)

#else

#define nni_qos_db_set_client_msg(db, pipe_id, packet_id, msg) \
	nni_id_set((nni_id_map *) &db, packet_id, msg)
#define nni_qos_db_get_client_msg(db, pipe_id, packet_id) \
	nni_id_get((nni_id_map *) &db, packet_id)
#define nni_qos_db_remove_client_msg(db, pipe_id, packet_id) \
	nni_id_remove((nni_id_map *) &db, packet_id)
#define nni_qos_db_remove_client_msg_by_id(db, id)
#define nni_qos_db_get_one_client_msg(db, id, packet_id) \
	nni_id_get_any((nni_id_map *) &db, &packet_id)
#define nni_qos_db_reset_client_msg_pipe_id(db)

#endif

#endif
