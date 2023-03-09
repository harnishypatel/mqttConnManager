/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2022 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <mosquitto.h>
#include <mqtt_protocol.h>
#include <rbus/rbus.h>
#include <rbus/rbus_object.h>
#include <rbus/rbus_property.h>
#include <rbus/rbus_value.h>

#define MQTT_CONFIG_FILE     "/tmp/.mqttconfig"
#define MOSQ_TLS_VERSION     "tlsv1.2"
#define OPENSYNC_CERT        "/etc/webcfg_mqtt/mqtt_cert_init.sh"
#define KEEPALIVE            60
#define MQTT_PORT            443
#define MAX_MQTT_LEN         128
#define NUM_WEBCFG_ELEMENTS3 1
#define MAX_BUF_SIZE 255
#define maxParamLen 128
#define pComponentName "mqttconnectionmgr"

#define MQTT_SUBSCRIBE_TOPIC_PREFIX "x/to/"
#define MQTT_PUBLISH_GET_TOPIC_PREFIX "x/fr/get/chi/"
#define MQTT_PUBLISH_NOTIFY_TOPIC_PREFIX "x/fr/poke/chi/"

#define WEBCFG_MQTT_DATA_PARAM "Device.X_RDK_WebConfig.MQTT.Data"
#define WEBCFG_MQTT_BROKER_PARAM     "Device.X_RDK_WebConfig.MQTT.Broker"
#define WEBCFG_MQTT_NODEID_PARAM     "Device.X_RDK_WebConfig.MQTT.NodeId"
#define WEBCFG_MQTT_PORT_PARAM       "Device.X_RDK_WebConfig.MQTT.Port"

#define MAX_MQTT_RETRY 8
#define MQTT_RETRY_ERR -1
#define MQTT_RETRY_SHUTDOWN 1
#define MQTT_DELAY_TAKEN 0
#define UNUSED(x) (void )(x)

typedef struct {
  struct timespec ts;
  int count;
  int max_count;
  int delay;
} mqtt_timer_t;

void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
void on_disconnect(struct mosquitto *mosq, void *obj, int reason_code);
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos);
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);
void on_publish(struct mosquitto *mosq, void *obj, int mid);

int writeToDBFile(char *db_file_path, char *data, size_t size);
bool webcfg_mqtt_init();
void get_from_file(char *key, char **val, char *filepath);
void publish_notify_mqtt(char *pub_topic, void *payload, ssize_t len, char * dest);
char * createMqttPubHeader(char * payload, char * dest, ssize_t * payload_len);
int get_global_mqtt_connected();
void reset_global_mqttConnected();
void set_global_mqttConnected();
int createMqttHeader(char **header_list);
int triggerBootupSync();
void checkMqttParamSet();
pthread_mutex_t *get_global_mqtt_retry_mut(void);
pthread_cond_t *get_global_mqtt_retry_cond(void);
int processPayload(char * data, int dataSize);
int validateForMqttInit();
pthread_cond_t *get_global_mqtt_cond(void);
pthread_mutex_t *get_global_mqtt_mut(void);
int regWebConfigDataModel_mqtt();
void execute_mqtt_script(char *name);
int getHostIPFromInterface(char *interface, char **ip);
rbusError_t eventSubHandler(rbusHandle_t handle, rbusEventSubAction_t action, const char* eventName, rbusFilter_t filter, int32_t interval, bool* autoPublish);

static int g_mqttConnected = 0;
static int mqttdata_len = 0;
struct mosquitto *mosq = NULL;
//global flag to do bootupsync only once after connect and subscribe callback.
static int bootupsync = 0;
static int subscribeFlag = 0;
//static uint8_t* mqttdata = NULL;
//static void* mqttdata = NULL;
static char* mqttdata = NULL;
static rbusHandle_t rbus_handle;
int subscribed1 = 1;

pthread_mutex_t mqtt_retry_mut=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t mqtt_retry_con=PTHREAD_COND_INITIALIZER;
pthread_mutex_t mqtt_mut=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t mqtt_con=PTHREAD_COND_INITIALIZER;

rbusDataElement_t dataElements[NUM_WEBCFG_ELEMENTS3] = {
        {WEBCFG_MQTT_DATA_PARAM, RBUS_ELEMENT_TYPE_EVENT, {NULL, NULL, NULL, NULL, eventSubHandler, NULL}}
    };

int rbus_GetValueFromDB( char* paramName, char** paramValue)
{
	printf("Inside rbus_GetValueFromDB weak fn\n");
	UNUSED(paramName);
	UNUSED(paramValue);
	return 0;
}

int rbus_StoreValueIntoDB(char *paramName, char *value)
{
	printf("Inside rbus_StoreValueIntoDB weak fn\n");
	UNUSED(paramName);
	UNUSED(value);
	return 0;
}

rbusHandle_t get_global_rbus_handle(void)
{
     return rbus_handle;
}

int get_global_shutdown()
{
	return 0;
}

int get_global_mqtt_connected()
{
    return g_mqttConnected;
}

void reset_global_mqttConnected()
{
	g_mqttConnected = 0;
}

void set_global_mqttConnected()
{
	g_mqttConnected = 1;
}

void convertToUppercase(char* deviceId)
{
	int j =0;
	while (deviceId[j])
	{
		deviceId[j] = toupper(deviceId[j]);
		j++;
	}
}

pthread_cond_t *get_global_mqtt_retry_cond(void)
{
    return &mqtt_retry_con;
}

pthread_mutex_t *get_global_mqtt_retry_mut(void)
{
    return &mqtt_retry_mut;
}

pthread_cond_t *get_global_mqtt_cond(void)
{
    return &mqtt_con;
}

pthread_mutex_t *get_global_mqtt_mut(void)
{
    return &mqtt_mut;
}
/*
void checkMqttParamSet()
{
	printf("checkMqttParamSet\n");

	if( !validateForMqttInit())
	{
		printf("Validation success for mqtt parameters, proceed to mqtt init\n");
	}
	else
	{
		pthread_mutex_lock(get_global_mqtt_mut());
		pthread_cond_wait(get_global_mqtt_cond(), get_global_mqtt_mut());
		pthread_mutex_unlock(get_global_mqtt_mut());
		printf("Received mqtt signal proceed to mqtt init\n");
	}
}
*/

rbusError_t eventSubHandler(rbusHandle_t handle, rbusEventSubAction_t action, const char* eventName, rbusFilter_t filter, int32_t interval, bool* autoPublish)
{
    (void)handle;
    (void)filter;
    (void)autoPublish;
    (void)interval;

    printf(
        "eventSubHandler called:\n" \
        "\taction=%s\n" \
        "\teventName=%s\n",
        action == RBUS_EVENT_ACTION_SUBSCRIBE ? "subscribe" : "unsubscribe",
        eventName);

    if(!strcmp(WEBCFG_MQTT_DATA_PARAM, eventName))
    {
        subscribed1 = action == RBUS_EVENT_ACTION_SUBSCRIBE ? 1 : 0;
    }
    else
    {
        printf("provider: eventSubHandler unexpected eventName %s\n", eventName);
    }

    return RBUS_ERROR_SUCCESS;
}

void init_mqtt_timer (mqtt_timer_t *timer, int max_count)
{
  timer->count = 1;
  timer->max_count = max_count;
  timer->delay = 3;  //7s,15s,31s....
  clock_gettime (CLOCK_MONOTONIC, &timer->ts);
}

unsigned update_mqtt_delay (mqtt_timer_t *timer)
{
  if (timer->count < timer->max_count) {
    timer->count += 1;
    timer->delay = timer->delay + timer->delay + 1;
    // 3,7,15,31 ..
  }
  return (unsigned) timer->delay;
}

unsigned mqtt_rand_secs (int random_num, unsigned max_secs)
{
  unsigned delay_secs = (unsigned) random_num & max_secs;
  if (delay_secs < 3)
    return delay_secs + 3;
  else
    return delay_secs;
}

unsigned mqtt_rand_nsecs (int random_num)
{
	/* random _num is in range 0..2147483648 */
	unsigned n = (unsigned) random_num >> 1;
	/* n is in range 0..1073741824 */
	if (n < 1000000000)
	  return n;
	return n - 1000000000;
}

void mqtt_add_timespec (struct timespec *t1, struct timespec *t2)
{
	t2->tv_sec += t1->tv_sec;
	t2->tv_nsec += t1->tv_nsec;
	if (t2->tv_nsec >= 1000000000) {
	  t2->tv_sec += 1;
	  t2->tv_nsec -= 1000000000;
	}
}

void mqtt_rand_expiration (int random_num1, int random_num2, mqtt_timer_t *timer, struct timespec *ts)
{
	unsigned max_secs = update_mqtt_delay (timer); // 3,7,15,31
	struct timespec ts_delay = {3, 0};

	if (max_secs > 3) {
	  ts_delay.tv_sec = mqtt_rand_secs (random_num1, max_secs);
	  ts_delay.tv_nsec = mqtt_rand_nsecs (random_num2);
	}
    printf("Waiting max delay %u mqttRetryTime %ld secs %ld usecs\n",
      max_secs, ts_delay.tv_sec, ts_delay.tv_nsec/1000);

	/* Add delay to expire time */
    mqtt_add_timespec (&ts_delay, ts);
}

/* mqtt_retry
 *
 * delays for the number of seconds specified in parameter timer
 * g_shutdown can break out of the delay.
 *
 * returns -1 pthread_cond_timedwait error
 *  1   shutdown
 *  0    delay taken
*/
static int mqtt_retry(mqtt_timer_t *timer)
{
  struct timespec ts;
  int rtn;

  pthread_condattr_t mqtt_retry_con_attr;

  pthread_condattr_init (&mqtt_retry_con_attr);
  pthread_condattr_setclock (&mqtt_retry_con_attr, CLOCK_MONOTONIC);
  pthread_cond_init (&mqtt_retry_con, &mqtt_retry_con_attr);

  clock_gettime(CLOCK_MONOTONIC, &ts);

  mqtt_rand_expiration(random(), random(), timer, &ts);

  pthread_mutex_lock(&mqtt_retry_mut);
  // The condition variable will only be set if we shut down.
  rtn = pthread_cond_timedwait(&mqtt_retry_con, &mqtt_retry_mut, &ts);
  pthread_mutex_unlock(&mqtt_retry_mut);

  pthread_condattr_destroy(&mqtt_retry_con_attr);

  if (get_global_shutdown())
    return MQTT_RETRY_SHUTDOWN;
  if ((rtn != 0) && (rtn != ETIMEDOUT)) {
    printf ("pthread_cond_timedwait error (%d) in mqtt_retry.\n", rtn);
    return MQTT_RETRY_ERR;
  }

  return MQTT_DELAY_TAKEN;
}

//Initialize mqtt library and connect to mqtt broker
bool webcfg_mqtt_init()
{
	char *client_id , *username = NULL;
	char hostname[256] = { 0 };
	int rc;
	char PORT[32] = { 0 };
	int port = 0;
	mqtt_timer_t mqtt_timer;
	int tls_count = 0;
	int rt = 0;
	char *bind_interface = NULL;
	char *hostip = NULL;

	//checkMqttParamSet();
	res_init();
	printf("Initializing MQTT library\n");

	mosquitto_lib_init();

	int clean_session = true;

	/*Get_Mqtt_NodeId(g_NodeID);
	printf("g_NodeID fetched from Get_Mqtt_NodeId is %s\n", g_NodeID);*/
	//client_id = strdup(g_NodeID);
    	client_id = strdup("E0DBD1DC8BFF");
	printf("client_id is %s\n", client_id);

	if(client_id !=NULL)
	{

		//Get_Mqtt_Broker(hostname);
		snprintf(hostname,255,"hcbroker-chi-02-pub.staging.us-west-2.plume.comcast.net");
		if(hostname != NULL && strlen(hostname)>0)
		{
			printf("The hostname is %s\n", hostname);
		}
		else
		{
			printf("Invalid config, hostname is NULL\n");
			return MOSQ_ERR_INVAL;
		}

		/*Get_Mqtt_Port(PORT);
		printf("PORT fetched from TR181 is %s\n", PORT);*/
		if(strlen(PORT) > 0)
		{
			port = atoi(PORT);
		}
		else
		{
			port = MQTT_PORT;
		}
		printf("port int %d\n", port);

		while(1)
		{
			username = client_id;
			printf("client_id is %s username is %s\n", client_id, username);

			//execute_mqtt_script(OPENSYNC_CERT);

			if(client_id !=NULL)
			{
				mosq = mosquitto_new(client_id, clean_session, NULL);
			}
			else
			{
				printf("client_id is NULL, init with clean_session true\n");
				mosq = mosquitto_new(NULL, true, NULL);
			}
			if(!mosq)
			{
				printf("Error initializing mosq instance\n");
				return MOSQ_ERR_NOMEM;
			}

			struct libmosquitto_tls *tls;
			tls = malloc (sizeof (struct libmosquitto_tls));
			if(tls)
			{
				memset(tls, 0, sizeof(struct libmosquitto_tls));

				char * CAFILE, *CERTFILE , *KEYFILE = NULL;

				get_from_file("CA_FILE_PATH=", &CAFILE, MQTT_CONFIG_FILE);
				get_from_file("CERT_FILE_PATH=", &CERTFILE, MQTT_CONFIG_FILE);
				get_from_file("KEY_FILE_PATH=", &KEYFILE, MQTT_CONFIG_FILE);

				if(CAFILE !=NULL && CERTFILE!=NULL && KEYFILE !=NULL)
				{
					printf("CAFILE %s, CERTFILE %s, KEYFILE %s MOSQ_TLS_VERSION %s\n", CAFILE, CERTFILE, KEYFILE, MOSQ_TLS_VERSION);

					tls->cafile = CAFILE;
					tls->certfile = CERTFILE;
					tls->keyfile = KEYFILE;
					tls->tls_version = MOSQ_TLS_VERSION;

					rc = mosquitto_tls_set(mosq, tls->cafile, tls->capath, tls->certfile, tls->keyfile, tls->pw_callback);
					printf("mosquitto_tls_set rc %d\n", rc);
					if(rc)
					{
						printf("Failed in mosquitto_tls_set %d %s\n", rc, mosquitto_strerror(rc));
					}
					else
					{
						rc = mosquitto_tls_opts_set(mosq, tls->cert_reqs, tls->tls_version, tls->ciphers);
						printf("mosquitto_tls_opts_set rc %d\n", rc);
						if(rc)
						{
							printf("Failed in mosquitto_tls_opts_set %d %s\n", rc, mosquitto_strerror(rc));
						}
					}

				}
				else
				{
					printf("Failed to get tls cert files\n");
					rc = 1;
				}

				if(rc != MOSQ_ERR_SUCCESS)
				{
					if(tls_count < 3)
					{
						sleep(10);
						printf("Mqtt tls cert Retry %d in progress\n", tls_count+1);
						mosquitto_destroy(mosq);
						tls_count++;
					}
					else
					{
						printf("Mqtt tls cert retry failed!!!, Abort the process\n");

						mosquitto_destroy(mosq);

						free(CAFILE);
						free(CERTFILE);
						free(KEYFILE);
						abort();
					}
				}
				else
				{
					tls_count = 0;
					//connect to mqtt broker
					mosquitto_connect_callback_set(mosq, on_connect);
					printf("set disconnect callback\n");
					mosquitto_disconnect_callback_set(mosq, on_disconnect);
					mosquitto_subscribe_callback_set(mosq, on_subscribe);
					mosquitto_message_callback_set(mosq, on_message);
					mosquitto_publish_callback_set(mosq, on_publish);

					printf("port %d\n", port);

					init_mqtt_timer(&mqtt_timer, MAX_MQTT_RETRY);

					//get_webCfg_interface(&bind_interface);
					if(bind_interface != NULL)
					{
						printf("Interface fetched for mqtt connect bind is %s\n", bind_interface);
						rt = getHostIPFromInterface(bind_interface, &hostip);
						if(rt == 1)
						{
							printf("hostip fetched from getHostIPFromInterface is %s\n", hostip);
						}
						else
						{
							printf("getHostIPFromInterface failed %d\n", rt);
						}
					}
					while(1)
					{
						//rc = mosquitto_connect(mosq, hostname, port, KEEPALIVE);
						printf("B4 mosquitto_connect_bind\n");
						rc = mosquitto_connect_bind(mosq, hostname, port, KEEPALIVE, hostip);

						printf("mosquitto_connect_bind rc %d\n", rc);
						if(rc != MOSQ_ERR_SUCCESS)
						{

							printf("mqtt connect Error: %s\n", mosquitto_strerror(rc));
							if(mqtt_retry(&mqtt_timer) != MQTT_DELAY_TAKEN)
							{
								mosquitto_destroy(mosq);

								free(CAFILE);
								free(CERTFILE);
								free(KEYFILE);
								return rc;
							}
						}
						else
						{
							printf("mqtt broker connect success %d\n", rc);
							set_global_mqttConnected();
							break;
						}
					}

					printf("mosquitto_loop_forever\n");
					rc = mosquitto_loop_forever(mosq, -1, 1);
					//rc = mosquitto_loop_start(mosq);
					if(rc != MOSQ_ERR_SUCCESS)
					{
						mosquitto_destroy(mosq);
						printf("mosquitto_loop_start Error: %s\n", mosquitto_strerror(rc));

						free(CAFILE);
						free(CERTFILE);
						free(KEYFILE);
						return rc;
					}
					else
					{
						printf("after loop rc is %d\n", rc);
						break;
					}
				}
				/*free(CAFILE);
				free(CERTFILE);
				free(KEYFILE);*/
			}
			else
			{
				printf("Allocation failed\n");
				rc = MOSQ_ERR_NOMEM;
			}
		}

	}
	else
	{
		printf("Failed to get client_id\n");
		return 1;

	}
	return rc;
}

// callback called when the client receives a CONNACK message from the broker
void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
        int rc;
	char topic[256] = { 0 };
        printf("on_connect: reason_code %d %s\n", reason_code, mosquitto_connack_string(reason_code));
        if(reason_code != 0)
	{
		printf("on_connect received error\n");
                //reconnect
                mosquitto_disconnect(mosq);
		return;
        }

	//mosquitto_subscribe_callback_set(mosq, on_subscribe1);
	//mosquitto_subscribe_callback_set(mosq, on_subscribe2);
	if(!subscribeFlag)
	{
		//snprintf(topic,MAX_MQTT_LEN,"%s%s", MQTT_SUBSCRIBE_TOPIC_PREFIX,g_NodeID);
		snprintf(topic,MAX_MQTT_LEN,"x/to/E0DBD1DC8BFF");
		if(topic != NULL && strlen(topic)>0)
		{
			printf("subscribe to topic %s\n", topic);
		}

		rc = mosquitto_subscribe(mosq, NULL, topic, 1);

		if(rc != MOSQ_ERR_SUCCESS)
		{
			printf("Error subscribing: %s\n", mosquitto_strerror(rc));
			mosquitto_disconnect(mosq);
		}
		else
		{
			printf("subscribe to topic %s success\n", topic);
			subscribeFlag = 1;
		}
	}
}

// callback called when the client gets DISCONNECT command from the broker
void on_disconnect(struct mosquitto *mosq, void *obj, int reason_code)
{
        printf("on_disconnect: reason_code %d %s\n", reason_code, mosquitto_reason_string(reason_code));
        if(reason_code != 0)
	{
		printf("on_disconnect received error\n");
                //reconnect
               //mosquitto_disconnect(mosq);
		//Resetting to trigger sync on wan_restore
		subscribeFlag = 0;
		bootupsync = 0;
		return;
        }
}
// callback called when the broker sends a SUBACK in response to a SUBSCRIBE.
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
        int i;
        bool have_subscription = false;

	printf("on_subscribe callback: qos_count %d\n", qos_count);
        //SUBSCRIBE can contain many topics at once
        for(i=0; i<qos_count; i++)
	{
                printf("on_subscribe: %d:granted qos = %d\n", i, granted_qos[i]);
                if(granted_qos[i] <= 2)
		{
                        have_subscription = true;
                }
		printf("on_subscribe: bootupsync %d\n", bootupsync);
		if(!bootupsync)
		{
			printf("mqtt is connected and subscribed to topic, trigger bootup sync to cloud.\n");
			int ret = 1;
			//int ret = triggerBootupSync();
			if(ret)
			{
				printf("Triggered bootup sync via mqtt\n");
			}
			else
			{
				printf("Failed to trigger bootup sync via mqtt\n");
			}
			bootupsync = 1;
		}
        }
        if(have_subscription == false)
	{
                printf("Error: All subscriptions rejected.\n");
                mosquitto_disconnect(mosq);
        }
}

/* callback called when the client receives a message. */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	//char* eventData[2] = { "Hello Earth", "Hello Mars" };
	if(msg !=NULL)
	{
		if(msg->payload !=NULL)
		{
			printf("Received message from %s qos %d payloadlen %d payload %s\n", msg->topic, msg->qos, msg->payloadlen, (char *)msg->payload);

			int dataSize = msg->payloadlen;
			char * data = malloc(sizeof(char) * dataSize+1);
			memset(data, 0, sizeof(char) * dataSize+1);
			data = memcpy(data, (char *) msg->payload, dataSize+1);
			data[dataSize] = '\0';

			printf("Received dataSize is %d\n", dataSize);
			printf("write to file /tmp/subscribe_message.bin\n");
			writeToDBFile("/tmp/subscribe_message.bin",(char *)data,dataSize);
			printf("write to file done\n");
			if(mqttdata)
			{
				free(mqttdata);
				mqttdata= NULL;
			}
			mqttdata = malloc(sizeof(char) * dataSize);
			memset(mqttdata, 0, sizeof(char) * dataSize);
			mqttdata = memcpy(mqttdata, data, dataSize );
			mqttdata_len = dataSize;
			free(data);
			data = NULL;

			if(subscribed1)
			{
			    rbusEvent_t event = {0};
			    rbusObject_t data1;
			    rbusValue_t value;

			    printf("publishing Event1\n");

			    rbusValue_Init(&value);
			    rbusValue_SetBytes(value, (uint8_t*)mqttdata, dataSize);

			    rbusObject_Init(&data1, NULL);
			    rbusObject_SetValue(data1, "blobdata", value);

			    event.name = dataElements[0].name;
			    event.data = data1;
			    event.type = RBUS_EVENT_GENERAL;

			    rbusError_t rc = rbusEvent_Publish(rbus_handle, &event);

			    rbusValue_Release(value);
			    rbusObject_Release(data1);

			    if(rc != RBUS_ERROR_SUCCESS)
				printf("provider: rbusEvent_Publish Event1 failed: %d\n", rc);
			}

		}
		else
		{
			printf("Received payload from mqtt is NULL\n");
		}
	}
	else
	{
		printf("Received message from mqtt is NULL\n");
	}
}

void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
        printf("Message with mid %d has been published.\n", mid);
}

/* This function pretends to read some data from a sensor and publish it.*/
void publish_notify_mqtt(char *pub_topic, void *payload, ssize_t len, char * dest)
{
        int rc;
	if(dest != NULL)
	{
		ssize_t payload_len = 0;
		char * pub_payload = createMqttPubHeader(payload, dest, &payload_len);
		if(pub_payload != NULL)
		{
			len = payload_len;
			free(payload);
			payload = (char *) malloc(sizeof(char) * 1024);
			payload = strdup(pub_payload);

			free(pub_payload);
		}
	}

	if(pub_topic == NULL)
	{
		char publish_topic[256] = { 0 };

		/*Get_Mqtt_LocationId(locationID);
		printf("locationID fetched from tr181 is %s\n", locationID);*/
		snprintf(publish_topic, MAX_MQTT_LEN, "%s%s/%s", MQTT_PUBLISH_NOTIFY_TOPIC_PREFIX, "E0DBD1DC8BFF","63a0fd5aad7e9a892c670333");
		if(strlen(publish_topic)>0)
		{
			printf("publish_topic fetched from tr181 is %s\n", publish_topic);
			pub_topic = strdup(publish_topic);
			printf("pub_topic from file is %s\n", pub_topic);
		}
		else
		{
			printf("Failed to fetch publish topic\n");
		}
	}
	else
	{
		printf("pub_topic is %s\n", pub_topic);
	}
	printf("Payload published is \n%s\n", (char*)payload);
	//writeToDBFile("/tmp/payload.bin", (char *)payload, len);
        rc = mosquitto_publish(mosq, NULL, pub_topic, len, payload, 2, false);
	printf("Publish rc %d\n", rc);
        if(rc != MOSQ_ERR_SUCCESS)
	{
                printf("Error publishing: %s\n", mosquitto_strerror(rc));
        }
	else
	{
		printf("Publish payload success %d\n", rc);
	}
	mosquitto_loop(mosq, 0, 1);
	printf("Publish mosquitto_loop done\n");
}

void get_from_file(char *key, char **val, char *filepath)
{
        FILE *fp = fopen(filepath, "r");

        if (NULL != fp)
        {
                char str[255] = {'\0'};
                while (fgets(str, sizeof(str), fp) != NULL)
                {
                    char *value = NULL;

                    if(NULL != (value = strstr(str, key)))
                    {
                        value = value + strlen(key);
                        value[strlen(value)-1] = '\0';
                        *val = strdup(value);
                        break;
                    }

                }
                fclose(fp);
        }

        if (NULL == *val)
        {
                printf("WebConfig val is not present in file\n");

        }
        else
        {
                printf("val fetched is %s\n", *val);
        }
}

/*int triggerBootupSync()
{
	char *mqttheaderList = NULL;
	mqttheaderList = (char *) malloc(sizeof(char) * 1024);
	char *pub_get_topic = NULL;

	if(mqttheaderList != NULL)
	{
		printf("B4 createMqttHeader\n");
		createMqttHeader(&mqttheaderList);
		if(mqttheaderList !=NULL)
		{
			printf("mqttheaderList generated is \n%s len %zu\n", mqttheaderList, strlen(mqttheaderList));
			char publish_get_topic[256] = { 0 };
			char locationID[256] = { 0 };*/
			/*Get_Mqtt_LocationId(locationID);
			printf("locationID is %s\n", locationID);*/
			//snprintf(publish_get_topic, MAX_MQTT_LEN, "%s%s/%s", MQTT_PUBLISH_GET_TOPIC_PREFIX, g_NodeID,locationID);
			/*snprintf(publish_get_topic, MAX_MQTT_LEN, "x/fr/get/chi/E0DBD1DC8BFF/63a0fd5aad7e9a892c670333");
			if(strlen(publish_get_topic) >0)
			{
				pub_get_topic = strdup(publish_get_topic);
				printf("pub_get_topic from tr181 is %s\n", pub_get_topic);
				publish_notify_mqtt(pub_get_topic, (void*)mqttheaderList, strlen(mqttheaderList), NULL);
				printf("triggerBootupSync published to topic %s\n", pub_get_topic);
			}
			else
			{
				printf("Failed to fetch publish_get_topic\n");
			}
		}
		else
		{
			printf("Failed to generate mqttheaderList\n");
			return 0;
		}
	}
	else
	{
		printf("Failed to allocate mqttheaderList\n");
		return 0;
	}
	printf("triggerBootupSync end\n");
	return 1;
}*/

void execute_mqtt_script(char *name)
{
    FILE* out = NULL, *file = NULL;
    char command[100] = {'\0'};

    if(strlen(name)>0)
    {
        file = fopen(name, "r");
        if(file)
        {
            snprintf(command,sizeof(command),"%s mqttcert-fetch", name);
            out = popen(command, "r");
            if(out)
            {
		printf("The Tls cert script executed successfully\n");
                pclose(out);

            }
            fclose(file);

        }
        else
        {
            printf ("File %s open error\n", name);
        }
    }
}

int getHostIPFromInterface(char *interface, char **ip)
{
	int file, rc;
	struct ifreq infr;

	file = socket(AF_INET, SOCK_DGRAM, 0);
	if(file)
	{
		infr.ifr_addr.sa_family = AF_INET;
		strncpy(infr.ifr_name, interface, IFNAMSIZ-1);
		rc = ioctl(file, SIOCGIFADDR, &infr);
		close(file);
		if(rc == 0)
		{
			printf("%s\n", inet_ntoa(((struct sockaddr_in *)&infr.ifr_addr)->sin_addr));
			*ip = inet_ntoa(((struct sockaddr_in *)&infr.ifr_addr)->sin_addr);
			return 1;
		}
		else
		{
			printf("Failed in ioctl command to get host ip\n");
		}
	}
	else
	{
		printf("Failed to get host ip from interface\n");
	}
	return 0;
}

rbusError_t webcfgMqttDataSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts)
{
	(void) handle;
	(void) opts;
	char const* paramName = rbusProperty_GetName(prop);

	if(strncmp(paramName, WEBCFG_MQTT_DATA_PARAM, maxParamLen) != 0)
	{
		printf("Unexpected parameter = %s\n", paramName);
		return RBUS_ERROR_ELEMENT_DOES_NOT_EXIST;
	}

	rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
	printf("Parameter name is %s \n", paramName);
	rbusValueType_t type_t;
	rbusValue_t paramValue_t = rbusProperty_GetValue(prop);
	if(paramValue_t) {
		type_t = rbusValue_GetType(paramValue_t);
	} else {
		printf("Invalid input to set\n");
		return RBUS_ERROR_INVALID_INPUT;
	}

	if(strncmp(paramName, WEBCFG_MQTT_DATA_PARAM, maxParamLen) == 0) {

		if(type_t == RBUS_BYTES) {
			//char* data = rbusValue_ToString(paramValue_t, NULL, 0);
			int len = 0;
			uint8_t* data = NULL;
			data = (uint8_t*)rbusValue_GetBytes(paramValue_t, &len);
			if(data) {
				printf("Call datamodel function  with data %s\n", data);

				if(mqttdata) {
					free(mqttdata);
					mqttdata= NULL;
				}
				mqttdata = malloc(sizeof(char) * len);
				memset(mqttdata, 0, sizeof(char) * len);
				mqttdata = memcpy(mqttdata, data, len );
				mqttdata_len = len;
				free(data);
				data = NULL;

				printf("mqttdata after processing %s\n", (char *)mqttdata);
				retPsmSet = rbus_StoreValueIntoDB( WEBCFG_MQTT_DATA_PARAM, (char *)mqttdata);
				if (retPsmSet != RBUS_ERROR_SUCCESS)
				{
					printf("psm_set failed ret %d for parameter %s and value %s\n", retPsmSet, paramName, (char *)mqttdata);
					return retPsmSet;
				}
				else
				{
					printf("psm_set success ret %d for parameter %s and value %s\n", retPsmSet, paramName, (char *)mqttdata);
				}
			}
		} else {
			printf("Unexpected value type for property %s\n", paramName);
			return RBUS_ERROR_INVALID_INPUT;
		}
	}
	return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgMqttDataGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts)
{

    (void) handle;
    (void) opts;
    char const* propertyName;
    rbusError_t retPsmGet = RBUS_ERROR_BUS_ERROR;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        printf("Property Name is %s \n", propertyName);
    } else {
        printf("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
	}
   if(strncmp(propertyName, WEBCFG_MQTT_DATA_PARAM, maxParamLen) == 0)
   {

	rbusValue_t value;
        rbusValue_Init(&value);

        if(mqttdata){
		//rbusValue_SetString(value, mqttdata);
		rbusValue_SetBytes(value, (uint8_t *)mqttdata, mqttdata_len);
	}
        else{
		retPsmGet = rbus_GetValueFromDB( WEBCFG_MQTT_DATA_PARAM, (char**)mqttdata );
		if (retPsmGet != RBUS_ERROR_SUCCESS){
			printf("psm_get failed ret %d for parameter %s and value %s\n", retPsmGet, propertyName, (char *)mqttdata);
			if(value)
			{
				rbusValue_Release(value);
			}
			return retPsmGet;
		}
		else{
			printf("psm_get success ret %d for parameter %s and value %s\n", retPsmGet, propertyName, (char *)mqttdata);
			if(mqttdata)
			{
				rbusValue_SetBytes(value, (uint8_t*)mqttdata, mqttdata_len);
			}
			else
			{
				printf("mqttdata is empty\n");
				/*uint8_t * val = NULL;
				rbusValue_SetBytes(value, val, 0);
				printf("mqttvalue after set\n");*/
			}
		}
	}
        rbusProperty_SetValue(property, value);
        rbusValue_Release(value);

    }
printf("before success\n");
    return RBUS_ERROR_SUCCESS;
}

int regMqttDataModel()
{
	int ret = RBUS_ERROR_SUCCESS;   

	printf("rbus_open for component %s\n", pComponentName);
	ret = rbus_open(&rbus_handle, pComponentName);
	if(ret != RBUS_ERROR_SUCCESS)
	{
		printf("regMqttDataModel failed with error code %d\n", ret);
		return ret;
	}
	printf("regMqttDataModel is success. ret is %d\n", ret);

	ret = rbus_regDataElements(get_global_rbus_handle(), NUM_WEBCFG_ELEMENTS3, dataElements);
	return ret;
}

char * createMqttPubHeader(char * payload, char * dest, ssize_t * payload_len)
{
	char * destination = NULL;
	char * content_type = NULL;
	char * content_length = NULL;
	char *pub_headerlist = NULL;

	pub_headerlist = (char *) malloc(sizeof(char) * 1024);

	if(pub_headerlist != NULL)
	{
		if(payload != NULL)
		{
			if(dest != NULL)
			{
				destination = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
				if(destination !=NULL)
				{
					snprintf(destination, MAX_BUF_SIZE, "Destination: %s", dest);
					printf("destination formed %s\n", destination);
				}
			}

			content_type = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
			if(content_type !=NULL)
			{
				snprintf(content_type, MAX_BUF_SIZE, "\r\nContent-type: application/json");
				printf("content_type formed %s\n", content_type);
			}

			content_length = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
			if(content_length !=NULL)
			{
				snprintf(content_length, MAX_BUF_SIZE, "\r\nContent-length: %zu", strlen(payload));
				printf("content_length formed %s\n", content_length);
			}

			printf("Framing publish notification header\n");
			snprintf(pub_headerlist, 1024, "%s%s%s\r\n\r\n%s\r\n", (destination!=NULL)?destination:"", (content_type!=NULL)?content_type:"", (content_length!=NULL)?content_length:"",(payload!=NULL)?payload:"");
	    }
	}
	printf("mqtt pub_headerlist is \n%s", pub_headerlist);
	*payload_len = strlen(pub_headerlist);
	return pub_headerlist;
}

int writeToDBFile(char *db_file_path, char *data, size_t size)
{
	FILE *fp;
	fp = fopen(db_file_path , "w+");
	if (fp == NULL)
	{
		printf("Failed to open file in db %s\n", db_file_path );
		return 0;
	}
	if(data !=NULL)
	{
		fwrite(data, size, 1, fp);
		fclose(fp);
		return 1;
	}
	else
	{
		printf("WriteToJson failed, Data is NULL\n");
		fclose(fp);
		return 0;
	}
}

int main()
{
	regMqttDataModel();
	webcfg_mqtt_init();
	printf("After mqtt init\n");
	return 0;
}
