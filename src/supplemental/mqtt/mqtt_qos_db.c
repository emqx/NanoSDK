#include "mqtt_qos_db.h"
#include "core/nng_impl.h"
#include "nng/nng.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/sqlite/sqlite3.h"

#define table_main "t_main"
#define table_msg "t_msg"
#define table_pipe_client "t_pipe_client"
#define table_client_msg "t_client_msg"

static uint8_t *nni_msg_serialize(nni_msg *msg, size_t *out_len);
static nni_msg *nni_msg_deserialize(uint8_t *bytes, size_t len);
static uint8_t *nni_mqtt_msg_serialize(nni_msg *msg, size_t *out_len);
static nni_msg *nni_mqtt_msg_deserialize(
    uint8_t *bytes, size_t len, bool aio_available);
static int      create_msg_table(sqlite3 *db);
static int      create_pipe_client_table(sqlite3 *db);
static int      create_main_table(sqlite3 *db);

static int64_t get_id_by_msg(sqlite3 *db, nni_msg *msg);
static int64_t insert_msg(sqlite3 *db, nni_msg *msg);
static int64_t get_id_by_pipe(sqlite3 *db, uint32_t pipe_id);
static int64_t get_id_by_client_id(sqlite3 *db, const char *client_id);
static int     get_id_by_p_id(sqlite3 *db, int64_t p_id, uint16_t packet_id,
        uint8_t *out_qos, int64_t *out_m_id);
static int     insert_main(
        sqlite3 *db, int64_t p_id, uint16_t packet_id, uint8_t qos, int64_t m_id);
static int update_main(
    sqlite3 *db, int64_t p_id, uint16_t packet_id, uint8_t qos, int64_t m_id);
static void set_main(sqlite3 *db, uint32_t pipe_id, uint16_t packet_id,
    uint8_t qos, nni_msg *msg);

static int
create_client_msg_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_client_msg ""
	             " (id INTEGER PRIMARY KEY AUTOINCREMENT, "
	             "  packet_id INTEGER NOT NULL, "
	             "  pipe_id INTEGER NOT NULL, "
	             "  data BLOB)";

	return sqlite3_exec(db, sql, 0, 0, 0);
}

static int
create_msg_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_msg ""
	             " (id INTEGER PRIMARY KEY AUTOINCREMENT, "
	             "  data BLOB)";

	return sqlite3_exec(db, sql, 0, 0, 0);
}

static int
create_pipe_client_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_pipe_client ""
	             "(id INTEGER PRIMARY KEY  AUTOINCREMENT, "
	             " pipe_id    INTEGER NOT NULL, "
	             " client_id  TEXT NOT NULL)";
	return sqlite3_exec(db, sql, 0, 0, 0);
}

static int
create_main_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_main ""
	             "(id INTEGER PRIMARY KEY  AUTOINCREMENT,"
	             " p_id INTEGER NOT NULL, "
	             " packet_id INTEGER NOT NULL, "
	             " qos  TINYINT NOT NULL , "
	             " m_id INTEGER NOT NULL )";

	return sqlite3_exec(db, sql, 0, 0, 0);
}

void
nni_mqtt_qos_db_init(sqlite3 **db, const char *db_name, bool is_broker)
{
	char pwd[512]   = { 0 };
	char path[1024] = { 0 };
	if (getcwd(pwd, sizeof(pwd)) != NULL) {
		sprintf(path, "%s/%s", pwd, db_name);
		if (sqlite3_open(path, db) != 0) {
			return;
		}
		if (is_broker) {
			if (create_msg_table(*db) != 0) {
				return;
			}
			if (create_pipe_client_table(*db) != 0) {
				return;
			}
			if (create_main_table(*db) != 0) {
				return;
			}
		} else {
			if (create_client_msg_table(*db) != 0) {
				return;
			}
		}
	}
}

void
nni_mqtt_qos_db_close(sqlite3 *db)
{
	sqlite3_close(db);
}

// static char *
// bytes2Hex(uint8_t *bytes, size_t sz)
// {
// 	char *hex = nng_zalloc(sz * 2 + 1);
// 	char *p   = hex;
// 	for (size_t i = 0; i < sz; i++) {
// 		p += sprintf(p, "%.2x", bytes[i]);
// 	}
// 	return hex;
// }

static int64_t
get_id_by_msg(sqlite3 *db, nni_msg *msg)
{
	int64_t       id = 0;
	sqlite3_stmt *stmt;
	size_t        len   = 0;
	uint8_t *     blob  = nni_msg_serialize(msg, &len);
	char          sql[] = "SELECT id FROM " table_msg " where data = ?";

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_blob64(stmt, 1, blob, len, SQLITE_TRANSIENT);
	if (SQLITE_ROW == sqlite3_step(stmt)) {
		id = sqlite3_column_int64(stmt, 0);
	}

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	nng_free(blob, len);
	return id;
}

static int64_t
insert_msg(sqlite3 *db, nni_msg *msg)
{
	int64_t       id = 0;
	sqlite3_stmt *stmt;
	char *        sql = "INSERT INTO  " table_msg " (data) VALUES (?)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	size_t   len  = 0;
	uint8_t *blob = nni_msg_serialize(msg, &len);
	sqlite3_bind_blob64(stmt, 1, blob, len, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	nng_free(blob, len);
	id = sqlite3_last_insert_rowid(db);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return id;
}

static int64_t
get_id_by_pipe(sqlite3 *db, uint32_t pipe_id)
{
	int64_t       id = 0;
	sqlite3_stmt *stmt;
	char sql[] = "SELECT id FROM " table_pipe_client " WHERE pipe_id = ?";

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int64(stmt, 1, pipe_id);
	if (SQLITE_ROW == sqlite3_step(stmt)) {
		id = sqlite3_column_int64(stmt, 0);
	}

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return id;
}

static int64_t
get_id_by_client_id(sqlite3 *db, const char *client_id)
{
	int64_t       id = 0;
	sqlite3_stmt *stmt;
	char          sql[] =
	    "SELECT id FROM " table_pipe_client " WHERE client_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_text(
	    stmt, 1, client_id, strlen(client_id), SQLITE_TRANSIENT);
	if (SQLITE_ROW == sqlite3_step(stmt)) {
		id = sqlite3_column_int64(stmt, 0);
	}

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return id;
}

static int
get_id_by_p_id(sqlite3 *db, int64_t p_id, uint16_t packet_id, uint8_t *out_qos,
    int64_t *out_m_id)
{
	int64_t       id = 0;
	sqlite3_stmt *stmt;
	char          sql[] = "SELECT id, qos, m_id FROM " table_main
	             " WHERE p_id = ? AND packet_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int64(stmt, 1, p_id);
	sqlite3_bind_int(stmt, 2, packet_id);
	if (SQLITE_ROW == sqlite3_step(stmt)) {
		id        = sqlite3_column_int64(stmt, 0);
		*out_qos  = sqlite3_column_int(stmt, 1);
		*out_m_id = sqlite3_column_int64(stmt, 2);
	}

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return id;
}

static int
insert_main(
    sqlite3 *db, int64_t p_id, uint16_t packet_id, uint8_t qos, int64_t m_id)
{
	sqlite3_stmt *stmt;
	char *        sql = "INSERT INTO " table_main ""
	            " (p_id, packet_id, qos, m_id) VALUES (?, ?, ?, ?)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, p_id);
	sqlite3_bind_int(stmt, 2, packet_id);
	sqlite3_bind_int(stmt, 3, qos);
	sqlite3_bind_int64(stmt, 4, m_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

static int
update_main(
    sqlite3 *db, int64_t p_id, uint16_t packet_id, uint8_t qos, int64_t m_id)
{
	sqlite3_stmt *stmt;
	char *        sql = "UPDATE " table_main ""
	            " SET qos = ?, m_id = ? WHERE p_id = ? AND packet_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int(stmt, 1, qos);
	sqlite3_bind_int64(stmt, 2, m_id);
	sqlite3_bind_int64(stmt, 3, p_id);
	sqlite3_bind_int(stmt, 4, packet_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_insert_pipe(
    sqlite3 *db, uint32_t pipe_id, const char *client_id)
{
	sqlite3_stmt *stmt;
	char *        sql = "INSERT INTO " table_pipe_client ""
	            " (pipe_id, client_id) VALUES (?, ?)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_text(
	    stmt, 2, client_id, strlen(client_id), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_pipe(sqlite3 *db, uint32_t pipe_id)
{
	sqlite3_stmt *stmt;
	char *        sql = "DELETE FROM " table_pipe_client ""
	            " where pipe_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_update_pipe_by_clientid(
    sqlite3 *db, uint32_t pipe_id, const char *client_id)
{
	sqlite3_stmt *stmt;
	char *        sql = "UPDATE " table_pipe_client " SET pipe_id = ?"
	            " where client_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_text(
	    stmt, 2, client_id, strlen(client_id), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_set_pipe(sqlite3 *db, uint32_t pipe_id, const char *client_id)
{
	int64_t id = get_id_by_client_id(db, client_id);
	if (id == 0) {
		nni_mqtt_qos_db_insert_pipe(db, pipe_id, client_id);
	} else {
		nni_mqtt_qos_db_update_pipe_by_clientid(
		    db, pipe_id, client_id);
	}
}

void
nni_mqtt_qos_db_update_all_pipe(sqlite3 *db, uint32_t pipe_id)
{
	sqlite3_stmt *stmt;
	char *        sql = "UPDATE " table_pipe_client " SET pipe_id = ?"
	            " where id > 0";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_msg(sqlite3 *db, nni_msg *msg)
{
	sqlite3_stmt *stmt;
	char *        sql = "DELETE FROM " table_msg " WHERE data = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	size_t   len  = 0;
	uint8_t *blob = nni_msg_serialize(msg, &len);
	sqlite3_bind_blob64(stmt, 1, blob, len, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	nng_free(blob, len);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_all_msg(sqlite3 *db)
{
	char *sql = "UPDATE " table_main " SET m_id = 0 WHERE m_id > 0;"
	            "DELETE FROM " table_msg " WHERE id > 0;";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_exec(db, sql, 0, 0, NULL);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_check_remove_msg(sqlite3 *db, nni_msg *msg)
{
	sqlite3_stmt *stmt;
	// remove the msg if it was not referenced by table `t_main`
	char sql[] = "DELETE FROM " table_msg " AS msg WHERE "
	             "( SELECT COUNT(main.id) FROM " table_main " AS main  "
	             "WHERE  m_id = "
	             "( SELECT msg.id FROM t_msg "
	             "AS msg WHERE data = ? )) = 0 AND msg.data = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	size_t   len  = 0;
	uint8_t *blob = nni_msg_serialize(msg, &len);
	sqlite3_bind_blob64(stmt, 1, blob, len, SQLITE_TRANSIENT);
	sqlite3_bind_blob64(stmt, 2, blob, len, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	nng_free(blob, len);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_unused_msg(sqlite3 *db)
{
	sqlite3_stmt *stmt;
	// remove the msg if it was not referenced by table `t_main`
	char sql[] = "DELETE FROM " table_msg
	             " WHERE id NOT IN (SELECT m_id FROM t_main)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_set(
    sqlite3 *db, uint32_t pipe_id, uint16_t packet_id, nni_msg *msg)
{
	uint8_t  qos = MQTT_DB_GET_QOS_BITS(msg);
	nni_msg *m   = MQTT_DB_GET_MSG_POINTER(msg);
	set_main(db, pipe_id, packet_id, qos, m);
}

static void
set_main(sqlite3 *db, uint32_t pipe_id, uint16_t packet_id, uint8_t qos,
    nni_msg *msg)
{
	int64_t p_id = get_id_by_pipe(db, pipe_id);
	if (p_id == 0) {
		// can not find client
		return;
	}
	int64_t msg_id = get_id_by_msg(db, msg);
	if (msg_id == 0) {
		msg_id = insert_msg(db, msg);
	}
	uint8_t main_qos  = 0;
	int64_t main_m_id = 0;
	int64_t main_id =
	    get_id_by_p_id(db, p_id, packet_id, &main_qos, &main_m_id);
	if (main_id == 0) {
		insert_main(db, p_id, packet_id, qos, msg_id);
	} else {
		if (main_qos != qos || main_m_id != msg_id) {
			update_main(db, p_id, packet_id, qos, msg_id);
		}
	}
}

nni_msg *
nni_mqtt_qos_db_get(sqlite3 *db, uint32_t pipe_id, uint16_t packet_id)
{
	nni_msg *     msg = NULL;
	uint8_t       qos = 0;
	sqlite3_stmt *stmt;

	char sql[] =
	    "SELECT main.qos, msg.data FROM " table_pipe_client ""
	    " AS pipe JOIN "
	    "" table_main " AS main ON  main.p_id = pipe.id JOIN " table_msg ""
	    " AS msg ON  main.m_id = msg.id "
	    "WHERE pipe.pipe_id = ? AND main.packet_id = ?";

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_int64(stmt, 2, packet_id);
	if (SQLITE_ROW == sqlite3_step(stmt)) {
		qos            = sqlite3_column_int(stmt, 0);
		size_t   nbyte = (size_t) sqlite3_column_bytes16(stmt, 1);
		uint8_t *bytes = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 1), nbyte);

		// deserialize blob data to nni_msg
		msg = nni_msg_deserialize(bytes, nbyte);
		msg = MQTT_DB_PACKED_MSG_QOS(msg, qos);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

nni_msg *
nni_mqtt_qos_db_get_one(sqlite3 *db, uint32_t pipe_id, uint16_t *packet_id)
{
	nni_msg *     msg = NULL;
	uint8_t       qos = 0;
	sqlite3_stmt *stmt;

	char sql[] =
	    "SELECT main.packet_id, main.qos, msg.data FROM " table_pipe_client
	    " AS pipe JOIN "
	    "" table_main " AS main ON  main.p_id = pipe.id JOIN " table_msg ""
	    " AS msg ON "
	    " main.m_id = msg.id WHERE pipe.pipe_id = ? AND main.m_id > 0 "
	    "LIMIT 1";

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int(stmt, 1, pipe_id);

	if (SQLITE_ROW == sqlite3_step(stmt)) {
		*packet_id     = sqlite3_column_int64(stmt, 0);
		qos            = sqlite3_column_int(stmt, 1);
		size_t   nbyte = (size_t) sqlite3_column_bytes16(stmt, 2);
		uint8_t *bytes = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 2), nbyte);
		// deserialize blob data to nni_msg
		msg = nni_msg_deserialize(bytes, nbyte);
		msg = MQTT_DB_PACKED_MSG_QOS(msg, qos);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

void
nni_mqtt_qos_db_remove(sqlite3 *db, uint32_t pipe_id, uint16_t packet_id)
{
	sqlite3_stmt *stmt;
	char *sql = "DELETE FROM " table_main " AS main WHERE main.p_id = "
	            "(SELECT pipe.id FROM " table_pipe_client ""
	            " AS pipe where  pipe.pipe_id = ? AND packet_id = ?)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int(stmt, 1, pipe_id);
	sqlite3_bind_int(stmt, 2, packet_id);
	sqlite3_step(stmt);

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_by_pipe(sqlite3 *db, uint32_t pipe_id)
{
	sqlite3_stmt *stmt;
	char *sql = "DELETE FROM " table_main " AS main WHERE main.p_id = "
	            "(SELECT pipe.id FROM " table_pipe_client ""
	            " AS pipe where  pipe.pipe_id = ?)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int(stmt, 1, pipe_id);
	sqlite3_step(stmt);

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_foreach(sqlite3 *db, nni_idhash_cb cb)
{
	sqlite3_stmt *stmt;
	char          sql[] =
	    "SELECT pipe.pipe_id, msg.data FROM " table_main " AS main JOIN "
	    " " table_msg
	    " AS msg ON main.m_id = msg.id JOIN " table_pipe_client " "
	    " AS pipe ON main.p_id = pipe.id";

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	while (SQLITE_ROW == sqlite3_step(stmt)) {
		uint32_t pipe_id = sqlite3_column_int64(stmt, 0);
		size_t   nbyte   = (size_t) sqlite3_column_bytes16(stmt, 1);
		uint8_t *bytes   = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 1), nbyte);
		// deserialize blob data to nni_msg
		nni_msg *msg = nni_msg_deserialize(bytes, nbyte);
		cb(&pipe_id, msg);
		if (msg) {
			nni_msg_free(msg);
		}
		sqlite3_free(bytes);
	}

	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

int
nni_mqtt_qos_db_set_client_msg(
    sqlite3 *db, uint32_t pipe_id, uint16_t packet_id, nni_msg *msg)
{
	char sql[] =
	    "INSERT INTO " table_client_msg " (pipe_id, packet_id, data ) "
	    "VALUES (?, ?, ?)";
	size_t   len  = 0;
	uint8_t *blob = nni_mqtt_msg_serialize(msg, &len);
	if (!blob) {
		printf("nni_mqtt_msg_serialize failed\n");
		return -1;
	}
	sqlite3_stmt *stmt;
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int(stmt, 1, pipe_id);
	sqlite3_bind_int64(stmt, 2, packet_id);

	sqlite3_bind_blob64(stmt, 3, blob, len, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	nng_free(blob, len);
	int rv = sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	nni_msg_free(msg);
	return 0;
}

nni_msg *
nni_mqtt_qos_db_get_client_msg(sqlite3 *db, uint32_t pipe_id, uint16_t packet_id)
{
	nni_msg *     msg = NULL;
	sqlite3_stmt *stmt;

	char sql[] = "SELECT data FROM " table_client_msg ""
	             " WHERE pipe_id = ? AND packet_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_int(stmt, 2, packet_id);

	if (SQLITE_ROW == sqlite3_step(stmt)) {
		size_t   nbyte = (size_t) sqlite3_column_bytes16(stmt, 0);
		uint8_t *bytes = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 0), nbyte);
		// deserialize blob data to nni_msg
		msg = nni_mqtt_msg_deserialize(
		    bytes, nbyte, pipe_id > 0 ? true : false);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

void
nni_mqtt_qos_db_remove_client_msg(
    sqlite3 *db, uint32_t pipe_id, uint16_t packet_id)
{
	sqlite3_stmt *stmt;

	char sql[] =
	    "DELETE FROM " table_client_msg " WHERE pipe_id = ? AND packet_id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_int(stmt, 2, packet_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_client_msg_by_id(sqlite3 *db, uint64_t id)
{
	sqlite3_stmt *stmt;

	char sql[] = "DELETE FROM " table_client_msg " WHERE id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_reset_client_msg_pipe_id(sqlite3 *db)
{
	sqlite3_stmt *stmt;

	char sql[] = "UPDATE " table_client_msg " SET pipe_id = 0";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

nni_msg *
nni_mqtt_qos_db_get_one_client_msg(sqlite3 *db, uint64_t *id, uint16_t *packet_id)
{
	nni_msg *     msg = NULL;
	sqlite3_stmt *stmt;

	char sql[] =
	    "SELECT id, pipe_id, packet_id, data FROM " table_client_msg
	    " ORDER BY id LIMIT 1";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	if (SQLITE_ROW == sqlite3_step(stmt)) {
		*id              = (uint64_t) sqlite3_column_int64(stmt, 0);
		uint32_t pipe_id = sqlite3_column_int64(stmt, 1);
		*packet_id       = sqlite3_column_int(stmt, 2);
		size_t   nbyte   = (size_t) sqlite3_column_bytes16(stmt, 3);
		uint8_t *bytes   = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 3), nbyte);
		// deserialize blob data to nni_msg
		msg = nni_mqtt_msg_deserialize(
		    bytes, nbyte, pipe_id > 0 ? true : false);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

static uint8_t *
nni_mqtt_msg_serialize(nni_msg *msg, size_t *out_len)
{
	// int rv;
	// if ((rv = nni_mqtt_msg_encode(msg)) != 0) {
	// 	printf("nni_mqtt_msg_encode failed: %d\n", rv);
	// 	return NULL;
	// }
	nni_mqtt_msg_encode(msg);

	size_t len = nni_msg_header_len(msg) + nni_msg_len(msg) +
	    (sizeof(uint32_t) * 2) + sizeof(nni_time) + sizeof(nni_aio *);
	*out_len = len;

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
	// time:	nni_time(uint64)
	// aio:		address value
	uint8_t *bytes = nng_zalloc(len);

	struct pos_buf buf = { .curpos = &bytes[0], .endpos = &bytes[len] };

	if (write_uint32(nni_msg_header_len(msg), &buf) != 0) {
		goto out;
	}
	if (write_bytes(nni_msg_header(msg), nni_msg_header_len(msg), &buf) !=
	    0) {
		goto out;
	}
	if (write_uint32(nni_msg_len(msg), &buf) != 0) {
		goto out;
	}
	if (write_bytes(nni_msg_body(msg), nni_msg_len(msg), &buf) != 0) {
		goto out;
	}
	if (write_uint64(nni_msg_get_timestamp(msg), &buf) != 0) {
		goto out;
	}

	nni_aio *aio = NULL;
	if ((aio = nni_mqtt_msg_get_aio(msg)) != NULL) {
		write_uint64((uint64_t) aio, &buf);
	} else {
		write_uint64((uint64_t) 0UL, &buf);
	}

	return bytes;

out:
	free(bytes);
	return NULL;
}

static nni_msg *
nni_mqtt_msg_deserialize(uint8_t *bytes, size_t len, bool aio_available)
{
	nni_msg *msg;
	if (nni_mqtt_msg_alloc(&msg, 0) != 0) {
		return NULL;
	}

	struct pos_buf buf = { .curpos = &bytes[0], .endpos = &bytes[len] };

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
	// time:	nni_time(uint64)
	// aio:		address value
	uint32_t header_len;
	if (read_uint32(&buf, &header_len) != 0) {
		goto out;
	}
	nni_msg_header_append(msg, buf.curpos, header_len);
	buf.curpos += header_len;

	uint32_t body_len;
	if (read_uint32(&buf, &body_len) != 0) {
		goto out;
	}
	nni_msg_append(msg, buf.curpos, body_len);
	buf.curpos += body_len;

	nni_time ts = 0;
	if (read_uint64(&buf, &ts) != 0) {
		goto out;
	}
	nni_msg_set_timestamp(msg, ts);

	nni_mqtt_msg_decode(msg);

	if (aio_available) {
		uint64_t addr = 0;
		if (read_uint64(&buf, &addr) != 0) {
			goto out;
		}
		nni_mqtt_msg_set_aio(msg, (nni_aio *) addr);
	} else {
		nni_mqtt_msg_set_aio(msg, NULL);
	}

	return msg;

out:
	nni_msg_free(msg);
	return NULL;
}


static uint8_t *
nni_msg_serialize(nni_msg *msg, size_t *out_len)
{
	size_t len = nni_msg_header_len(msg) + nni_msg_len(msg) +
	    (sizeof(uint32_t) * 2) + sizeof(nni_time) + sizeof(nni_aio *);
	*out_len = len;

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
	// time:	nni_time(uint64)
	uint8_t *bytes = nng_zalloc(len);

	struct pos_buf buf = { .curpos = &bytes[0], .endpos = &bytes[len] };

	if (write_uint32(nni_msg_header_len(msg), &buf) != 0) {
		goto out;
	}
	if (write_bytes(nni_msg_header(msg), nni_msg_header_len(msg), &buf) !=
	    0) {
		goto out;
	}
	if (write_uint32(nni_msg_len(msg), &buf) != 0) {
		goto out;
	}
	if (write_bytes(nni_msg_body(msg), nni_msg_len(msg), &buf) != 0) {
		goto out;
	}
	if (write_uint64(nni_msg_get_timestamp(msg), &buf) != 0) {
		goto out;
	}

	return bytes;

out:
	free(bytes);
	return NULL;
}

static nni_msg *
nni_msg_deserialize(uint8_t *bytes, size_t len)
{
	nni_msg *msg;
	if (nni_msg_alloc(&msg, 0) != 0) {
		return NULL;
	}

	struct pos_buf buf = { .curpos = &bytes[0], .endpos = &bytes[len] };

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
	// time:	nni_time(uint64)
	uint32_t header_len;
	if (read_uint32(&buf, &header_len) != 0) {
		goto out;
	}
	nni_msg_header_append(msg, buf.curpos, header_len);
	buf.curpos += header_len;

	uint32_t body_len;
	if (read_uint32(&buf, &body_len) != 0) {
		goto out;
	}
	nni_msg_append(msg, buf.curpos, body_len);
	buf.curpos += body_len;

	nni_time ts = 0;
	if (read_uint64(&buf, &ts) != 0) {
		goto out;
	}
	nni_msg_set_timestamp(msg, ts);

	return msg;

out:
	nni_msg_free(msg);
	return NULL;
}
