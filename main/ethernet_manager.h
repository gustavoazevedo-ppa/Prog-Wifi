#pragma once
#include "esp_netif.h"
#include <stdint.h>
#include <stdbool.h>

void ethernet_init(void);
esp_netif_t *ethernet_get_netif(void);

// Static IP configuration for Ethernet interface
// All uint32_t values are in network byte order (as returned by esp_netif_str_to_ip4)
bool ethernet_set_static_ip(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2);
bool ethernet_clear_static_ip(void);