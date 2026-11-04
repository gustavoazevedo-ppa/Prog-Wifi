#pragma once
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>

// Packet source enum
typedef enum {
  PACKET_SOURCE_UART = 0,
  PACKET_SOURCE_BLE = 1,
  PACKET_SOURCE_MQTT = 2
} packet_source_t;

#define PKT_TYPE_BLE 0x00       // BLE
#define PKT_TYPE_CLOUD 0x01     // MQTT (AES Encrypted)
#define PKT_TYPE_CONFIG 0x02    // Plain-text
#define PKT_TYPE_CONTACTID 0x03 // Contact ID protocol for alarm panels

#define PKT_CMD_40 0x28 // Set central MAC
#define PKT_CMD_41 0x29 // MQTT connection status notification
#define PKT_CMD_43 0x2B // WiFi configuration
#define PKT_CMD_44 0x2C // Connection type LTE/WiFi (discontinued)
#define PKT_CMD_45 0x2D // MQTT status query/response
#define PKT_CMD_46 0x2E // Camera configuration (add/update/delete)
#define PKT_CMD_47 0x2F // Camera motion violation event
#define PKT_CMD_48 0x30 // Camera online bitmask (status notification/query)
#define PKT_CMD_49 0x31 // Request camera configs from central
#define PKT_CMD_A0 0xA0 // Delete camera by id (single-byte param: low byte of id_cam; all matching slots erased)

// Contact ID / MONIP commands
#define PKT_CMD_01H 0x01 // Send CID event (receiver A)
#define PKT_CMD_02H 0x02 // Send CID event (receiver B)
#define PKT_CMD_03H 0x03 // Send CID event (both receivers)
#define PKT_CMD_10H 0x10 // MONIP receiver configuration
#define PKT_CMD_20H 0x20 // MONIP config query
#define PKT_CMD_30H 0x30 // MONIP connect to receiver
#define PKT_CMD_32H 0x32 // MONIP keep alive
#define PKT_CMD_35H 0x35 // MONIP forward response to receiver
#define PKT_CMD_38H 0x38 // MONIP disconnect command
#define PKT_CMD_40H 0x40 // MONIP disconnect notify

typedef enum {
  CONNECTION_NOT = 0,
  CONNECTION_LTE,
  CONNECTION_WIFI
} type_connection_t;

// Packet structure
typedef struct {
  packet_source_t source;
  uint8_t *data;
  int length;
  uint8_t packet_type;
} packet_t;

// Action handlers
typedef bool (*packet_handler_t)(packet_t *packet);

// Register/unregister handlers
void packet_manager_init(void);
bool packet_manager_submit(packet_t *pkt, TickType_t ticks_to_wait);
bool packet_manager_submit_packet(packet_source_t source, uint8_t *data,
                                  int length, uint8_t packet_type,
                                  TickType_t ticks_to_wait);
void packet_manager_register_handler(uint8_t packet_type,
                                     packet_handler_t handler);
uint8_t crc8(uint8_t *data, uint16_t len);