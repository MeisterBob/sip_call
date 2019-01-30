#ifndef MAIN_H
#define MAIN_H

#define VERSION "1.9"

#if CONFIG_POWER_SAVE_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MODEM
#elif CONFIG_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif

// some settings from menuconfig
static constexpr auto GPIO_LEDFLASH = static_cast<gpio_num_t>(CONFIG_FLASHLED);
static constexpr auto BELL_GPIO_PIN = static_cast<gpio_num_t>(CONFIG_BELL_INPUT_GPIO);
static constexpr auto DOOR_GPIO_PIN = static_cast<gpio_num_t>(CONFIG_DOOR_OUTPUT_GPIO);

static constexpr auto RING_DURATION_TIMEOUT_MSEC = CONFIG_RING_DURATION;
static constexpr auto DOOR_DURATION_TIMEOUT_MSEC = CONFIG_DOOR_DURATION;
char DOORCODE[5] = {CONFIG_DOORCODE};
uint32_t CODE_POS = 0;
static xQueueHandle door_opener_queue = NULL;

// MQTT
esp_mqtt_client_handle_t mqtt_client;
const char* mqttOutTopic = "test/haus/klingel/set";
const char* mqttInTopic = "test/haus/klingel";
const char* mqttStatusTopic = "test/haus/klingel/status";
const char* mqttVersionTopic = "test/haus/klingel/version";
//const char* mqttIpTopic = "test/haus/klingel/ip";

const char* mqttWillMessage = "offline";

// httpserver
static void handle_jpg(http_context_t http_ctx, void* ctx);

/* FreeRTOS event group to signal when we are connected properly */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int CONNECTED_BIT = BIT0;
static ip4_addr_t s_ip_addr;

static const char *TAG = "main";

using SipClientT = SipClient<LwipUdpClient, MbedtlsMd5>;

SipClientT s_client{CONFIG_SIP_USER, CONFIG_SIP_PASSWORD, CONFIG_SIP_SERVER_IP, CONFIG_SIP_SERVER_PORT, CONFIG_LOCAL_IP};

#endif