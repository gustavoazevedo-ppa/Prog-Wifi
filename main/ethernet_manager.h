#pragma once
#include "esp_netif.h"
#include <stdint.h>
#include <stdbool.h>

void ethernet_init(void); // no-op when ETHERNET_ENABLED is 0 in ethernet_manager.c
bool ethernet_is_enabled(void);
esp_netif_t *ethernet_get_netif(void);

// Physical link state. Returns true when the link is up; then *speed_mbps (10 or 100)
// and *full_duplex are filled in (either pointer may be NULL). When the link is down
// (or the driver is not installed) returns false and leaves the outputs untouched.
bool ethernet_get_link(int *speed_mbps, bool *full_duplex);

// Static IP configuration for Ethernet interface
// All uint32_t values are in network byte order (as returned by esp_netif_str_to_ip4)
bool ethernet_set_static_ip(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2);
bool ethernet_clear_static_ip(void);