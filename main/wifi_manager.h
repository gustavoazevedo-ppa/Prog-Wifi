#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "esp_netif.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

void wifi_init_sta(void);
bool wifi_store_credentials(const char *ssid, const char *pass);
bool wifi_load_credentials(char *ssid_out, size_t ssid_out_len,
                           char *pass_out, size_t pass_out_len);
bool wifi_clear_credentials(void);
bool wifi_update_credentials(const char *ssid, const char *pass);
esp_netif_t *wifi_get_netif(void);