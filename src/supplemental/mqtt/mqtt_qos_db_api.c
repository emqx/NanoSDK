#include "mqtt_qos_db_api.h"

int
nni_qos_db_set_client_msg(bool is_sqlite, void *db, uint32_t pipe_id,
    uint16_t packet_id, nng_msg *msg, const char *config_name,
    uint8_t proto_ver)
{
	int rv = 0;
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		rv = nni_mqtt_qos_db_set_client_msg((sqlite3 *) db, pipe_id,
		    packet_id, msg, config_name, proto_ver);
#endif
	} else {
		NNI_ARG_UNUSED(pipe_id);
		NNI_ARG_UNUSED(config_name);
		NNI_ARG_UNUSED(proto_ver);
		rv = nni_id_set((nni_id_map *) db, packet_id, msg);
	}
	return rv;
}

nng_msg *
nni_qos_db_get_client_msg(bool is_sqlite, void *db, uint32_t pipe_id,
    uint16_t packet_id, const char *config_name)
{
	nng_msg *msg = NULL;
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		msg = nni_mqtt_qos_db_get_client_msg(
		    (sqlite3 *) db, pipe_id, packet_id, config_name);
#endif
	} else {
		NNI_ARG_UNUSED(pipe_id);
		NNI_ARG_UNUSED(config_name);
		msg = nni_id_get((nni_id_map *) db, packet_id);
	}
	return msg;
}

void
nni_qos_db_remove_client_msg(
    bool is_sqlite, void *db, uint32_t pipe_id, uint16_t packet_id, const char *config_name)
{
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		nni_mqtt_qos_db_remove_client_msg(
		    (sqlite3 *) db, pipe_id, packet_id, config_name);
#endif
	} else {
		NNI_ARG_UNUSED(pipe_id);
		NNI_ARG_UNUSED(config_name);
		nni_id_remove((nni_id_map *) db, packet_id);
	}
}

void
nni_qos_db_remove_oldest_client_msg(
    bool is_sqlite, void *db, uint64_t limit, const char *config_name)
{
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		nni_mqtt_qos_db_remove_oldest_client_msg(
		    (sqlite3 *) (db), limit, config_name);
#endif
	} else {
		NNI_ARG_UNUSED(db);
		NNI_ARG_UNUSED(limit);
		NNI_ARG_UNUSED(config_name);
	}
}

void
nni_qos_db_remove_client_msg_by_id(bool is_sqlite, void *db, uint64_t row_id)
{
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		nni_mqtt_qos_db_remove_client_msg_by_id(
		    (sqlite3 *) db, row_id);
#endif
	} else {
		NNI_ARG_UNUSED(db);
		NNI_ARG_UNUSED(row_id);
	}
}

nng_msg *
nni_qos_db_get_one_client_msg(bool is_sqlite, void *db, uint64_t *row_id,
    uint16_t *packet_id, const char *config_name)
{
	nng_msg *msg = NULL;
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		msg = nni_mqtt_qos_db_get_one_client_msg(
		    (sqlite3 *) db, row_id, packet_id, config_name);
#endif
	} else {
		NNI_ARG_UNUSED(row_id);
		NNI_ARG_UNUSED(config_name);
		msg = nni_id_get_min((nni_id_map *) db, packet_id);
	}
	return msg;
}

void
nni_qos_db_reset_client_msg_pipe_id(
    bool is_sqlite, void *db, const char *config_name)
{
	if (is_sqlite) {
#ifdef NNG_SUPP_SQLITE
		nni_mqtt_qos_db_reset_client_msg_pipe_id(
		    (sqlite3 *) db, config_name);
#endif
	} else {
		NNI_ARG_UNUSED(db);
		NNI_ARG_UNUSED(config_name);
	}
}
