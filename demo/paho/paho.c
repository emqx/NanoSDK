
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/mqtt/mqtt_paho.h>
#include <nng/nng.h>
#include <nng/supplemental/util/platform.h>
#include <nng/supplemental/tls/tls.h>

#if defined(_WIN32)
#include <windows.h>
#define sleep Sleep
#else
#include <unistd.h>
#include <sys/time.h>
#endif

#if defined(_WRS_KERNEL)
#include <OsWrapper.h>
#endif

#define ADDRESS     "tls+mqtt-tcp://123.60.191.138:2443"
#define CLIENTID    "ExampleClientSub"
#define TOPIC       "South China Grid"
#define PAYLOAD     "Hello World!"
#define QOS         2
#define TIMEOUT     10000L

#define CLIENT_CA_CERT_PATH "/home/jaylin/Downloads/certs/client/ca.crt"

#define CLIENT_SIGN_CERT_PATH "/home/jaylin/Downloads/certs/client/client_sign.crt"
#define CLIENT_SIGN_KEY_PATH "/home/jaylin/Downloads/certs/client/client_sign.key"

#define CLIENT_ENC_CERT_PATH "/home/jaylin/Downloads/certs/client/client.crt"
#define CLIENT_ENC_KEY_PATH "/home/jaylin/Downloads/certs/client/client.key"

int disc_finished = 0;
int subscribed = 0;
int finished = 0;

void connlost(void *context, char *cause)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	int rc;

	printf("\nConnection lost\n");
	if (cause)
		printf("     cause: %s\n", cause);

	printf("Reconnecting\n");
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start connect, return code %d\n", rc);
		finished = 1;
	}
}


int msgarrvd(void *context, char *topicName, int topicLen, MQTTAsync_message *message)
{
    printf("Message arrived\n");
    printf("     topic: %s qos : %d\n", topicName, message->msgid);
    printf("   message: %.*s\n", message->payloadlen, (char*)message->payload);
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);
    return 1;
}

void onDisconnectFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Disconnect failed, rc %d\n", response->code);
	disc_finished = 1;
}

void onDisconnect(void* context, MQTTAsync_successData* response)
{
	printf("Successful disconnection\n");
	disc_finished = 1;
}

void onSubscribe(void* context, MQTTAsync_successData* response)
{
	printf("Subscribe succeeded\n");
	subscribed = 1;
}

void onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Subscribe failed, rc %d\n", response->code);
	finished = 1;
}


void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Connect failed, rc %d\n", response->code);
	finished = 1;
}


void onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	printf("Successful connection\n");

	printf("Subscribing to topic %s\nfor client %s using QoS%d\n\n"
           "Press Q<Enter> to quit\n\n", TOPIC, CLIENTID, QOS);
	opts.onSuccess5 = onSubscribe;
	opts.onFailure5 = onSubscribeFailure;
	opts.context = client;
	if ((rc = MQTTAsync_subscribe(client, TOPIC, QOS, &opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start subscribe, return code %d\n", rc);
		finished = 1;
	}
}

void asyncDeliveryComplete(void* context, MQTTAsync_token token)
{
	printf("qos finished!");
}

void testConnected(void* context, char* cause)
{
	MQTTAsync c = (MQTTAsync)context;
	nng_log_debug("Test", "testConnected is called! %s", cause);
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	printf("Successful connection\n");
	// opts.onSuccess = onSubscribe;
	// opts.onFailure = onSubscribeFailure;
	opts.onSuccess5 = onSubscribe;
	opts.onFailure5 = onSubscribeFailure;
	opts.context = c;

	// opts.onSuccess = onSubscribeSuccess;
	// opts.onFailure = onSubscribeFailure;
	// opts.context = NULL;
	if ((rc = MQTTAsync_subscribe(c, "msg", 1, &opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start subscribe, return code %d\n", rc);
		finished = 1;
	}
}

int main(int argc, char* argv[])
{
	MQTTAsync client;
	MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer5;
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer5;
	MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer5;
	int rc;
	int ch;

	// nng_log_set_logger(nng_system_logger);
	nng_log_set_logger(nng_stderr_logger);
	nng_log_set_level(NNG_LOG_DEBUG);
	rc = MQTTAsync_createWithOptions(&client, ADDRESS, CLIENTID,
		MQTTCLIENT_PERSISTENCE_NONE, NULL, &create_opts);
	if (rc != MQTTASYNC_SUCCESS)
	{
		printf("Failed to create client, return code %d\n", rc);
		rc = EXIT_FAILURE;
		goto exit;
	}

	if ((rc = MQTTAsync_setCallbacks(client, client, connlost, msgarrvd, asyncDeliveryComplete)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to set callbacks, return code %d\n", rc);
		rc = EXIT_FAILURE;
		goto destroy_exit;
	}
	MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
	ssl_opts.enableServerCertAuth = 1;  // 开启服务器证书认证
	ssl_opts.sslVersion = MQTT_SSL_VERSION_DEFAULT;
	ssl_opts.keyStore = CLIENT_SIGN_CERT_PATH;
	ssl_opts.privateKey = CLIENT_SIGN_KEY_PATH;
	ssl_opts.dkeyStore = CLIENT_ENC_CERT_PATH;
	ssl_opts.dprivateKey = CLIENT_ENC_KEY_PATH;
	ssl_opts.trustStore = CLIENT_CA_CERT_PATH;

	conn_opts.keepAliveInterval = 64;
	conn_opts.scram             = true;
	conn_opts.MQTTVersion       = MQTTVERSION_5;
	conn_opts.onSuccess         = onConnect;
	conn_opts.onFailure         = onConnectFailure;
	conn_opts.onSuccess5        = onConnect;
	conn_opts.onFailure5        = onConnectFailure;
	conn_opts.username          = "admin";
	conn_opts.password          = "public";
	conn_opts.context           = NULL;
	// 增加重连机制
	// conn_opts.automaticReconnect = 1; // NO effects at all
	conn_opts.maxRetryInterval = 60; // 单位秒，最大重连尝试间隔
	// ssl选项
	conn_opts.ssl = &ssl_opts;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start connect, return code %d\n", rc);
		rc = EXIT_FAILURE;
		goto destroy_exit;
	}
	if (MQTTAsync_setConnected(client, NULL, testConnected) != MQTTASYNC_SUCCESS)
		printf("MQTTAsync_setConnected failed");

	while (!subscribed && !finished)
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif

	if (finished)
		goto exit;

	do 
	{
		ch = getchar();
	} while (ch!='Q' && ch != 'q');

	disc_opts.onSuccess = onDisconnect;
	disc_opts.onFailure = onDisconnectFailure;
	if ((rc = MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start disconnect, return code %d\n", rc);
		rc = EXIT_FAILURE;
		goto destroy_exit;
	}
 	while (!disc_finished)
 	{
		#if defined(_WIN32)
			Sleep(100);
		#else
			usleep(10000L);
		#endif
 	}

destroy_exit:
	MQTTAsync_destroy(&client);
exit:
 	return rc;
}
