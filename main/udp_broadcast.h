#pragma once
#include <stdint.h>
#include <stddef.h>

// Start the periodic UDP broadcast task
// payload/len define what gets sent each cycle on all active interfaces
void udp_broadcast_start(const uint8_t *payload, size_t len, uint16_t port);
