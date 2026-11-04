#include "packet_actions.h"
#include "ble_gatt.h"
#include "fw_slot_sender.h"
#include "monip_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_manager.h"
#include "lwip/inet.h"
#include "mqtt_manager.h"
#include "nvs.h"
#include "onvif_events.h"
#include "uart_handler.h"
#include "udp_broadcast.h"
#include "wifi_manager.h"


#define TAG "PKT_ACTIONS"

/**
 * Strip leading zeros from each octet of an IPv4 string to prevent
 * lwIP's ip4addr_aton from interpreting them as octal.
 * Example: "192.168.010.001" → "192.168.10.1"
 * Returns true if the result looks like a valid IPv4 (4 octets, digits only).
 */
static bool sanitize_ipv4_string(const char *input, char *output,
                                 size_t out_size) {
  if (!input || !output || out_size < 8) {
    if (output && out_size > 0)
      output[0] = '\0';
    return false;
  }

  const char *p = input;
  char *out = output;
  char *end = output + out_size - 1;
  int octets = 0;

  for (int octet = 0; octet < 4 && *p && out < end; octet++) {
    // Skip leading zeros (but keep at least one digit)
    while (*p == '0' && *(p + 1) != '.' && *(p + 1) != '\0')
      p++;

    // Copy digits
    bool has_digit = false;
    while (*p >= '0' && *p <= '9' && out < end) {
      *out++ = *p++;
      has_digit = true;
    }

    if (!has_digit) {
      output[0] = '\0';
      return false;
    }

    octets++;

    // Expect dot between octets, nothing after the 4th
    if (octet < 3) {
      if (*p != '.') {
        output[0] = '\0';
        return false;
      }
      if (out < end)
        *out++ = '.';
      p++;
    }
  }

  *out = '\0';

  if (octets != 4 || (*p != '\0' && *p != ' '))
    return false;

  return true;
}

// static bool mqtt_forwarding_enabled = true;
static bool cmd40_received = false;
static bool cmd43_received = false;
static bool cmd10_received = false;
static bool cmd46_received = false;

bool isCmd40_received(void) { return cmd40_received; }

bool isCmd43_received(void) { return cmd43_received; }

bool isCmd10_received(void) { return cmd10_received; }

bool isCmd46_received(void) { return cmd46_received; }

#define PKT_DATA_OFFSET (2 + 1 + 1 + 1) // header + length + type + cmd

bool handle_default_packet(packet_t *packet) {
  ESP_LOGI(TAG, "Default packet received with type = %d from source %d",
           packet->packet_type, packet->source);
  switch (packet->source) {
  case PACKET_SOURCE_UART:
    // if (mqtt_forwarding_enabled && packet->packet_type == PKT_TYPE_CLOUD)
    // {
    mqtt_manager_publish_default((const char *)packet->data, packet->length, 1,
                                 0);
    // }
    // else
    // {
    //     ESP_LOGI(TAG, "MQTT forwarding is disabled, not publishing UART
    //     packet to MQTT");
    // }
    // ble_send_response(packet->data, packet->length);
    break;
  case PACKET_SOURCE_BLE:
    // uart_handler_write_bytes(packet->data, packet->length);
    ESP_LOGI(TAG, "BLE packet with type = %d ignoring", packet->packet_type);
    break;
  case PACKET_SOURCE_MQTT:
    // if (mqtt_forwarding_enabled)
    // {
    uart_handler_write_bytes(packet->data, packet->length);
    // }
    // else
    // {
    //     ESP_LOGI(TAG, "MQTT forwarding is disabled, not sending MQTT packet
    //     to UART");
    // }
    break;
  default:
    break;
  }
  return true;
}

bool handle_ble_packet(packet_t *packet) {
  ESP_LOGI(TAG, "BLE packet received");
  switch (packet->source) {
  case PACKET_SOURCE_UART:
    ble_send_response(packet->data, packet->length);
    break;
  case PACKET_SOURCE_BLE:
    uart_handler_write_bytes(packet->data, packet->length);
    break;
  case PACKET_SOURCE_MQTT:
    ESP_LOGI(TAG, "MQTT packet with type = %d ignoring", packet->packet_type);
    break;
  default:
    break;
  }
  return true;
}

bool handle_config_packet(packet_t *packet) {
  uint8_t crc, crc_calc;

  // Decode command
  if (packet->length < 2) {
    ESP_LOGW(TAG, "Command packet too short");
    return false;
  }

  uint8_t length = packet->data[2];
  length += PKT_DATA_OFFSET;

  uint8_t cmd = packet->data[4]; // cmd is after header, length and type

  ESP_LOGI(TAG, "Handling packet type 0x%02X from source %d cmd=%d",
           packet->packet_type, packet->source, cmd);

  switch (cmd) {
  case FW_SLOT_CMD: {
    // Central firmware-update ACK (type 0x02, cmd 0x0A).
    // Frame: AA AA | len(1) | type | 0x0A | status | crc8 | BB BB
    // status is the byte right after cmd (data[5]).
    if (packet->length < 6) {
      ESP_LOGW(TAG, "FW ACK packet too short");
      break;
    }
    uint8_t status = packet->data[5];
    ESP_LOGI(TAG, "FW slot ACK received, status=0x%02X", status);
    fw_slot_sender_deliver_ack(status);
    break;
  }
  case PKT_CMD_40:                         // Set central MAC command
    if (packet->length <= PKT_DATA_OFFSET) // header + length + type + cmd
    {
      ESP_LOGW(TAG, "Set MAC packet too short");
      break;
    } else {
      crc =
          packet
              ->data[length]; // CRC is after header, length, type, cmd and data
      crc_calc = crc8(
          &packet->data[4],
          length - 4); // Calculate CRC starting from cmd byte to end of data
      if (crc != crc_calc) {
        ESP_LOGI(TAG, "Packet CRC Checksum Error. CRC=%02X, Calculated=%02X",
                 crc, crc_calc);
        break;
      }

      // Extract MAC string from payload (starts at byte 6, after
      // header+length+type+cmd+status)
      char mac_str[18] = {0};      // "aa:bb:cc:dd:ee:ff" + null
      packet->data[length] = '\0'; // Ensure null-terminated
      strncpy(mac_str, (const char *)(packet->data + 6), sizeof(mac_str) - 1);

      ESP_LOGI(TAG, "CMD_40 received, central MAC: %s", mac_str);

      // Store MAC and rebuild MQTT topics
      mqtt_manager_set_central_mac(mac_str);

      // Set BLE device name from MAC and start advertising
      char ble_name[24] = {0};
      snprintf(ble_name, sizeof(ble_name), "PPA_%s", mac_str);
      ble_rename_device(ble_name);

      // Start or update UDP broadcast with the central MAC
      char discovery_msg[24] = {0};
      snprintf(discovery_msg, sizeof(discovery_msg), "PPA_%s\r\n", mac_str);
      udp_broadcast_start((const uint8_t *)discovery_msg, strlen(discovery_msg),
                          30303);

      if (!mqtt_manager_online()) {
        mqtt_app_start();
      }
      cmd40_received = true;

      //Send CMD40 to signal status disconnected on both monip clients

      uint8_t response[] = {0xAA, 0xAA,   0x00, 0x01, 0x03,
                            PKT_CMD_40H, 0x00, 0x00, 0xBB, 0xBB};
      response[7] = crc8(&response[5], 2); // CRC over CMD + status
      uart_handler_write_bytes(response, sizeof(response));
      ESP_LOGI(TAG, "CMD_40H STATUS_RESET sent: status = 0x00");
    }
    break;
  case PKT_CMD_41:
    // CMD_41 is sent by MQTT handler when connection status changes, so we
    // don't expect to receive it from UART or BLE. Just log and ignore if it
    // happens.
    ESP_LOGI(TAG,
             "Received CMD_41 config packet, which is unexpected. Ignoring.");
    break;
  case PKT_CMD_43: // WiFi configuration command
    // Check minimum packet size
    if (packet->length <= PKT_DATA_OFFSET) {
      ESP_LOGW(TAG, "WiFi config packet too short");
      break;
    }

    // Calculate total length including header/cmd
    uint8_t config_length = packet->data[2];
    config_length += PKT_DATA_OFFSET;

    // Validate CRC
    crc = packet->data[config_length];
    crc_calc = crc8(&packet->data[4], config_length - 4);
    if (crc != crc_calc) {
      ESP_LOGI(TAG, "Packet CRC Checksum Error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    // Extract WiFi config from payload (starting at packet->data[6])
    // Full payload structure (159 bytes, status byte is at packet->data[5]):
    // [0-31]:   ssid (32 bytes)
    // [32-94]:  senha/password (63 bytes)
    // [95-110]: ip (16 bytes)
    // [111-126]: mascara/subnet mask (16 bytes)
    // [127-142]: gateway (16 bytes)
    // [143-149]: dns1 (7 bytes)
    // [150-156]: dns2 (7 bytes)
    // [157]:    config (1 byte)

    uint8_t *payload = &packet->data[6];

    char ssid[33] = {0};
    memcpy(ssid, &payload[0], 32);
    ssid[32] = '\0';

    char senha[64] = {0};
    memcpy(senha, &payload[32], 63);
    senha[63] = '\0';

    ESP_LOGI(TAG, "WiFi config received - SSID: %s", ssid);

    // Update WiFi credentials
    if (!wifi_update_credentials(ssid, senha)) {
      ESP_LOGW(TAG, "Failed to update WiFi credentials");
    } else {
      ESP_LOGI(TAG, "WiFi credentials updated successfully");

      // Send success response
      if (isCmd43_received()) {
        uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                              PKT_CMD_43, 0x00, 0x00, 0xBB, 0xBB};
        response[7] = crc8(&response[0], 7);
        uart_handler_write_bytes(response, sizeof(response));
        ESP_LOGI(TAG, "CMD 43 - WiFi config accepted, 0x00 response sent");
      }
    }

    // --- Parse static IP settings for Ethernet ---
    // Verify we have enough data for the IP fields
    uint8_t raw_payload_len =
        packet->data[2]; // LEN field = 159 for full packet
    if (raw_payload_len >= 158) {
      char ip_raw[17] = {0};
      memcpy(ip_raw, &payload[95], 16);

      char mask_raw[17] = {0};
      memcpy(mask_raw, &payload[111], 16);

      char gw_raw[17] = {0};
      memcpy(gw_raw, &payload[127], 16);

      char dns1_raw[8] = {0};
      memcpy(dns1_raw, &payload[143], 7);

      char dns2_raw[8] = {0};
      memcpy(dns2_raw, &payload[150], 7);

      ESP_LOGI(TAG,
               "CMD43 network config - IP:'%s' Mask:'%s' GW:'%s' DNS1:'%s' "
               "DNS2:'%s'",
               ip_raw, mask_raw, gw_raw, dns1_raw, dns2_raw);

      char ip_clean[16] = {0}, mask_clean[16] = {0}, gw_clean[16] = {0};
      char dns1_clean[16] = {0}, dns2_clean[16] = {0};
      sanitize_ipv4_string(ip_raw, ip_clean, sizeof(ip_clean));
      sanitize_ipv4_string(mask_raw, mask_clean, sizeof(mask_clean));
      sanitize_ipv4_string(gw_raw, gw_clean, sizeof(gw_clean));

      if (ip_raw[0] != '\0' && strcmp(ip_clean, "0.0.0.0") != 0) {
        esp_ip4_addr_t ip_addr = {0}, mask_addr = {0}, gw_addr = {0};
        esp_ip4_addr_t dns1_addr = {0}, dns2_addr = {0};

        bool ip_ok = (ESP_OK == esp_netif_str_to_ip4(ip_clean, &ip_addr));
        bool mask_ok = (ESP_OK == esp_netif_str_to_ip4(mask_clean, &mask_addr));
        bool gw_ok = (ESP_OK == esp_netif_str_to_ip4(gw_clean, &gw_addr));

        // DNS: optional, ignore if invalid (fallback DNS in sdkconfig covers
        // it)
        if (sanitize_ipv4_string(dns1_raw, dns1_clean, sizeof(dns1_clean)))
          esp_netif_str_to_ip4(dns1_clean, &dns1_addr);
        if (sanitize_ipv4_string(dns2_raw, dns2_clean, sizeof(dns2_clean)))
          esp_netif_str_to_ip4(dns2_clean, &dns2_addr);

        ESP_LOGI(TAG, "Static IP parsed: IP=%s Mask=%s GW=%s DNS1=%s DNS2=%s",
                 ip_clean, mask_clean, gw_clean,
                 dns1_clean[0] ? dns1_clean : "(none)",
                 dns2_clean[0] ? dns2_clean : "(none)");

        if (ip_ok && mask_ok && gw_ok) {
          ESP_LOGW(TAG, "Static configuration OK");
          ethernet_set_static_ip(ip_addr.addr, mask_addr.addr, gw_addr.addr,
                                 dns1_addr.addr, dns2_addr.addr);
        } else {
          ESP_LOGW(TAG,
                   "Invalid static IP fields (ip=%d mask=%d gw=%d), ignoring",
                   ip_ok, mask_ok, gw_ok);
        }
      } else {
        ESP_LOGI(TAG, "No static IP configured, using DHCP for Ethernet");
        ethernet_clear_static_ip();
      }
    } else {
      ESP_LOGI(TAG,
               "CMD43 payload too short for IP fields (len=%d), skipping "
               "network config",
               raw_payload_len);
    }

    cmd43_received = true;
    break;
  case PKT_CMD_44:
    // crc = packet->data[length];                    // CRC is after header,
    // length, type, cmd and data[18] crc_calc = crc8(&packet->data[4], length -
    // 4); // Calculate CRC starting from cmd byte to end of data if (crc !=
    // crc_calc)
    // {
    //     ESP_LOGI(TAG, "Packet CRC Checksum Error. CRC=%02X, Calculated=%02X",
    //     crc, crc_calc);
    //     // Send error response back through same source
    //     break;
    // }
    // uint8_t connection_type = packet->data[5]; // connection type is first
    // byte of data if (connection_type != CONNECTION_WIFI)
    // {
    //     // TODO: set a flag not to forward packets to and from MQTT
    //     mqtt_forwarding_enabled = false;
    //     ESP_LOGI(TAG, "MQTT forwarding disabled due to non-WiFi connection
    //     type in CMD_44");
    // }
    // else
    // {
    //     mqtt_forwarding_enabled = true;
    //     ESP_LOGI(TAG, "MQTT forwarding enabled due to WiFi connection type in
    //     CMD_44");
    // }
    // uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
    // PKT_CMD_44, 0x00, 0x00, 0xBB, 0xBB};

    // response[7] = crc8(&response[0], 7); // Calculate CRC for header, length,
    // packet_type and cmd

    // uart_handler_write_bytes(response, sizeof(response));
    break;
  case PKT_CMD_45:
    crc = packet->data[length]; // CRC is after header, length, type, cmd and
                                // data[18]
    crc_calc =
        crc8(&packet->data[4],
             length - 4); // Calculate CRC starting from cmd byte to end of data
    if (crc != crc_calc) {
      ESP_LOGI(TAG, "Packet CRC Checksum Error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      // Send error response back through same source
      break;
    }
    if (mqtt_manager_connected()) {
      uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                            PKT_CMD_45, 0x01, 0x00, 0xBB, 0xBB};

      response[7] =
          crc8(&response[0],
               7); // Calculate CRC for header, length, packet_type and cmd

      uart_handler_write_bytes(response, sizeof(response));
      ESP_LOGI(TAG, "CMD 45 - MQTT Connected status 0x01 sent to UART");
    } else {
      uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                            PKT_CMD_45, 0x00, 0x00, 0xBB, 0xBB};

      response[7] =
          crc8(&response[0],
               7); // Calculate CRC for header, length, packet_type and cmd

      uart_handler_write_bytes(response, sizeof(response));
      ESP_LOGI(TAG, "CMD 45 - MQTT Connected status 0x00 sent to UART");
    }
    break;
  case PKT_CMD_46: {
    ESP_LOGI(TAG, "CMD_46: Camera configuration received");

    // Check minimum packet size
    if (packet->length <= PKT_DATA_OFFSET) {
      ESP_LOGW(TAG, "Camera config packet too short");
      break;
    }

    // Calculate total length including header/cmd
    uint8_t cam_config_length = packet->data[2];
    cam_config_length += PKT_DATA_OFFSET;

    // Validate CRC
    crc = packet->data[cam_config_length];
    crc_calc = crc8(&packet->data[4], cam_config_length - 4);
    if (crc != crc_calc) {
      ESP_LOGI(TAG, "Packet CRC Checksum Error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    // Payload starts directly at data[5] (no status byte)
    uint8_t *cam_payload = &packet->data[5];
    uint8_t cam_payload_len = packet->data[2];

    if (cam_payload_len < 89) {
      ESP_LOGW(TAG, "CMD_46 payload too short (%d < 89)", cam_payload_len);
      break;
    }

    // Parse fields per protocol:
    // [0]:      índice (1 byte) — camera slot 0..7
    // [1-16]:   nome_camera (16 bytes)
    // [17-48]:  IP (32 bytes)
    // [49-52]:  ID_camera (4 bytes)
    // [53-68]:  user (16 bytes)
    // [69-84]:  senha (16 bytes)
    // [85-88]:  ID_setor (4 bytes)

    uint8_t cam_index = cam_payload[0];
    if (cam_index > 7) {
      ESP_LOGW(TAG, "CMD_46: invalid camera slot %d (max 7)", cam_index);
      break;
    }

    char nome_camera[17] = {0};
    memcpy(nome_camera, &cam_payload[1], 16);

    char cam_ip_raw[33] = {0};
    memcpy(cam_ip_raw, &cam_payload[17], 32);
    cam_ip_raw[32] = '\0'; // Ensure null-terminated even if field is full

    // Validate / sanitize the IP address
    char cam_ip[16] = {0};
    bool is_delete =
        (cam_ip_raw[0] == '\0' || strcmp(cam_ip_raw, "0.0.0.0") == 0);

    if (!is_delete) {
      if (!sanitize_ipv4_string(cam_ip_raw, cam_ip, sizeof(cam_ip))) {
        ESP_LOGW(TAG, "CMD_46: invalid IP address '%s', rejecting", cam_ip_raw);
        break;
      }
      // Final check: must parse as a real IPv4
      esp_ip4_addr_t test_addr;
      if (esp_netif_str_to_ip4(cam_ip, &test_addr) != ESP_OK) {
        ESP_LOGW(TAG, "CMD_46: IP '%s' failed to parse, rejecting", cam_ip);
        break;
      }
    }

    uint32_t id_camera_raw;
    memcpy(&id_camera_raw, &cam_payload[49], 4);
    uint32_t id_camera = ntohl(id_camera_raw);

    char cam_user[17] = {0};
    memcpy(cam_user, &cam_payload[53], 16);

    char cam_senha[17] = {0};
    memcpy(cam_senha, &cam_payload[69], 16);

    uint32_t id_setor_raw;
    memcpy(&id_setor_raw, &cam_payload[85], 4);
    uint32_t id_setor = ntohl(id_setor_raw);

    ESP_LOGI(TAG, "CMD_46: slot=%d name='%s' IP='%s'", cam_index, nome_camera,
             cam_ip);
    ESP_LOGI(TAG, "CMD_46: id_camera=0x%08lX user='%s' id_setor=0x%08lX",
             (unsigned long)id_camera, cam_user, (unsigned long)id_setor);

    if (is_delete) {
      ESP_LOGI(TAG, "CMD_46: Delete request for camera slot %d", cam_index);

      // Stop the ONVIF task for this slot
      onvif_events_stop_camera(cam_index);

      // Erase NVS keys for this slot
      nvs_handle_t cam_nvs;
      esp_err_t cam_err = nvs_open("camera", NVS_READWRITE, &cam_nvs);
      if (cam_err == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "name%d", cam_index);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "ip%d", cam_index);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "id_cam%d", cam_index);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "user%d", cam_index);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "pass%d", cam_index);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "setor%d", cam_index);
        nvs_erase_key(cam_nvs, key);
        nvs_commit(cam_nvs);
        nvs_close(cam_nvs);
        ESP_LOGI(TAG, "CMD_46: camera slot %d erased from NVS", cam_index);
      }

      // Send ACK
      uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                            PKT_CMD_46, 0x00, 0x00, 0xBB, 0xBB};
      response[7] = crc8(&response[0], 7);
      uart_handler_write_bytes(response, sizeof(response));
      ESP_LOGI(TAG, "CMD_46: Camera slot %d deleted, ACK sent", cam_index);
      cmd46_received = true;
      break;
    }

    // Store camera config to NVS keyed by slot index
    nvs_handle_t cam_nvs;
    esp_err_t cam_err = nvs_open("camera", NVS_READWRITE, &cam_nvs);
    if (cam_err == ESP_OK) {
      char key[16];

      snprintf(key, sizeof(key), "name%d", cam_index);
      nvs_set_str(cam_nvs, key, nome_camera);

      snprintf(key, sizeof(key), "ip%d", cam_index);
      nvs_set_str(cam_nvs, key, cam_ip);

      snprintf(key, sizeof(key), "id_cam%d", cam_index);
      nvs_set_u32(cam_nvs, key, id_camera);

      snprintf(key, sizeof(key), "user%d", cam_index);
      nvs_set_str(cam_nvs, key, cam_user);

      snprintf(key, sizeof(key), "pass%d", cam_index);
      nvs_set_str(cam_nvs, key, cam_senha);

      snprintf(key, sizeof(key), "setor%d", cam_index);
      nvs_set_u32(cam_nvs, key, id_setor);

      nvs_commit(cam_nvs);
      nvs_close(cam_nvs);
      ESP_LOGI(TAG, "CMD_46: camera slot %d saved to NVS", cam_index);
    } else {
      ESP_LOGE(TAG, "CMD_46: NVS open failed: %s", esp_err_to_name(cam_err));
    }

    // Start or restart the ONVIF polling task for this camera
    onvif_events_start_camera(cam_index, cam_ip, cam_user, cam_senha,
                              id_camera);

    // Send ACK to Host
    uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                          PKT_CMD_46, 0x00, 0x00, 0xBB, 0xBB};
    response[7] = crc8(&response[0], 7);
    uart_handler_write_bytes(response, sizeof(response));
    ESP_LOGI(TAG, "CMD_46: Camera config accepted, ACK sent");
    cmd46_received = true;

    break;
  }
  case PKT_CMD_48: {
    ESP_LOGI(TAG, "CMD_48: Camera online bitmask");

    // Validate minimum length: header(2) + len(1) + type(1) + cmd(1) +
    // direction(1) + crc(1) + tail(2)
    uint8_t raw_len = packet->data[2];
    uint8_t total_len = raw_len + PKT_DATA_OFFSET;

    // Validate CRC
    crc = packet->data[total_len];
    crc_calc = crc8(&packet->data[4], total_len - 4);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_48 CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    uint8_t direction = packet->data[5];

    if (direction == 0x00) {
      // Central is requesting current camera status → reply with bitmask
      uint8_t bitmask = onvif_events_get_online_bitmask();
      uint8_t reply[] = {0xAA,       0xAA, 0x00,    0x02, PKT_TYPE_CONFIG,
                         PKT_CMD_48, 0x00, bitmask, 0x00, 0xBB,
                         0xBB};
      reply[8] = crc8(&reply[4], 3); // CRC over CMD + direction + bitmask
      uart_handler_write_bytes(reply, sizeof(reply));
      ESP_LOGI(TAG, "CMD_48: Status requested, replied bitmask=0x%02X",
               bitmask);
    } else {
      // direction == 0x01: Central is ACKing our voluntary notification
      ESP_LOGI(TAG, "CMD_48: ACK received from central");
    }
    break;
  }
  case PKT_CMD_A0: {
    ESP_LOGI(TAG, "CMD_A0: Delete camera by slot received");

    // Minimum size check
    if (packet->length <= PKT_DATA_OFFSET) {
      ESP_LOGW(TAG, "CMD_A0 packet too short");
      break;
    }

    // Length + CRC validation (same scheme as CMD_46/CMD_48)
    uint8_t a0_length = packet->data[2];
    a0_length += PKT_DATA_OFFSET;

    crc = packet->data[a0_length];
    crc_calc = crc8(&packet->data[4], a0_length - 4);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_A0 CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    uint8_t a0_payload_len = packet->data[2];
    if (a0_payload_len < 1) {
      ESP_LOGW(TAG, "CMD_A0 payload too short (need 1 byte id)");
      break;
    }

    // The single payload byte is the camera ID (low byte of the stored 4-byte
    // id_cam). The upper 3 bytes of the stored ID are central-side dedup data
    // that is not meaningful to us, so we match on the low byte only.
    uint8_t target_id = packet->data[5];

    ESP_LOGI(TAG, "CMD_A0: delete request for camera id %u (0x%02X)", target_id,
             target_id);

    // Loop over all slots and delete every one whose stored id_cam low byte
    // matches. We never stop at the first match: duplicate-ID slots have been
    // observed (suspected central/app bug), so all matches must be cleared.
    nvs_handle_t cam_nvs;
    esp_err_t cam_err = nvs_open("camera", NVS_READWRITE, &cam_nvs);
    int deleted_count = 0;
    if (cam_err == ESP_OK) {
      char key[16];
      for (uint8_t slot = 0; slot < 8; slot++) {
        uint32_t stored_id = 0;
        snprintf(key, sizeof(key), "id_cam%d", slot);
        esp_err_t get_err = nvs_get_u32(cam_nvs, key, &stored_id);
        if (get_err != ESP_OK) {
          // Empty/unconfigured slot (key missing) or read error → skip.
          continue;
        }

        if ((stored_id & 0xFF) != target_id) {
          continue;
        }

        ESP_LOGI(TAG,
                 "CMD_A0: id %u matched slot %d (stored id_cam=0x%08lX), "
                 "deleting",
                 target_id, slot, (unsigned long)stored_id);

        // Stop the ONVIF task for this slot
        onvif_events_stop_camera(slot);

        // Erase NVS keys for this slot (same set CMD_46 delete erases)
        snprintf(key, sizeof(key), "name%d", slot);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "ip%d", slot);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "id_cam%d", slot);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "user%d", slot);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "pass%d", slot);
        nvs_erase_key(cam_nvs, key);
        snprintf(key, sizeof(key), "setor%d", slot);
        nvs_erase_key(cam_nvs, key);
        deleted_count++;
      }
      nvs_commit(cam_nvs);
      nvs_close(cam_nvs);
      ESP_LOGI(TAG, "CMD_A0: id %u → %d slot(s) erased from NVS", target_id,
               deleted_count);
    } else {
      ESP_LOGE(TAG, "CMD_A0: NVS open failed: %s", esp_err_to_name(cam_err));
    }

    // Send ACK (mirrors CMD_46 delete ACK format). The delete is idempotent:
    // we ACK even when deleted_count == 0 (no slot had this id).
    uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                          PKT_CMD_A0, 0x00, 0x00, 0xBB, 0xBB};
    response[7] = crc8(&response[0], 7);
    uart_handler_write_bytes(response, sizeof(response));
    ESP_LOGI(TAG, "CMD_A0: id %u delete processed (%d slot(s)), ACK sent",
             target_id, deleted_count);

    break;
  }
  default:
    break;
  }

  return true;
}

bool handle_contactid_packet(packet_t *packet) {
  if (packet->length < PKT_DATA_OFFSET) {
    ESP_LOGW(TAG, "Contact ID packet too short");
    return false;
  }

  uint8_t cmd = packet->data[4];

  ESP_LOGI(TAG, "Contact ID packet type 0x%02X from source %d cmd=0x%02X",
           packet->packet_type, packet->source, cmd);

  switch (cmd) {
  case PKT_CMD_01H:
  case PKT_CMD_02H:
  case PKT_CMD_03H: {
    ESP_LOGI(TAG, "CMD_0x%02X: Send CID event", cmd);

    uint8_t raw_len = packet->data[2];

    // Validate CRC
    uint8_t crc = packet->data[5 + raw_len];
    uint8_t crc_calc = crc8(&packet->data[4], raw_len + 1);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_0x%02X CRC error. CRC=%02X, Calculated=%02X", cmd, crc,
               crc_calc);
      break;
    }

    // dest is the CMD itself (0x01=A, 0x02=B, 0x03=both)
    uint8_t dest = cmd;

    // Heap-copy payload for async task (pkt.data will be freed by pkt_proc)
    uint8_t *payload_copy = pvPortMalloc(raw_len);
    if (!payload_copy) {
      ESP_LOGE(TAG, "CMD_0x%02X: malloc failed", cmd);
      break;
    }
    memcpy(payload_copy, &packet->data[5], raw_len);

    monip_cmd_t monip_cmd = {
        .type = MONIP_CMD_SEND_EVENT,
        .dest = dest,
        .payload = payload_copy,
        .payload_len = raw_len,
        .origin_cmd = cmd,
    };
    if (!monip_submit_cmd(&monip_cmd)) {
      ESP_LOGW(TAG, "CMD_0x%02X: MONIP queue full", cmd);
    }
    break;
  }
  case PKT_CMD_10H: {
    ESP_LOGI(TAG, "CMD_10H: MONIP configuration received");

    // Frame: AA AA [LEN] [ID] [CMD] [PAYLOAD...] [CRC] BB BB
    // LEN = data[2] = payload size (bytes after CMD, before CRC)
    // CRC covers CMD + PAYLOAD = (LEN + 1) bytes starting at data[4]
    uint8_t raw_len = packet->data[2];

    // Validate CRC
    uint8_t crc = packet->data[5 + raw_len];                // CRC position
    uint8_t crc_calc = crc8(&packet->data[4], raw_len + 1); // CMD + PAYLOAD
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_10H CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    // Payload starts at data[5], size = raw_len
    uint8_t *p = &packet->data[5];

    if (raw_len < 81) {
      ESP_LOGW(TAG, "CMD_10H payload too short (%d < 81)", raw_len);
      break;
    }

    // Decode fields
    char ip_a[33] = {0};
    memcpy(ip_a, &p[0], 32);
    ip_a[32] = '\0';

    uint16_t port_a = ((uint16_t)p[32] << 8) | p[33];

    char ip_b[33] = {0};
    memcpy(ip_b, &p[34], 32);
    ip_b[32] = '\0';

    uint16_t port_b = ((uint16_t)p[66] << 8) | p[67];

    char acct[5] = {0};
    memcpy(acct, &p[68], 4);
    acct[4] = '\0';

    char imei[7] = {0};
    memcpy(imei, &p[72], 6);
    imei[6] = '\0';

    uint16_t keep_alive = ((uint16_t)p[78] << 8) | p[79];

    char conn_type = (char)p[80];

    ESP_LOGI(TAG, "CMD_10H decoded:");
    ESP_LOGI(TAG, "  IP A: %s:%u", ip_a, port_a);
    ESP_LOGI(TAG, "  IP B: %s:%u", ip_b, port_b);
    ESP_LOGI(TAG, "  ACCT: %s, IMEI: %s", acct, imei);
    ESP_LOGI(TAG, "  Keep Alive: %us, Conn Type: %c", keep_alive, conn_type);

    // Save to NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open("dc09", NVS_READWRITE, &h);
    if (err == ESP_OK) {
      nvs_set_str(h, "ip_a", ip_a);
      nvs_set_u16(h, "port_a", port_a);
      nvs_set_str(h, "ip_b", ip_b);
      nvs_set_u16(h, "port_b", port_b);
      nvs_set_str(h, "acct", acct);
      nvs_set_str(h, "imei", imei);
      nvs_set_u16(h, "keep_alive", keep_alive);
      nvs_set_u8(h, "conn_type", (uint8_t)conn_type);
      nvs_commit(h);
      nvs_close(h);
      ESP_LOGI(TAG, "CMD_10H: config saved to NVS");
    } else {
      ESP_LOGE(TAG, "CMD_10H: NVS open failed: %s", esp_err_to_name(err));
    }

    cmd10_received = true;
    monip_init();
    break;
  }
  case PKT_CMD_30H: {
    ESP_LOGI(TAG, "CMD_30H: Connect to receiver");

    uint8_t raw_len = packet->data[2];

    // Validate CRC
    uint8_t crc = packet->data[5 + raw_len];
    uint8_t crc_calc = crc8(&packet->data[4], raw_len + 1);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_30H CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    uint8_t dest = packet->data[5];
    ESP_LOGI(TAG, "CMD_30H: dest=0x%02X", dest);

    monip_cmd_t monip_cmd = {
        .type = MONIP_CMD_CONNECT,
        .dest = dest,
        .payload = NULL,
        .payload_len = 0,
        .origin_cmd = 0x30,
    };
    monip_submit_cmd(&monip_cmd);
    break;
  }
  case PKT_CMD_32H: {
    ESP_LOGI(TAG, "CMD_32H: Keep alive");

    uint8_t raw_len = packet->data[2];

    // Validate CRC
    uint8_t crc = packet->data[5 + raw_len];
    uint8_t crc_calc = crc8(&packet->data[4], raw_len + 1);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_32H CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    uint8_t dest = packet->data[5];
    ESP_LOGI(TAG, "CMD_32H: dest=0x%02X", dest);

    monip_cmd_t monip_cmd = {
        .type = MONIP_CMD_KEEPALIVE,
        .dest = dest,
        .payload = NULL,
        .payload_len = 0,
        .origin_cmd = 0x32,
    };
    monip_submit_cmd(&monip_cmd);
    break;
  }
  case PKT_CMD_35H: {
    ESP_LOGI(TAG, "CMD_35H: Forward response to receptor");

    uint8_t raw_len = packet->data[2];

    // Validate CRC
    uint8_t crc = packet->data[5 + raw_len];
    uint8_t crc_calc = crc8(&packet->data[4], raw_len + 1);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_35H CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    // Heap-copy payload for async task
    uint8_t *payload_copy = pvPortMalloc(raw_len);
    if (!payload_copy) {
      ESP_LOGE(TAG, "CMD_35H: malloc failed");
      break;
    }
    memcpy(payload_copy, &packet->data[5], raw_len);

    monip_cmd_t monip_cmd = {
        .type = MONIP_CMD_FORWARD,
        .dest = MONIP_DEST_ALL,
        .payload = payload_copy,
        .payload_len = raw_len,
        .origin_cmd = 0x35,
    };
    if (!monip_submit_cmd(&monip_cmd)) {
      ESP_LOGW(TAG, "CMD_35H: MONIP queue full");
    }
    break;
  }
  case PKT_CMD_38H: {
    ESP_LOGI(TAG, "CMD_38H: Disconnect");

    uint8_t raw_len = packet->data[2];

    // Validate CRC
    uint8_t crc = packet->data[5 + raw_len];
    uint8_t crc_calc = crc8(&packet->data[4], raw_len + 1);
    if (crc != crc_calc) {
      ESP_LOGW(TAG, "CMD_38H CRC error. CRC=%02X, Calculated=%02X", crc,
               crc_calc);
      break;
    }

    uint8_t dest = packet->data[5];
    ESP_LOGI(TAG, "CMD_38H: dest=0x%02X", dest);

    monip_cmd_t monip_cmd = {
        .type = MONIP_CMD_DISCONNECT,
        .dest = dest,
        .payload = NULL,
        .payload_len = 0,
        .origin_cmd = 0x38,
    };
    monip_submit_cmd(&monip_cmd);
    break;
  }
  default:
    ESP_LOGW(TAG, "Unknown Contact ID cmd: 0x%02X", cmd);
    break;
  }

  return true;
}

void packet_actions_init(void) {
  packet_manager_register_handler(PKT_TYPE_BLE, handle_ble_packet);
  packet_manager_register_handler(PKT_TYPE_CLOUD, handle_default_packet);
  packet_manager_register_handler(PKT_TYPE_CONFIG, handle_config_packet);
  packet_manager_register_handler(PKT_TYPE_CONTACTID, handle_contactid_packet);

  ESP_LOGI(TAG, "Packet actions initialized");
}