#include "packet_manager.h"

bool isCmd40_received(void);
bool isCmd43_received(void);
bool isCmd10_received(void);
bool isCmd46_received(void);

// Action handlers
bool handle_ble_packet(packet_t *packet);
bool handle_config_packet(packet_t *packet);
bool handle_default_packet(packet_t *packet);

void packet_actions_init(void);