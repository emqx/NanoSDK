#include "mqtt_qos_db.h"
#include "core/nng_impl.h"
#include "nng/nng.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "supplemental/sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NNG_PLATFORM_WINDOWS
#include <unistd.h>
#endif

#define table_client_msg "t_client_msg"

static uint8_t *nni_mqtt_msg_serialize(nni_msg *msg, size_t *out_len);
static nni_msg *nni_mqtt_msg_deserialize(
    uint8_t *bytes, size_t len, bool aio_available);

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
		if (create_client_msg_table(*db) != 0) {
			return;
		}

	}
}

void
nni_mqtt_qos_db_close(sqlite3 *db)
{
	sqlite3_close(db);
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
	sqlite3_exec(db, "COMMIT;", 0, 0, 0);
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
	nni_mqtt_msg_encode(msg);

	size_t len = nni_msg_header_len(msg) + nni_msg_len(msg) +
	    (sizeof(uint32_t) * 2) + sizeof(nni_time) + sizeof(nni_aio *);
	*out_len = len;

	// bytes:
	// header:  header_len(uint32) + header(header_len)
	// body:	body_len(uint32) + body(body_len)
	// aio:		address value
	uint8_t *bytes = nng_alloc(len);

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
