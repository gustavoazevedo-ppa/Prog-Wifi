# Project structure

```
CMakeLists.txt          Project root (project name prog_wifi, PROJECT_VER)
partitions.csv          Flash / OTA partition table
sdkconfig*              Config (edit the .defaults files, not sdkconfig)
dependencies.lock       Component Manager lock file
main/                   Application component
components/enc28j60/    Vendored, patched Ethernet driver (see enc28j60-vendored.md)
managed_components/     Downloaded by Component Manager; do not edit
patches/                Numbered diffs of enc28j60 fixes + PR_DRAFTS.md
scripts/                build.ps1, idf_env.ps1 (always build through these)
docs/                   API specs, selector / connect protocols, bridge design, open questions, capture analysis script, hardware PDFs
cert/                   AWS certificates and keys (secrets)
build/                  Build output (generated)
.kiro/steering/         Steering files
.kiro/hooks/            Agent hooks (update-foundation-steering keeps product/tech/structure.md current)
```

## main/ modules

| Module | Role |
|---|---|
| `main.c` | `app_main`: boot sequence, status LED task, boot health timer |
| `wifi_manager` | Wi-Fi station init, credentials stored in NVS, can be disabled with `WIFI_ENABLED` |
| `ethernet_manager` | ENC28J60 init, link state, optional static IP, can be disabled with `ETHERNET_ENABLED` |
| `nvs_schema` | NVS schema version check, boot-loop protection, boot/panic counters |
| `ota_button` | Physical button recovery / OTA, manifest check, decrypt of pre-encrypted images, NVS format |
| `preset_server` | HTTP server: `POST /iniciar`, `GET /status`, preset download, run history |
| `admin_api` | `/admin/*` endpoints, registered on the same httpd server |
| `http_log` | Request logging wrappers around `esp_http_server` |
| `log_server` | Read-only TCP log console (port 2323), opened for a limited window on request |
| `selector_wire`, `selector_keys` | Selector protocol framer / codec and key edge helper (pure C, no ESP-IDF dependency) |
| `selector_port` | One UART per instance: UART2 plays the selector toward the central (GPIO17 TX, GPIO16 RX), UART1 plays the host toward the real selector (GPIO25 TX, GPIO26 RX) |
| `selector_bridge` | Queues, bridge task and translator contract; drop counters and frame tap |
| `selector_tr_menu` | Translator: Prog menu (project / version, Wi-Fi / Ethernet IP and MAC pages) while the central is silent, central screens pass through otherwise |
| `device_info.h` | Device identification constants |
| `index.html` | Static device-info page, embedded in flash |
| `ota_decrypt_priv.pem` | Embedded OTA decryption key (secret) |

## Adding a module

1. Create `main/<name>.c` and `main/<name>.h`.
2. Add the `.c` to `SRCS` in `main/CMakeLists.txt`, plus any needed components.
3. Wire its init into `app_main()` in the right place of the boot sequence.
4. Build with `.\scripts\build.ps1` and confirm `Project build complete.`

## Related steering

- `build.md`: how to build and verify.
- `enc28j60-vendored.md`: rules for the vendored driver.
- `product.md`, `tech.md`: purpose, stack and conventions.
