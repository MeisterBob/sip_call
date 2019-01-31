#ifndef MQTT_TASK_H
#define MQTT_TASK_H

#include "mqtt_client.h"

// MQTT
esp_mqtt_client_handle_t mqtt_client;
const char* mqttOutTopic = CONFIG_TOPIC_OUT;
const char* mqttInTopic = CONFIG_TOPIC_IN;
const char* mqttStatusTopic = CONFIG_TOPIC_STATUS;
const char* mqttVersionTopic = CONFIG_TOPIC_VERSION;
const char* mqttWillMessage = "offline";

typedef enum { DING, DONG, TEST } mqtt_out_msg_t;

#endif