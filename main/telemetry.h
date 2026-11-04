#ifndef TELEMETRY_H
#define TELEMETRY_H

/**
 * @brief Initialize the telemetry module.
 *
 * Publishes identity on MQTT connect and health periodically.
 * Must be called after mqtt_app_start().
 */
void telemetry_init(void);

#endif // TELEMETRY_H
