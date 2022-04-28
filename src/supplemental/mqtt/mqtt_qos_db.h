#ifndef NNG_MQTT_QOS_DB_H
#define NNG_MQTT_QOS_DB_H

#include "core/nng_impl.h"
#include "nng/nng.h"
#include "supplemental/sqlite/sqlite3.h"

/**
 pipe_client_table
----------------------------
|  id  | pipe_id |client_id|
----------------------------
|      |         |         |
----------------------------
**/

/**
 msg_table
---------------------
|    id   |   data  |
---------------------
|         |         |
---------------------
**/

/**
 main_table
------------------------------------------
| id | p_id  | packet_id | msg_id | qos  |
------------------------------------------
|    |       |           |        |      |
------------------------------------------
**/

/**
 client main_table
------------------------------------
| id | pipe_id  | packet_id | data |
------------------------------------
|    |          |           |      |
------------------------------------
**/

#define MQTT_DB_GET_QOS_BITS(msg) ((size_t)(msg) &0x03)
#define MQTT_DB_PACKED_MSG_QOS(msg, qos) \
	((nni_msg *) ((size_t)(msg) | ((qos) &0x03)))

#define MQTT_DB_GET_MSG_POINTER(msg) ((nni_msg *) ((size_t)(msg) & (~0x03)))

extern void     nni_mqtt_qos_db_init(sqlite3 **, const char *, bool);
extern void     nni_mqtt_qos_db_close(sqlite3 *);
extern void     nni_mqtt_qos_db_set(sqlite3 *, uint32_t, uint16_t, nni_msg *);
extern nni_msg *nni_mqtt_qos_db_get(sqlite3 *, uint32_t, uint16_t);
extern nni_msg *nni_mqtt_qos_db_get_one(sqlite3 *, uint32_t, uint16_t *);
extern void     nni_mqtt_qos_db_remove(sqlite3 *, uint32_t, uint16_t);
extern void     nni_mqtt_qos_db_remove_by_pipe(sqlite3 *, uint32_t);
extern void     nni_mqtt_qos_db_remove_msg(sqlite3 *, nni_msg *);
extern void     nni_mqtt_qos_db_remove_unused_msg(sqlite3 *);
extern void     nni_mqtt_qos_db_remove_all_msg(sqlite3 *);
extern void     nni_mqtt_qos_db_foreach(sqlite3 *, nni_idhash_cb);
extern void     nni_mqtt_qos_db_set_pipe(sqlite3 *, uint32_t, const char *);
extern void     nni_mqtt_qos_db_insert_pipe(sqlite3 *, uint32_t, const char *);
extern void     nni_mqtt_qos_db_remove_pipe(sqlite3 *, uint32_t);
extern void     nni_mqtt_qos_db_update_pipe_by_clientid(
        sqlite3 *, uint32_t, const char *);
extern void nni_mqtt_qos_db_update_all_pipe(sqlite3 *, uint32_t);
extern void nni_mqtt_qos_db_check_remove_msg(sqlite3 *, nni_msg *);

// Only work for client
extern int nni_mqtt_qos_db_set_client_msg(
    sqlite3 *, uint32_t, uint16_t, nni_msg *);
extern nni_msg *nni_mqtt_qos_db_get_client_msg(sqlite3 *, uint32_t, uint16_t);
extern void nni_mqtt_qos_db_remove_client_msg(sqlite3 *, uint32_t, uint16_t);
extern void nni_mqtt_qos_db_remove_client_msg_by_id(sqlite3 *, uint64_t);
extern nni_msg *nni_mqtt_qos_db_get_one_client_msg(
    sqlite3 *, uint64_t *, uint16_t *);
extern void nni_mqtt_qos_db_reset_client_msg_pipe_id(sqlite3 *);
#endif