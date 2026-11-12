# Tech stack and conventions

## Stack

- **ESP-IDF v5.4.4**, target ESP32, C (no C++). Build rules are in `build.md`; follow them.
- FreeRTOS tasks and timers, `esp_http_server`, `esp_http_client`, `esp_https_ota`, `esp_encrypted_img` (managed component, `^2.7.1`), `esp-tls`/mbedtls, `cJSON` (`json` component), NVS.
- Ethernet: vendored, patched ENC28J60 driver over SPI. See `enc28j60-vendored.md`; never replace it with the managed copy.
- Managed components live in `managed_components/` and are resolved by the IDF Component Manager (`dependencies.lock`). Do not hand-edit them.

## Config files

- `sdkconfig.defaults` (plus per-chip variants) holds intended config. `sdkconfig` is generated; change options through defaults, not by hand.
- `partitions.csv` defines the OTA layout. Changing it affects fielded units, so flag it before editing.
- `main/idf_component.yml` declares managed dependencies only. New `main` sources must be added to `SRCS` in `main/CMakeLists.txt`, and new IDF components to `REQUIRES` / `PRIV_REQUIRES`.

## Secrets

- `cert/` (AWS certs and keys) and `main/ota_decrypt_priv.pem` are secrets. Do not print, copy or commit their contents. The checked-in `ota_decrypt_priv.pem` is a dev-only key; release swaps in the real one.
- The API key and console credentials are inline constants in the code for now (`preset_server.c`). Do not log them.

## Code conventions

- Each module is a `.c`/`.h` pair in `main/` with a short purpose comment, a `#define TAG` for logging and `ESP_LOGx` for output (no `printf`).
- Include guards in the form `NAME_H`. Public functions are prefixed by module (`wifi_`, `ethernet_`, `nvs_`, `ota_`, `preset_server_`, `log_server_`, `admin_api_`, `http_log_`).
- Brace style: Allman (opening brace on its own line), 4-space indent.
- HTTP handlers register through `http_log_register()` and use `http_log_set_status()` / `http_log_send_err()` instead of the raw `httpd_*` equivalents, so requests are logged.
- Keep handler stacks small: long work goes to a separate task using the `http_log_async_*` helpers.
- JSON responses: `Content-Type: application/json`, `Content-Length` set (no chunked), ASCII only, stable shape and types. Be lenient on input, strict on output.
- Both Wi-Fi and Ethernet must work for every network feature; do not assume one interface.
- Admin actions are refused while a test run is active (`preset_server_run_active()`, `admin_api_blocks_runs()`).
- NVS layout is versioned (`NVS_SCHEMA_VERSION` in `nvs_schema.h`). Changing stored keys or layout needs a version bump, which erases NVS on upgrade. Flag that to the user.

## Boot sequence (main.c)

Log hook, NVS init, schema check, boot-loop check, boot counters, netif and event loop, Ethernet (no-op when `ETHERNET_ENABLED` is 0), LEDs, Wi-Fi, OTA button, preset server, log server, selector bridge, then mark the app valid only if the HTTP server is running. A 30 s timer marks the boot healthy. Keep this order unless there is a reason to change it.

## Hardware notes

- Status LEDs are active-low: `WIFI_LED_PIN` GPIO33, `ETH_LED_PIN` GPIO32.
- The OTA process owns the ETH LED while it runs (`ota_button_leds_busy()`).
- Selector bridge UARTs, 9600 8N1, 3.3 V (see `docs/selector-protocol.md` and `docs/selector-bridge-design.md`): UART1 faces the real selector (TX GPIO25, RX GPIO26), UART2 faces the central / DUT (TX GPIO17, RX GPIO16). The module is an ESP32-WROOM-32D (no PSRAM, GPIO16/17 free).
