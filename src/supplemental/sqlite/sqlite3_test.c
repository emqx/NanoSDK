
#include <nng/nng.h>
#include <stdint.h>
#include <string.h>

#include "nuts.h"
#include "nng/supplemental/sqlite/sqlite3.h"

void
test_sqlite_write(void)
{
	const int nCount = 500000;
	sqlite3 * db;
	TEST_CHECK(sqlite3_open("testdb.db", &db) == 0);
	TEST_CHECK(
	    sqlite3_exec(db, "PRAGMA synchronous = OFF; ", 0, 0, 0) == 0);
	TEST_CHECK(sqlite3_exec(db, "drop table if exists t1", 0, 0, 0) == 0);
	TEST_CHECK(
	    sqlite3_exec(db,
	        "create table t1(id integer,x integer,y integer ,weight real)",
	        0, 0, 0) == 0);

	sqlite3_exec(db, "begin;", 0, 0, 0);
	sqlite3_stmt *stmt;
	const char *  sql = "insert into t1 values(?,?,?,?)";
	sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, 0);

	for (int i = 0; i < nCount; ++i) {
		sqlite3_reset(stmt);
		sqlite3_bind_int(stmt, 1, i);
		sqlite3_bind_int(stmt, 2, i * 2);
		sqlite3_bind_int(stmt, 3, i / 2);
		sqlite3_bind_double(stmt, 4, i * i);
		sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	
	TEST_CHECK(sqlite3_exec(db, "commit;", 0, 0, 0) == 0);

	TEST_CHECK(sqlite3_close(db) == 0);
}

TEST_LIST = {
	{ "write", test_sqlite_write },
	{ NULL, NULL },
};
