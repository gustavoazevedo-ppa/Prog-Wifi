#pragma once

#include <stdbool.h>

// Current NVS schema version. Increment this whenever the NVS layout changes
// (keys renamed, removed, format changed, etc.). On mismatch, NVS is erased
// and reinitialized with safe defaults.
#define NVS_SCHEMA_VERSION 1

// Maximum consecutive panic reboots before NVS is erased automatically.
// Protects against boot loops caused by corrupted NVS data.
#define NVS_MAX_PANIC_BOOTS 3

// Check NVS schema version. If it doesn't match NVS_SCHEMA_VERSION,
// erases NVS and writes the current version. Call after nvs_flash_init().
void nvs_schema_check(void);

// Check for boot loop condition. If the device has panic-rebooted
// NVS_MAX_PANIC_BOOTS times consecutively, erases NVS to break the loop.
// Call after nvs_flash_init() and nvs_schema_check().
// Returns true if NVS was erased due to boot loop detection.
bool nvs_boot_loop_check(void);

// Mark the current boot as healthy. Call this once the application has been
// running successfully for a reasonable period (e.g., 30s timer or after
// first successful network activity). Resets the panic counter to 0.
void nvs_boot_mark_healthy(void);
