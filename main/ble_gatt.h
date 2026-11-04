#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

void ble_gatt_init(void);
void ble_send_response(const void *data, size_t len);
void ble_rename_device(const char *new_name);
void ble_device_name_load(void);
void ble_start_advertising(void);
bool ble_has_valid_name(void);