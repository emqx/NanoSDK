#include "mqtt_qos_db.h"
#include "core/nng_impl.h"
#include "nng/nng.h"
#include "supplemental/sqlite/sqlite3.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define table_client_msg "t_client_msg"
#define table_client_offline_msg "t_client_offline_msg"
#define table_client_info "t_client_info"

static uint8_t *nni_mqtt_msg_serialize(
    nni_msg *msg, size_t *out_len, uint8_t proto_ver);
static nni_msg *nni_mqtt_msg_deserialize(
    uint8_t *bytes, size_t len, bool aio_available, uint8_t proto_ver);

static int      create_client_msg_table(sqlite3 *db);
static int      create_client_offline_msg_table(sqlite3 *db);
static int      create_client_info_table(sqlite3 *db);
static char *   get_db_path(
       char *dest_path, const char *user_path, const char *db_name);
static void    set_db_pragma(sqlite3 *db);
static void    remove_oldest_client_msg(sqlite3 *db, const char *table_name,
       const char *col_name, uint64_t limit, const char *config_name);
static int     get_client_info_id(sqlite3 *db, const char *config_name);


static int
create_client_msg_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_client_msg ""
	             " (id INTEGER PRIMARY KEY AUTOINCREMENT, "
	             "  packet_id INTEGER NOT NULL, "
	             "  pipe_id INTEGER NOT NULL, "
	             "  data BLOB, "
	             "  info_id INTEGER NOT NULL,"
				 "  proto_ver TINYINT DEFAULT 4,"
	             "  ts DATETIME DEFAULT CURRENT_TIMESTAMP )";

	return sqlite3_exec(db, sql, 0, 0, 0);
}

static int
create_client_offline_msg_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_client_offline_msg ""
	             " (id INTEGER PRIMARY KEY AUTOINCREMENT, "
	             "  data BLOB, "
	             "  info_id INTEGER NOT NULL, "
				 "  proto_ver TINYINT DEFAULT 4, "
	             "  ts DATETIME DEFAULT CURRENT_TIMESTAMP )";

	return sqlite3_exec(db, sql, 0, 0, 0);
}

static int
create_client_info_table(sqlite3 *db)
{
	char sql[] = "CREATE TABLE IF NOT EXISTS " table_client_info ""
	             " (id INTEGER PRIMARY KEY AUTOINCREMENT, "
	             "  config_name TEXT NOT NULL UNIQUE, "
	             "  client_id TEXT , "
	             "  proto_name TEXT , "
	             "  proto_ver TINY INT , "
	             "  ts DATETIME DEFAULT CURRENT_TIMESTAMP )";

	return sqlite3_exec(db, sql, 0, 0, 0);
}

static void
set_db_pragma(sqlite3 *db)
{
	sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, 0, 0);
	sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, 0, 0);
	sqlite3_exec(db, "PRAGMA wal_autocheckpoint", NULL, 0, 0);
}

static char *
get_db_path(char *dest_path, const char *user_path, const char *db_name)
{
	if (user_path == NULL) {
		char pwd[512] = { 0 };
		if (nni_plat_getcwd(pwd, sizeof(pwd)) != NULL) {
			sprintf(dest_path, "%s/%s", pwd, db_name);
		} else {
			return NULL;
		}
	} else {
		if (user_path[strlen(user_path) - 1] == '/') {
			sprintf(dest_path, "%s%s", user_path, db_name);
		} else {
			sprintf(dest_path, "%s/%s", user_path, db_name);
		}
	}

	return dest_path;
}

void
nni_mqtt_qos_db_init(sqlite3 **db, const char *user_path, const char *db_name)
{
	char db_path[1024] = { 0 };

	int rv = 0;

	if (NULL != get_db_path(db_path, user_path, db_name) &&
	    ((rv = sqlite3_open(db_path, db)) != 0)) {
		nni_panic("Can't open database %s: %s\n", db_path,
		    sqlite3_errmsg(*db));
		return;
	}
	set_db_pragma(*db);

	if (create_client_msg_table(*db) != 0) {
		return;
	}
	if (create_client_offline_msg_table(*db) != 0) {
		return;
	}
	if (create_client_info_table(*db) != 0) {
		return;
	}
}

void
nni_mqtt_qos_db_close(sqlite3 *db)
{
	sqlite3_close(db);
}

int
nni_mqtt_qos_db_set_client_msg(sqlite3 *db, uint32_t pipe_id,
    uint16_t packet_id, nni_msg *msg, const char *config_name, uint8_t proto_ver)
{
	char sql[] =
	    "INSERT INTO " table_client_msg
	    " ( pipe_id, packet_id, data, proto_ver, info_id ) "
	    " VALUES (?, ?, ?, ?, (SELECT id FROM " table_client_info
	    " WHERE config_name = ? LIMIT 1 ))";
	size_t   len  = 0;
	uint8_t *blob = nni_mqtt_msg_serialize(msg, &len, proto_ver);
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
	sqlite3_bind_int(stmt, 4, proto_ver);
	sqlite3_bind_text(
	    stmt, 5, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	nng_free(blob, len);
	int rv = sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	nni_msg_free(msg);
	return rv;
}

nni_msg *
nni_mqtt_qos_db_get_client_msg(
    sqlite3 *db, uint32_t pipe_id, uint16_t packet_id, const char *config_name)
{
	nni_msg *     msg = NULL;
	sqlite3_stmt *stmt;

	char sql[] =
	    "SELECT proto_ver, data FROM " table_client_msg ""
	    " WHERE pipe_id = ? AND packet_id = ? AND info_id = (SELECT id "
	    "FROM " table_client_info " WHERE config_name = ? LIMIT 1) ";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_int(stmt, 2, packet_id);
	sqlite3_bind_text(
	    stmt, 3, config_name, strlen(config_name), SQLITE_TRANSIENT);

	if (SQLITE_ROW == sqlite3_step(stmt)) {
		uint8_t  proto_ver = sqlite3_column_int(stmt, 0);
		size_t   nbyte     = (size_t) sqlite3_column_bytes16(stmt, 1);
		uint8_t *bytes     = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 1), nbyte);
		// deserialize blob data to nni_msg
		msg = nni_mqtt_msg_deserialize(
		    bytes, nbyte, pipe_id > 0 ? true : false, proto_ver);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

void
nni_mqtt_qos_db_remove_client_msg(
    sqlite3 *db, uint32_t pipe_id, uint16_t packet_id, const char *config_name)
{
	sqlite3_stmt *stmt;

	char sql[] =
	    "DELETE FROM " table_client_msg
	    " WHERE pipe_id = ? AND packet_id = ? AND info_id = (SELECT id "
	    "FROM "table_client_info" WHERE config_name = ? LIMIT 1)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int64(stmt, 1, pipe_id);
	sqlite3_bind_int(stmt, 2, packet_id);
	sqlite3_bind_text(
	    stmt, 3, config_name, strlen(config_name), SQLITE_TRANSIENT);
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
nni_mqtt_qos_db_reset_client_msg_pipe_id(sqlite3 *db, const char *config_name)
{
	sqlite3_stmt *stmt;

	char sql[] =
	    "UPDATE " table_client_msg " SET pipe_id = 0 WHERE info_id = "
	    "(SELECT id FROM " table_client_info
	    " WHERE config_name = ? LIMIT 1)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

static void
remove_oldest_client_msg(sqlite3 *db, const char *table_name,
    const char *col_name, uint64_t limit, const char *config_name)
{
	sqlite3_stmt *stmt;
	char          sql[256] = { 0 };

	snprintf(sql, 256,
	    "DELETE FROM %s WHERE %s NOT IN ( SELECT %s FROM %s WHERE info_id "
	    "= (SELECT id "
	    "FROM " table_client_info
	    " WHERE config_name = ? LIMIT 1) ORDER BY"
	    " %s DESC LIMIT ?)",
	    table_name, col_name, col_name, table_name, col_name);

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, limit);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

void
nni_mqtt_qos_db_remove_oldest_client_msg(
    sqlite3 *db, uint64_t limit, const char *config_name)
{
	remove_oldest_client_msg(
	    db, table_client_msg, "ts", limit, config_name);
}

nni_msg *
nni_mqtt_qos_db_get_one_client_msg(
    sqlite3 *db, uint64_t *id, uint16_t *packet_id, const char *config_name)
{
	nni_msg *     msg = NULL;
	sqlite3_stmt *stmt;

	char sql[] =
	    "SELECT id, pipe_id, packet_id, data, proto_ver FROM " table_client_msg
	    " WHERE info_id = (SELECT id FROM " table_client_info
	    " WHERE config_name = ? LIMIT 1) "
	    " ORDER BY id LIMIT 1";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);

	if (SQLITE_ROW == sqlite3_step(stmt)) {
		*id              = (uint64_t) sqlite3_column_int64(stmt, 0);
		uint32_t pipe_id = sqlite3_column_int64(stmt, 1);
		*packet_id       = sqlite3_column_int(stmt, 2);
		size_t   nbyte   = (size_t) sqlite3_column_bytes16(stmt, 3);
		uint8_t *bytes   = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 3), nbyte);
		uint8_t proto_ver = sqlite3_column_int(stmt, 4);

		// deserialize blob data to nni_msg
		msg = nni_mqtt_msg_deserialize(
		    bytes, nbyte, pipe_id > 0 ? true : false, proto_ver);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);

	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

int
nni_mqtt_qos_db_set_client_offline_msg(
    sqlite3 *db, nni_msg *msg, const char *config_name, uint8_t proto_ver)
{
	char sql[] = "INSERT INTO " table_client_offline_msg
	             " (proto_ver, data, info_id ) "
	             "VALUES ( ?, ?, (SELECT id FROM " table_client_info
	             " WHERE config_name = ? LIMIT 1 ))";
	size_t   len  = 0;
	uint8_t *blob = nni_mqtt_msg_serialize(msg, &len, proto_ver);

	if (!blob) {
		printf("nni_mqtt_msg_serialize failed\n");
		nni_msg_free(msg);
		return -1;
	}

	sqlite3_stmt *stmt;
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_int(stmt, 1, proto_ver);
	sqlite3_bind_blob64(stmt, 2, blob, len, SQLITE_TRANSIENT);
	sqlite3_bind_text(
	    stmt, 3, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	nng_free(blob, len);
	int rv = sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	nni_msg_free(msg);
	return rv;
}

int
nni_mqtt_qos_db_set_client_offline_msg_batch(
    sqlite3 *db, nni_lmq *lmq, const char *config_name, uint8_t proto_ver)
{
	int info_id = get_client_info_id(db, config_name);
	if(info_id < 0) {
		return -1;
	}

	char sql[] =
	    "INSERT INTO " table_client_offline_msg " ( data, proto_ver, info_id ) "
	    "VALUES ( ? , ? , ?)";

	sqlite3_stmt *stmt;
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	size_t lmq_len = nni_lmq_len(lmq);
	for (size_t i = 0; i < lmq_len; i++) {
		nni_msg *msg;
		if (nni_lmq_get(lmq, &msg) == 0) {
			size_t   len  = 0;
			uint8_t *blob = nni_mqtt_msg_serialize(msg, &len, proto_ver);
			if (!blob) {
				printf("nni_mqtt_msg_serialize failed\n");
				nni_msg_free(msg);
				continue;
			}
			sqlite3_reset(stmt);
			sqlite3_bind_blob64(
			    stmt, 1, blob, len, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, 2, proto_ver);
			sqlite3_bind_int(stmt, 3, info_id);
			sqlite3_step(stmt);
			nng_free(blob, len);
			nni_msg_free(msg);
		}
	}
	sqlite3_finalize(stmt);
	int rv = sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return rv;
}

size_t
nni_mqtt_qos_db_get_client_offline_msg_count(
    sqlite3 *db, const char *config_name)
{
	char sql[] = "SELECT COUNT(*) FROM " table_client_offline_msg
	             " WHERE info_id = (SELECT id FROM " table_client_info
	             " WHERE config_name = ? LIMIT 1) ";
	sqlite3_stmt *stmt;
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	size_t count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return count;
}

nng_msg *
nni_mqtt_qos_db_get_client_offline_msg(
    sqlite3 *db, int64_t *row_id, const char *config_name)
{
	nni_msg *     msg = NULL;
	sqlite3_stmt *stmt;

	char sql[] = "SELECT id, proto_ver, data FROM " table_client_offline_msg
	             " WHERE info_id = (SELECT id FROM " table_client_info
	             " WHERE config_name = ? LIMIT 1) "
	             " ORDER BY id ASC LIMIT 1 ";

	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);

	if (SQLITE_ROW == sqlite3_step(stmt)) {
		*row_id            = sqlite3_column_int64(stmt, 0);
		uint8_t  proto_ver = sqlite3_column_int(stmt, 1);
		size_t   nbyte     = (size_t) sqlite3_column_bytes16(stmt, 2);
		uint8_t *bytes     = sqlite3_malloc(nbyte);
		memcpy(bytes, sqlite3_column_blob(stmt, 2), nbyte);
		// deserialize blob data to nni_msg
		msg = nni_mqtt_msg_deserialize(bytes, nbyte, false, proto_ver);
		sqlite3_free(bytes);
	}
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	return msg;
}

void
nni_mqtt_qos_db_remove_oldest_client_offline_msg(
    sqlite3 *db, uint64_t limit, const char *config_name)
{
	remove_oldest_client_msg(
	    db, table_client_offline_msg, "ts", limit, config_name);
}

int
nni_mqtt_qos_db_remove_client_offline_msg(sqlite3 *db, int64_t row_id)
{
	sqlite3_stmt *stmt;
	char sql[] = "DELETE FROM " table_client_offline_msg " WHERE id = ?";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);

	sqlite3_bind_int64(stmt, 1, row_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	return sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

int
nni_mqtt_qos_db_remove_all_client_offline_msg(sqlite3 *db, const char *config_name)
{
	sqlite3_stmt *stmt;
	char          sql[] = "DELETE FROM " table_client_offline_msg
	             " WHERE info_id = (SELECT id FROM " table_client_info
	             " WHERE config_name = ? LIMIT 1)";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	return sqlite3_exec(db, "COMMIT;", 0, 0, 0);
}

static int
get_client_info_id(sqlite3 *db, const char *config_name)
{
	int           id = -1;
	sqlite3_stmt *stmt;
	char          sql[] = "SELECT id FROM " table_client_info
	             " WHERE config_name = ? LIMIT 1";
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);
	if (SQLITE_ROW == sqlite3_step(stmt)) {
		id = sqlite3_column_int(stmt, 0);
	}
	sqlite3_finalize(stmt);
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return id;
}

int
nni_mqtt_qos_db_set_client_info(sqlite3 *db, const char *config_name,
    const char *client_id, const char *proto_name, uint8_t proto_ver)
{
	char sql[] = "INSERT OR REPLACE INTO " table_client_info
	             " (id, config_name, client_id, proto_name, proto_ver ) "
	             " VALUES ( ( SELECT id FROM " table_client_info
	             " WHERE config_name = ? LIMIT 1 ), ?, ?, ?, ? )";

	sqlite3_stmt *stmt;
	sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);
	sqlite3_reset(stmt);
	sqlite3_bind_text(
	    stmt, 1, config_name, strlen(config_name), SQLITE_TRANSIENT);
	sqlite3_bind_text(
	    stmt, 2, config_name, strlen(config_name), SQLITE_TRANSIENT);
	if (client_id) {
		sqlite3_bind_text(
		    stmt, 3, client_id, strlen(client_id), SQLITE_TRANSIENT);
	} else {
		sqlite3_bind_null(stmt, 3);
	}
	sqlite3_bind_text(
	    stmt, 4, proto_name, strlen(proto_name), SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 5, proto_ver);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	int rv = sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	return rv;
}

static uint8_t *
nni_mqtt_msg_serialize(nni_msg *msg, size_t *out_len, uint8_t proto_ver)
{
	// int rv;
	// if ((rv = nni_mqtt_msg_encode(msg)) != 0) {
	// 	printf("nni_mqtt_msg_encode failed: %d\n", rv);
	// 	return NULL;
	// }
	if (proto_ver == MQTT_PROTOCOL_VERSION_v5) {
		nni_mqttv5_msg_encode(msg);
	} else {
		nni_mqtt_msg_encode(msg);
	}

	size_t len = nni_msg_header_len(msg) + nni_msg_len(msg) +
	    (sizeof(uint32_t) * 2) + sizeof(nni_time) + sizeof(nni_aio *);
	*out_len = len;

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
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
nni_mqtt_msg_deserialize(
    uint8_t *bytes, size_t len, bool aio_available, uint8_t proto_ver)
{
	nni_msg *msg;
	if (nni_mqtt_msg_alloc(&msg, 0) != 0) {
		return NULL;
	}

	struct pos_buf buf = { .curpos = &bytes[0], .endpos = &bytes[len] };

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
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

	if (proto_ver == MQTT_PROTOCOL_VERSION_v5) {
		nni_mqttv5_msg_decode(msg);
	} else {
		nni_mqtt_msg_decode(msg);
	}

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
	if (msg) {
		nni_msg_free(msg);
	}
	return NULL;
}

void
nni_mqtt_sqlite_db_init(
    nng_mqtt_sqlite_option *sqlite, const char *db_name, uint8_t proto_ver)
{
	if (sqlite != NULL && sqlite->enable) {
		nni_lmq_init(
		    &sqlite->offline_cache, sqlite->flush_mem_threshold);
		sqlite->db_name = nni_strdup(db_name);
		nni_mqtt_qos_db_init(
		    &sqlite->db, sqlite->mounted_file_path, db_name);

		sqlite->mqtt_version = proto_ver;

		nni_mqtt_qos_db_set_client_info(
		    sqlite->db, db_name, NULL, "MQTT", sqlite->mqtt_version);
	}
}

void
nni_mqtt_sqlite_db_fini(nni_mqtt_sqlite_option *sqlite)
{
	if (sqlite != NULL && sqlite->enable) {
		nni_lmq_fini(&sqlite->offline_cache);
		nni_strfree(sqlite->db_name);
		nni_mqtt_qos_db_close(sqlite->db);
	}
}

size_t
nni_mqtt_sqlite_db_get_cached_size(nni_mqtt_sqlite_option *sqlite)
{
	size_t sz = nni_mqtt_qos_db_get_client_offline_msg_count(
	    sqlite->db, sqlite->db_name);
	sz += nni_lmq_len(&sqlite->offline_cache);
	return sz;
}