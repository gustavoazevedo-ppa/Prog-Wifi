#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// Persistent diagnostics counters (NVS keys "boot_cnt" / "panic_tot" in the same
// namespace; adding keys does not change NVS_SCHEMA_VERSION). Both are cleared
// by anything that erases NVS (format, schema change, boot loop recovery).
//   boot_count  : every boot, including this one.
//   panic_total : boots that followed a panic or watchdog reset (cumulative,
//                 unlike the consecutive-panic counter used for boot loop recovery).
// Call once per boot, after nvs_schema_check() and nvs_boot_loop_check().
void nvs_boot_counters_update(void);

// Returns the values cached by nvs_boot_counters_update(). Returns false (outputs
// untouched) if NVS was unusable. Either output pointer may be NULL.
bool nvs_boot_counters_get(uint32_t *boot_count, uint32_t *panic_total);

// Mark the current boot as healthy. Call this once the application has been
// running successfully for a reasonable period (e.g., 30s timer or after
// first successful network activity). Resets the panic counter to 0.
void nvs_boot_mark_healthy(void);
