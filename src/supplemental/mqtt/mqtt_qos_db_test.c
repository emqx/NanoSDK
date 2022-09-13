#include <string.h>

#include "mqtt_msg.h"
#include "mqtt_qos_db.h"
#include "nng/nng.h"
#include "nuts.h"

#define test_db "test.db"

void
test_db_init(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);
	nni_mqtt_qos_db_close(db);
}

void
test_set_client_info(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);

	nni_mqtt_qos_db_set_client_info(
	    db, "nanomq", "client-2984792", "MQTT", 4);
	nni_mqtt_qos_db_set_client_info(
	    db, "emqx", "client-2984792", "MQTT", 4);
	nni_mqtt_qos_db_set_client_info(
	    db, "aws", "client-2984791", "MQTT", 4);

	nni_mqtt_qos_db_close(db);
}

void
test_set_client_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);

	uint32_t pipe_id   = 12345;
	uint16_t packet_id = 54321;

	nni_msg *msg;
	nni_mqtt_msg_alloc(&msg, 0);

	nni_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	NUTS_TRUE(nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT);
	nni_mqtt_msg_set_connect_client_id(msg, "nanomq-client-0FADECF");
	nni_mqtt_msg_set_connect_proto_version(msg, 4);

	char user[]   = "nanomq";
	char passwd[] = "nanomq";

	nng_mqtt_msg_set_connect_user_name(msg, user);
	nng_mqtt_msg_set_connect_password(msg, passwd);
	nng_mqtt_msg_set_connect_clean_session(msg, true);
	nng_mqtt_msg_set_connect_keep_alive(msg, 60);

	TEST_CHECK(nni_mqtt_qos_db_set_client_msg(
	               db, pipe_id, packet_id, msg, "emqx", 4) == 0);
	nni_mqtt_qos_db_close(db);
}

void
test_get_client_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);

	nni_msg *msg =
	    nni_mqtt_qos_db_get_client_msg(db, 12345, 54321, "emqx");
	TEST_CHECK(msg != NULL);
	TEST_CHECK(nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT);
	TEST_CHECK(nni_mqtt_msg_get_connect_proto_version(msg) == 4);
	TEST_CHECK(nni_mqtt_msg_get_connect_keep_alive(msg) == 60);
	TEST_CHECK(strcmp(nni_mqtt_msg_get_connect_client_id(msg),
	               "nanomq-client-0FADECF") == 0);
	TEST_CHECK(
	    strcmp(nni_mqtt_msg_get_connect_user_name(msg), "nanomq") == 0);
	nni_msg_free(msg);
	nni_mqtt_qos_db_close(db);
}

void
test_remove_client_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);
	nni_mqtt_qos_db_remove_client_msg(db, 12345, 54321, "emqx");
	nni_mqtt_qos_db_close(db);
}

void
test_set_client_offline_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);

	nni_msg *msg;
	nni_mqtt_msg_alloc(&msg, 0);

	nni_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	NUTS_TRUE(nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT);
	nni_mqtt_msg_set_connect_client_id(msg, "nanomq-client-0FADECF");
	nni_mqtt_msg_set_connect_proto_version(msg, 4);

	char user[]   = "nanomq";
	char passwd[] = "nanomq";

	nng_mqtt_msg_set_connect_user_name(msg, user);
	nng_mqtt_msg_set_connect_password(msg, passwd);
	nng_mqtt_msg_set_connect_clean_session(msg, true);
	nng_mqtt_msg_set_connect_keep_alive(msg, 60);

	TEST_CHECK(
	    nni_mqtt_qos_db_set_client_offline_msg(db, msg, "emqx", 4) == 0);
	nni_mqtt_qos_db_close(db);
}

void
test_get_client_offline_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);

	int64_t  row_id = 0;

	nni_msg *msg =
	    nni_mqtt_qos_db_get_client_offline_msg(db, &row_id, "emqx");
	TEST_CHECK(msg != NULL);
	TEST_CHECK(nni_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT);
	TEST_CHECK(nni_mqtt_msg_get_connect_proto_version(msg) == 4);
	TEST_CHECK(nni_mqtt_msg_get_connect_keep_alive(msg) == 60);
	TEST_CHECK(strcmp(nni_mqtt_msg_get_connect_client_id(msg),
	               "nanomq-client-0FADECF") == 0);
	TEST_CHECK(
	    strcmp(nni_mqtt_msg_get_connect_user_name(msg), "nanomq") == 0);

	nni_msg_free(msg);
	nni_mqtt_qos_db_close(db);
}

void
test_remove_client_offline_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);
	nni_mqtt_qos_db_remove_client_offline_msg(db, 1);
	nni_mqtt_qos_db_close(db);
}

void
test_batch_insert_client_offline_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);

	nni_lmq lmq;
	nni_lmq_init(&lmq, 10);

	for (int i = 0; i < 10; i++) {
		nni_msg *msg;
		nni_mqtt_msg_alloc(&msg, 0);
		nni_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
		NUTS_TRUE(
		    nng_mqtt_msg_get_packet_type(msg) == NNG_MQTT_CONNECT);
		nni_mqtt_msg_set_connect_proto_version(msg, 4);
		nng_mqtt_msg_set_connect_keep_alive(msg, 60 + i);
		nni_lmq_put(&lmq, msg);
	}

	TEST_CHECK(nni_mqtt_qos_db_set_client_offline_msg_batch(
	               db, &lmq, "emqx", 4) == 0);
	nni_lmq_fini(&lmq);
	nni_mqtt_qos_db_close(db);
}

void
test_remove_oldest_client_offline_msg(void)
{
	sqlite3 *db = NULL;
	nni_mqtt_qos_db_init(&db, NULL, test_db);
	nni_mqtt_qos_db_remove_oldest_client_offline_msg(db, 0, "emqx");
	nni_mqtt_qos_db_close(db);
}

TEST_LIST = {
	{ "db_init", test_db_init },
	{ "db_set_client_info", test_set_client_info },
	{ "db_set_client_msg", test_set_client_msg },
	{ "db_get_client_msg", test_get_client_msg },
	{ "db_remove_client_msg", test_remove_client_msg },
	{ "db_set_client_offline_msg", test_set_client_offline_msg },
	{ "db_get_client_offline_msg", test_get_client_offline_msg },
	{ "db_remove_client_offline_msg", test_remove_client_offline_msg },
	{ "db_batch_insert_client_offline_msg",
	    test_batch_insert_client_offline_msg },
	{ "db_remove_oldest_client_offline_msg",
	    test_remove_oldest_client_offline_msg },
	{ NULL, NULL },
};