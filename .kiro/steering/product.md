# Product overview

Firmware for the **Modulo WiFi Infinity S** ("Prog"), an ESP32 network module. Project name in CMake: `prog_wifi`; build artifact: `prog_wifi.bin`.

## What it does

- Receives commands over HTTP on the local network (Wi-Fi station and Ethernet via ENC28J60, independently).
- Is driven by the **Stinger**, a custom ERP on TOTVS Protheus (ADVPL). The Stinger sends `POST /iniciar` with product info and a base path. The Prog fetches a preset config file (with a fallback order), then the Stinger polls `GET /status` until the run ends.
- Will run tests and diagnostics on a device under test through a proprietary UART protocol. The UART protocol is **not implemented yet**; actions are only logged for now.
- May drive a "selector" (XMC1100 board with 16x2 LCD, 4 keys, buzzer) over UART as host, to show IP/MAC addresses and read keys. Not yet integrated.
- Exposes admin endpoints (`/admin/*`), a static device-info page (`index.html`) and a read-only remote log console (TCP port 2323, opened on request for a limited window).
- Updates itself via OTA from a button or remotely. Firmware images are pre-encrypted (RSA-3072 wrapped), served from a public S3 bucket.

## Reliability expectations

- Units are deployed in the field. A bad update or corrupted NVS must self-recover (OTA rollback, NVS schema check, boot-loop protection).
- A unit booted without a cable must not lose a good update on its next reset, so firmware validation is gated on the HTTP server running, not on getting an IP.

## Reference docs (source of truth for protocols)

- `docs/preset-api-spec.md`: preset / test start API (endpoints, payloads, auth, forgiving input rules).
- `docs/stinger-api-iniciar-status.md`: `/iniciar` and `/status` details.
- `docs/selector-protocol.md`: selector UART protocol (parts marked **[inferred]** or **[unverified]** need hardware confirmation).
- `docs/MA00476 - MODULO WIFI INFINITY S V1.3.pdf`, `docs/Novo Prog.pdf`: hardware manual and requirements.

## Language

Docs and some source comments mix English and Portuguese. Endpoint and field names from the Stinger (`iniciar`, `produto`, `cliente`, `estado`, `caminho`) stay in Portuguese. Write new code and comments in English.
