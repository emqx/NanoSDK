#ifndef NNG_MQTT_QOS_DB_H
#define NNG_MQTT_QOS_DB_H

#include "core/nng_impl.h"
#include "nng/nng.h"
#include "supplemental/sqlite/sqlite3.h"


/**
 *
 *  client msg_table
-----------------------------------------------------------------
| id | pipe_id  | packet_id | data | ts   | info_id | proto_ver |
-----------------------------------------------------------------
|    |          |           |      |      |         |           |
-----------------------------------------------------------------
**/

/**
 *
 * client offline_msg_table
-----------------------------------------------
| id    | data | ts   | info_id   | proto_ver |
-----------------------------------------------
|       |      |      |           |           |
-----------------------------------------------
**/

/**
 * 
 * client info_table
-------------------------------------------------------------------
| id    | config_name | client_id | proto_name  | proto_ver | ts  |
-------------------------------------------------------------------
|       |             |           |             |           |     |
-------------------------------------------------------------------
**/

struct nng_mqtt_sqlite_option {
	bool    enable;
	uint8_t mqtt_version;      // mqtt version
	size_t  disk_cache_size;   // specify the max rows of sqlite table
	char *  mounted_file_path; // specify the db file path
	char *  db_name;
	size_t
	    flush_mem_threshold; // flush to sqlite table when count of message
	                         // is equal or greater than this value
	nni_lmq offline_cache;
#if defined(NNG_SUPP_SQLITE)
	sqlite3 *db;
#else
	void *db;
#endif
};

typedef struct nng_mqtt_sqlite_option nni_mqtt_sqlite_option;

#define MQTT_DB_GET_QOS_BITS(msg) ((size_t)(msg) &0x03)
#define MQTT_DB_PACKED_MSG_QOS(msg, qos) \
	((nni_msg *) ((size_t)(msg) | ((qos) &0x03)))

#define MQTT_DB_GET_MSG_POINTER(msg) ((nni_msg *) ((size_t)(msg) & (~0x03)))

extern void nni_mqtt_qos_db_init(sqlite3 **, const char *, const char *);
extern void nni_mqtt_qos_db_close(sqlite3 *);

extern void nni_mqtt_qos_db_remove_oldest_client_msg(
    sqlite3 *, uint64_t ,const char *);
extern void nni_mqtt_qos_db_remove_oldest_client_offline_msg(
    sqlite3 *, uint64_t ,const char *);
// Only work for client
extern int nni_mqtt_qos_db_set_client_msg(
    sqlite3 *, uint32_t, uint16_t, nni_msg *, const char *, uint8_t);
extern nni_msg *nni_mqtt_qos_db_get_client_msg(
    sqlite3 *, uint32_t, uint16_t, const char *);
extern size_t nni_mqtt_qos_db_get_client_offline_msg_count(
    sqlite3 *, const char *);
extern void nni_mqtt_qos_db_remove_client_msg(
    sqlite3 *, uint32_t, uint16_t, const char *);
extern void nni_mqtt_qos_db_remove_client_msg_by_id(sqlite3 *, uint64_t);
extern nni_msg *nni_mqtt_qos_db_get_one_client_msg(
    sqlite3 *, uint64_t *, uint16_t *, const char *);
extern void nni_mqtt_qos_db_reset_client_msg_pipe_id(sqlite3 *,const char *);

extern int nni_mqtt_qos_db_set_client_offline_msg(
    sqlite3 *, nni_msg *, const char *, uint8_t);
extern int nni_mqtt_qos_db_set_client_offline_msg_batch(
    sqlite3 *, nni_lmq *, const char *, uint8_t);
extern nng_msg *nni_mqtt_qos_db_get_client_offline_msg(sqlite3 *, int64_t *,const char *);
extern int      nni_mqtt_qos_db_remove_client_offline_msg(sqlite3 *, int64_t);
extern int      nni_mqtt_qos_db_remove_all_client_offline_msg(sqlite3 *,const char *);

extern int nni_mqtt_qos_db_set_client_info(
    sqlite3 *, const char *, const char *, const char *, uint8_t);

extern void nni_mqtt_sqlite_db_init(
    nni_mqtt_sqlite_option *, const char *, uint8_t);
extern void nni_mqtt_sqlite_db_fini(nni_mqtt_sqlite_option *);
extern size_t nni_mqtt_sqlite_db_get_cached_size(nni_mqtt_sqlite_option *);

#endif