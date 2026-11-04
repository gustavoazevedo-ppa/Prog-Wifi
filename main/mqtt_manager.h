#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include "mqtt_client.h"
#include "esp_netif.h"

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0xFB                      // 251 bytes
extern uint8_t mqtt_fifo_buffer[GATTS_DEMO_CHAR_VAL_LEN_MAX]; //+1??
extern int mqtt_fifo_index;

extern esp_mqtt_client_handle_t mqtt_client;

void mqtt_app_start(void);
int mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain);
int mqtt_manager_publish_default(const char *data, int len, int qos, int retain);

// Central MAC management (stores MAC in NVS, builds topics from it)
bool mqtt_manager_set_central_mac(const char *mac);
const char *mqtt_manager_get_central_mac(void);
bool mqtt_manager_has_central_mac(void);

// Load central MAC from NVS (call early at boot, before mqtt_app_start)
void mqtt_manager_load_mac(void);

bool mqtt_manager_online(void);
bool mqtt_manager_connected(void);

// Network interface failover: register both netifs so MQTT can swap on disconnect
void netif_failover_init(esp_netif_t *wifi_netif, esp_netif_t *eth_netif);