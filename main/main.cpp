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

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "sip_client/lwip_udp_client.h"
#include "sip_client/mbedtls_md5.h"
#include "sip_client/sip_client.h"

#include "button_handler.h"

#include <string.h>


static constexpr auto BELL_GPIO_PIN = static_cast<gpio_num_t>(CONFIG_BELL_INPUT_GPIO);
// GPIO_NUM_23 is connected to the opto coupler
static constexpr auto RING_DURATION_TIMEOUT_MSEC = CONFIG_RING_DURATION;

static constexpr auto DOOR_GPIO_PIN = static_cast<gpio_num_t>(CONFIG_DOOR_OUTPUT_GPIO);
static constexpr auto DOOR_DURATION_TIMEOUT_MSEC = CONFIG_DOOR_DURATION;
char DOORCODE[5] = {CONFIG_DOORCODE};
uint32_t CODE_POS = 0;
static xQueueHandle door_opener_queue = NULL;

#if CONFIG_POWER_SAVE_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MODEM
#elif CONFIG_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/



/* FreeRTOS event group to signal when we are connected properly */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static const char *TAG = "main";

using SipClientT = SipClient<LwipUdpClient, MbedtlsMd5>;

SipClientT client{CONFIG_SIP_USER, CONFIG_SIP_PASSWORD, CONFIG_SIP_SERVER_IP, CONFIG_SIP_SERVER_PORT, CONFIG_LOCAL_IP};

static std::string ip_to_string(const ip4_addr_t *ip)
{
    static constexpr size_t BUFFER_SIZE = 16;
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, IPSTR, IP2STR(ip));
    return std::string(buffer);
}

static std::string get_gw_ip_address(const system_event_sta_got_ip_t *got_ip)
{
    const ip4_addr_t *gateway = &got_ip->ip_info.gw;
    return ip_to_string(gateway);
}

static std::string get_local_ip_address(const system_event_sta_got_ip_t *got_ip)
{
    const ip4_addr_t *local_addr = &got_ip->ip_info.ip;
    return ip_to_string(local_addr);
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
    {
        system_event_sta_got_ip_t *got_ip = &event->event_info.got_ip;
        client.set_server_ip(get_gw_ip_address(got_ip));
        client.set_my_ip(get_local_ip_address(got_ip));
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

static void initialize_wifi(void)
{
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

ButtonInputHandler<SipClientT, BELL_GPIO_PIN, RING_DURATION_TIMEOUT_MSEC> button_input_handler(client);

static void sip_task(void *pvParameters)
{
    for(;;)
    {
        // Wait for wifi connection
        EventBits_t uxBits;
        uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 10000 / portTICK_RATE_MS);

        if (!(uxBits & CONNECTED_BIT)) {
            ESP_LOGI(TAG, "Wifi connection failed - retrying");
            esp_wifi_connect();
            continue;
        }

        if (!client.is_initialized())
        {
            bool result = client.init();
            ESP_LOGI(TAG, "SIP client initialized %ssuccessfully", result ? "" : "un");
            if (!result)
            {
                ESP_LOGI(TAG, "Waiting to try again...");
                vTaskDelay(2000 / portTICK_RATE_MS);
                continue;
            }
            client.set_event_handler([](const SipClientEvent& event) {
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

        client.run();
    }
}

static void door_opener_task(void* arg) {
    gpio_pad_select_gpio(DOOR_GPIO_PIN);
    gpio_set_direction(DOOR_GPIO_PIN, GPIO_MODE_OUTPUT);
    uint32_t data=0;
    static constexpr uint32_t DOOR_DURATION_TICKS = DOOR_DURATION_TIMEOUT_MSEC / portTICK_PERIOD_MS;
    for(;;) {
        if(xQueueReceive(door_opener_queue, (void*) &data, portMAX_DELAY)) {
            gpio_set_level((gpio_num_t) CONFIG_DOOR_OUTPUT_GPIO, 1);
            ESP_LOGI("DOOR_OPENER_HANDLER", "door unlocked");
            vTaskDelay(DOOR_DURATION_TICKS);
            gpio_set_level((gpio_num_t) CONFIG_DOOR_OUTPUT_GPIO, 0);
            ESP_LOGI("DOOR_OPENER_HANDLER", "door locked");
        }
    }
}

extern "C" void app_main(void)
{
    nvs_flash_init();
    initialize_wifi();

    std::srand(esp_random());
    xTaskCreate(&sip_task, "sip_task", 4096, NULL, 5, NULL);

    door_opener_queue = xQueueCreate(1, sizeof(uint32_t));
    xTaskCreate(&door_opener_task, "door_opener_task", 4096, NULL, 5, NULL);

    //blocks forever
    button_input_handler.run();
}
