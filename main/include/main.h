#ifndef MAIN_H
#define MAIN_H

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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

static xQueueHandle door_opener_queue = NULL;
static xQueueHandle mqtt_queue = NULL;

#endif