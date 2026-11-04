#ifndef OTA_MQTT_H
#define OTA_MQTT_H
#include <stdbool.h>

/**
 * @brief Initialize MQTT OTA command handling.
 *
 * Subscribes to cmd/ppa/<central_mac>/ota and triggers OTA on command.
 * Publishes status to dt/ppa/<central_mac>/ota_status.
 *
 * Must be called after MQTT is connected and central MAC is available.
 */
void ota_mqtt_init(void);

/**
 * @brief Handle an incoming MQTT message that might be an OTA command.
 *
 * Call this from the MQTT event handler for every incoming message.
 * Returns true if the message was an OTA command (consumed), false otherwise.
 */
bool ota_mqtt_handle_message(const char *topic, int topic_len,
                             const char *data, int data_len);

#endif // OTA_MQTT_H
