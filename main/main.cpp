/*
   Copyright 2017 Christian Taedcke <hacking@taedcke.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */
// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ESP32
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// default librarys
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// components
#include "mqtt_task.h"
#include "app_camera.h"
#include "http_server.h"
#include "sip_client/lwip_udp_client.h"
#include "sip_client/mbedtls_md5.h"
#include "sip_client/sip_client.h"
#include "button_handler.h"

#include "main.h"

static const char *TAG = "main";

static void handle_jpg(http_context_t http_ctx, void* ctx);

using SipClientT = SipClient<LwipUdpClient, MbedtlsMd5>;
SipClientT s_client{CONFIG_SIP_USER, CONFIG_SIP_PASSWORD, CONFIG_SIP_SERVER_IP, CONFIG_SIP_SERVER_PORT, CONFIG_LOCAL_IP};

const int CONNECTED_BIT = BIT0;
static ip4_addr_t s_ip_addr;

uint32_t CODE_POS = 0;

static EventGroupHandle_t wifi_event_group;

ButtonInputHandler<SipClientT, BELL_GPIO_PIN, RING_DURATION_TIMEOUT_MSEC> button_input_handler(s_client);

/*************************************************************************************************************************/

static std::string ip_to_string(const ip4_addr_t *ip) {
    static constexpr size_t BUFFER_SIZE = 16;
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, IPSTR, IP2STR(ip));
    return std::string(buffer);
}

static std::string get_gw_ip_address(const system_event_sta_got_ip_t *got_ip) {
    const ip4_addr_t *gateway = &got_ip->ip_info.gw;
    return ip_to_string(gateway);
}

static std::string get_local_ip_address(const system_event_sta_got_ip_t *got_ip) {
    const ip4_addr_t *local_addr = &got_ip->ip_info.ip;
    return ip_to_string(local_addr);
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP: {
        system_event_sta_got_ip_t *got_ip = &event->event_info.got_ip;
        s_client.set_server_ip(get_gw_ip_address(got_ip));
        s_client.set_my_ip(get_local_ip_address(got_ip));
        s_ip_addr = event->event_info.got_ip.ip_info.ip;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
    break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    int msg_id;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
                ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
                esp_mqtt_client_publish(mqtt_client, mqttStatusTopic, "online", 0, 0, 1);
                esp_mqtt_client_publish(mqtt_client, mqttVersionTopic, VERSION, 0, 0, 1);
                msg_id = esp_mqtt_client_subscribe(mqtt_client, mqttInTopic, 0);
                ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
                ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
                ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
                ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
                ESP_LOGI(TAG, "MQTT_EVENT_DATA");
                //printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
                //printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
                ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
    }
    return ESP_OK;
}

static void initialize_wifi(void) {
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.bssid_set = false;

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );

    esp_wifi_set_ps(DEFAULT_PS_MODE);
}

void led_init() {
    gpio_pad_select_gpio(GPIO_LEDFLASH);
    gpio_set_direction(GPIO_LEDFLASH, GPIO_MODE_OUTPUT);
}

static void sip_task(void *pvParameters) {
    for(;;)     {
        // Wait for wifi connection
        EventBits_t uxBits;
        uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 3000 / portTICK_RATE_MS);

        if (!(uxBits & CONNECTED_BIT)) {
            ESP_LOGI(TAG, "Wifi connection failed - retrying");
            esp_wifi_connect();
            continue;
        }

        if (!s_client.is_initialized())         {
            bool result = s_client.init();
            ESP_LOGI(TAG, "SIP client initialized %ssuccessfully", result ? "" : "un");
            if (!result)             {
                ESP_LOGI(TAG, "Waiting to try again...");
                vTaskDelay(2000 / portTICK_RATE_MS);
                continue;
            }
            s_client.set_event_handler([](const SipClientEvent& event) {
                uint32_t data = 0;
                switch (event.event) {
                    case SipClientEvent::Event::CALL_START:
                        ESP_LOGI(TAG, "Call start");
                        CODE_POS = 0;
                    break;
                    case SipClientEvent::Event::CALL_END:
                        ESP_LOGI(TAG, "Call end");
                        button_input_handler.call_end();
                        CODE_POS = 0;
                    break;
                    case SipClientEvent::Event::CALL_CANCELLED:
                        ESP_LOGI(TAG, "Call cancelled, reason %d", (int) event.cancel_reason);
                        button_input_handler.call_end();
                        break;
                    case SipClientEvent::Event::BUTTON_PRESS:
                        ESP_LOGI(TAG, "Got button press: %c for %d milliseconds", event.button_signal, event.button_duration);
                        if(event.button_signal == DOORCODE[CODE_POS]) CODE_POS++; else CODE_POS=0;
                        if (CODE_POS==4) {
                            CODE_POS = 0;
                            xQueueSendToBack(door_opener_queue, &data, (TickType_t) 10);
                        }
                    break;
                }
            });
        }

        s_client.run();
    }
}

static void door_opener_task(void* arg) {
    gpio_pad_select_gpio(DOOR_GPIO_PIN);
    gpio_set_direction(DOOR_GPIO_PIN, GPIO_MODE_OUTPUT);
    uint32_t data=0;
    static constexpr uint32_t DOOR_DURATION_TICKS = DOOR_DURATION_TIMEOUT_MSEC / portTICK_PERIOD_MS;
    for(;;) {
        if(xQueueReceive(door_opener_queue, (void*) &data, portMAX_DELAY)) {
            gpio_set_level(DOOR_GPIO_PIN, 1);
            ESP_LOGI("DOOR_OPENER_HANDLER", "door unlocked");
            vTaskDelay(DOOR_DURATION_TICKS);
            gpio_set_level(DOOR_GPIO_PIN, 0);
            ESP_LOGI("DOOR_OPENER_HANDLER", "door locked");
        }
    }
}

static void app_mqtt(void* arg) {
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.event_handle = mqtt_event_handler;
    mqtt_cfg.uri = CONFIG_BROKER_URL;
    mqtt_cfg.lwt_topic = mqttStatusTopic;
    mqtt_cfg.lwt_msg = mqttWillMessage;
    mqtt_cfg.lwt_retain = 1;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(mqtt_client);
    mqtt_out_msg_t data;
    for(;;) {
        if(xQueueReceive(mqtt_queue, &data, portMAX_DELAY)) {
            switch (data) {
                case DING: esp_mqtt_client_publish(mqtt_client, mqttOutTopic, "1", 0, 0, 0); break;
                case DONG: esp_mqtt_client_publish(mqtt_client, mqttOutTopic, "0", 0, 0, 0); break;
                case TEST: esp_mqtt_client_publish(mqtt_client, mqttOutTopic, "test", 0, 0, 0); break;
            }
        }
    }
}

static esp_err_t write_frame(http_context_t http_ctx, camera_fb_t * fb) {
    http_buffer_t fb_data = {
            .data = fb->buf,
            .size = fb->len,
            .data_is_persistent = true
    };
    return http_response_write(http_ctx, &fb_data);
}

static void handle_jpg(http_context_t http_ctx, void* ctx) {
    ESP_LOGI(TAG, "handle jpg");

    gpio_set_level(GPIO_LEDFLASH, 1);
	vTaskDelay(50 / portTICK_PERIOD_MS);

    camera_fb_t * fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Camera capture failed");
        return;
    } else {
        http_response_begin(http_ctx, 200, "image/jpeg", fb->len);
        http_response_set_header(http_ctx, "Content-disposition", "inline; filename=capture.jpg");
        write_frame(http_ctx, fb);
        http_response_end(http_ctx);
    }

    gpio_set_level(GPIO_LEDFLASH, 0);
}

extern "C" void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("http_server", ESP_LOG_WARN);
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("camera", ESP_LOG_WARN);
    esp_log_level_set("camera_xclk", ESP_LOG_WARN);
    esp_log_level_set("SipClient", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ESP_ERROR_CHECK( nvs_flash_init() );
    }

    ESP_LOGD(TAG, "initialize WIFI");
    initialize_wifi();

    ESP_LOGD(TAG, "initialize camera");
    app_camera_init();

    ESP_LOGD(TAG, "initialize sip client");
    std::srand(esp_random());
    xTaskCreate(&sip_task, "sip_task", 4096, NULL, 5, NULL);

    ESP_LOGD(TAG, "initialize door opener");
    door_opener_queue = xQueueCreate(1, sizeof(uint32_t));
    xTaskCreate(&door_opener_task, "door_opener_task", 4096, NULL, 5, NULL);

    ESP_LOGD(TAG, "initialize HTTP server");
    http_server_t server;
    http_server_options_t http_options = HTTP_SERVER_OPTIONS_DEFAULT();
    ESP_ERROR_CHECK( http_server_start(&http_options, &server) );
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 5000 / portTICK_RATE_MS);
    ESP_ERROR_CHECK( http_register_handler(server, "/capture.jpg", HTTP_GET, HTTP_HANDLE_RESPONSE, &handle_jpg, NULL) );
    ESP_LOGI(TAG, "Open http://" IPSTR "/capture.jpg for single image/jpg image", IP2STR(&s_ip_addr));

    ESP_LOGD(TAG, "initialize LED Flash");
    led_init();

    ESP_LOGD(TAG, "initialize MQTT client");
    mqtt_queue = xQueueCreate(1, sizeof(mqtt_out_msg_t));
    xTaskCreate(&app_mqtt, "mqtt_task", 4096, NULL, 5, NULL);

    //blocks forever
    ESP_LOGD(TAG, "initialize button handler");
    button_input_handler.run();
}
