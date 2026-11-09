# Preset / Test Start API Spec

Source: message from the Stinger side (originally in Portuguese), summarizing what was agreed earlier, plus decisions made afterwards.

## Context and scope

The Prog is a device that receives commands over HTTP on the local network and performs tests and diagnostics through a proprietary UART protocol.

- The UART protocol is not implemented yet. It will come later.
- For now, the "actions" (start tests, apply the preset, etc.) are only logged to the console, using a static constant defined inline in the code.
- The goal of this phase is to validate the HTTP integration with the Stinger: endpoints, payloads, preset lookup and status polling.

## Overview

The Stinger (client) tells the Prog to start a test run for a product. It sends a `POST` with the product info and a base path where preset files live. The Prog fetches a fixed-name config file from that location, using a fallback order, and starts the tests. The Stinger then polls a status endpoint every X seconds until the run finishes.

```
Stinger                                   Prog
   |  POST /iniciar {id, produto, ...}      |
   |--------------------------------------->|  resolve + download exemplo.conf
   |                                        |  (future: send to DUT via UART)
   |  200 OK (status object)                |
   |<---------------------------------------|
   |                                        |
   |  GET /status   (every X seconds)       |
   |--------------------------------------->|
   |  200 {id, status, message[]}           |
   |<---------------------------------------|
   |  ... until status = 2 or 3             |
```

Base URL: `http://{IP_DO_PROG}`

## General rules

| Topic          | Decision                                                                                                   |
|----------------|------------------------------------------------------------------------------------------------------------|
| Network        | Endpoints must work on both Wi-Fi and Ethernet (ENC28J60).                                                 |
| Authentication | Header `X-API-Key` with a fixed key defined inline as a constant in the code. Requests without a valid key are rejected. |
| Busy           | If a `POST /iniciar` arrives while a run is in progress, reject it and log the event.                       |
| `message`      | Always an array of strings. A single error is a one-item array. Empty array when there is no error.         |
| Leniency       | Be as forgiving as possible on input (see "Forgiving input rules"). Strict on output: stable shape and types. |
| Response format| `Content-Type: application/json`, `Content-Length` set, no chunked encoding, ASCII-only strings, no BOM.    |
| Client         | The Stinger is a custom ERP on TOTVS Protheus (ADVPL). Details of its HTTP client are unknown and will be refined later. |

## Forgiving input rules

- Header names are case-insensitive. `X-API-Key` value is compared exactly.
- `id` accepted as number or numeric string. It is echoed back as received type if possible, otherwise as a number.
- Unknown JSON fields are ignored.
- `Content-Type` is not required to be exactly `application/json`. Parse the body as JSON regardless.
- `produto` is informational only, so it is optional.
- `cliente` and `estado` are optional. A missing or empty one skips that lookup step.
- `caminho` is normalized: a trailing `/` is added if missing. It is required.
- Surrounding whitespace in string fields is trimmed.
- Hex content: whitespace and line breaks are stripped, upper and lower case both accepted. A `0x` prefix is not expected.
- Only an HTTP 404 on a preset URL moves on to the next location. Any other failure (timeout, 5xx, TLS) is also tried on the next location, and the last error is reported if all fail.

## Endpoint 1: `POST /iniciar`

Sets the preset and starts the tests.

### Request body (JSON)

```json
{
  "id": 1234,
  "produto": "F02303100",
  "cliente": "008249",
  "estado": "SP",
  "caminho": "https://preset.ppa.com.br/agility123/"
}
```

| Field     | Type   | Description                                                                 |
|-----------|--------|-----------------------------------------------------------------------------|
| `id`      | number | Run identifier chosen by the Stinger. Echoed back in `/status`.             |
| `produto` | string | Product code.                                                               |
| `cliente` | string | Customer code. Used for the first (most specific) preset lookup.            |
| `estado`  | string | State (UF). Used for the second lookup.                                     |
| `caminho` | string | Base path of the preset repository. HTTPS, and HTTP in some cases. SMB/FTP are out of scope for now. |

Note: the original example is not valid JSON (missing comma after `id`, and a `//` comment). The real payload must be valid JSON.

### Preset file resolution

The config file always has a fixed name: `exemplo.conf`. The Prog tries these locations in order and uses the first one that exists:

| Order | URL                                  | Scope    |
|-------|--------------------------------------|----------|
| 1     | `{caminho}/{cliente}/exemplo.conf`   | Customer |
| 2     | `{caminho}/{estado}/exemplo.conf`    | State    |
| 3     | `{caminho}/exemplo.conf`             | Default  |

Example with the values above:

1. `https://preset.ppa.com.br/agility123/008249/exemplo.conf`
2. `https://preset.ppa.com.br/agility123/SP/exemplo.conf`
3. `https://preset.ppa.com.br/agility123/exemplo.conf`

If none exist, the start fails (see error handling, still to be defined).

### Preset file format

- `exemplo.conf` is a hex-encoded text representation of a binary file.
- The binary holds preset and calibration data for the equipment under test (DUT).
- Size: a few dozen bytes up to 2 KB (binary) in the worst case. That is up to about 4 KB of hex text.
- Current phase: the Prog downloads the file and logs its content to the console.

### TLS

- HTTPS is enough. HTTP may be used in some cases.
- Certificate validation is ignored for now (first phase uses a self-signed CA). This must be revisited before production.

### Timing

- The `POST` replies only after the preset has been handled, not right away.
- In the final version "handled" includes transmitting the binary to the DUT over a 9600 bps UART. For 2 KB that is roughly 2 to 3 seconds of transmission time (10 bits per byte at 9600 bps is about 2.1 s), plus download time.
- The Stinger HTTP client timeout must account for this.

### Response

- `200 OK`: the preset was found and the tests started. The Stinger relies on this code to know the start succeeded.
- Body: may be the same object returned by `GET /status`.
- Failure responses (kept to few, distinct codes; the reason always goes in `message`):

| Case | HTTP code | Body |
|---|---|---|
| Missing or wrong `X-API-Key` | 401 | Empty |
| Invalid JSON or missing `id` / `caminho` | 400 | Status object, `status: 3` |
| Run already in progress | 409 | Status object of the current run (state untouched) |
| Firmware update download, NVS format or reboot in progress (see "Admin endpoints") | 409 | Status object with the request `id`, `status: 3` and `message: ["firmware update in progress, try again later"]` (stored state untouched) |
| No `exemplo.conf` found in any location | 404 | Status object, `status: 3` |
| Invalid hex content, or any other failure | 500 | Status object, `status: 3` |

- The error body is the same object as `GET /status`, so the Stinger can reuse one parser.
- A failed start leaves `/status` reporting `status: 3` with the same `id` and message.
- A rejected request (401, 409) does not change the current run's state. Every rejection is logged.

## Endpoint 2: `GET /status`

Returns the state of the current run. Polled by the Stinger every X seconds. Requires the `X-API-Key` header.

### Response body (JSON)

```json
{
  "id": 1234,
  "status": 1,
  "message": []
}
```

| Field     | Type     | Description                                                          |
|-----------|----------|----------------------------------------------------------------------|
| `id`      | number   | The `id` sent in the `POST /iniciar`.                                |
| `status`  | number   | `1` = Running, `2` = Finished OK, `3` = Finished with ERROR.         |
| `message` | string[] | Error descriptions. Always an array, empty when there is no error.   |

### Status values

| Value | Meaning             | Polling                      |
|-------|---------------------|------------------------------|
| 1     | Running             | Keep polling                 |
| 2     | Finished OK         | Stop polling                 |
| 3     | Finished with ERROR | Stop polling, read `message` |

## Typical flow

1. Stinger sends `POST /iniciar` with the run info and `X-API-Key`.
2. Prog resolves and downloads `exemplo.conf`, logs its content (later: sends it over UART), starts the tests, replies `200`.
3. Stinger polls `GET /status` every X seconds.
4. When `status` is `2` or `3`, the Stinger stops polling.

## Assumed defaults (to revisit)

Details are still missing and the spec will be updated as the project goes. Until then, these defaults apply:

1. **Simulated run:** after a successful `POST`, the run reports `status: 1` for a few seconds (constant defined inline), then `status: 2`.
2. **Test action:** the future UART action is a log line built from an inline static constant.
3. **UART delay:** emulate the transmission time (about `bytes * 10 / 9600` s) so the Stinger timeout is tested realistically.
4. **`GET /status` with no run yet (or after reboot):** `200` with `{"id": 0, "status": 0, "message": []}` (idle). Value `0` is an addition to the agreed 1/2/3 and must be confirmed with the Stinger side.
5. **Final status retention:** kept until the next `POST /iniciar` is accepted or the device reboots.
6. **Path safety:** `cliente` and `estado` are rejected (400) if they contain `/`, `\` or `..`.
7. **Fallthrough rule:** see "Forgiving input rules".

## Still unknown (Stinger side)

- Which HTTP call the Stinger uses (`FWRest`, `HttpPost`, ...) and whether it sends `Content-Type: application/json`.
- Whether it can read the body of non-200 responses.
- Polling interval (X seconds) and request timeout. The timeout must exceed download plus UART time.
- Whether `id` is always a number or sometimes a padded string.
- `exemplo.conf` exact format details and CA used by `preset.ppa.com.br`.

## Implementation notes (firmware)

Code: `main/preset_server.c` / `preset_server.h`, started from `app_main()`.

- Listens on port 80 on all interfaces (Wi-Fi and Ethernet).
- Inline constants at the top of `preset_server.c`: `PRESET_API_KEY` (default `change-me-prog-key`, change it), `PRESET_SIM_RUN_MS` (simulated run, 5000 ms), `PRESET_SIM_ACTION_TEXT`, `PRESET_FETCH_TIMEOUT_MS` (5000 ms per preset URL), `PRESET_MAX_HEX_CHARS`.
- A `400` reply does not change the stored run state (the start was never attempted). `404` and `500` after the request was accepted do set `status: 3`.
- `POST /iniciar` is validated and accepted in the HTTP task, then handed to a worker task (`preset_worker`) using the async handler API of `esp_http_server`. The worker downloads the preset, runs the (simulated) UART step and sends the reply. The HTTP task stays free, so `GET /status` answers right away (status 1 while the preset is being handled) and a second `POST` gets `409` immediately.
- The busy check and the acceptance are one atomic step, so two simultaneous `POST`s cannot both start a run.
- Worst case `POST` time: 3 preset URLs x 5 s timeout, plus about 2.1 s of emulated UART time for 2 KB. The client timeout should be above 30 s.
- Outbound HTTPS to the preset server does not verify the certificate (`CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` in `sdkconfig.defaults`). The OTA code still verifies with the certificate bundle. Revisit before production.
- `GET /` and `GET /index.html` serve a **static** HTML page (`main/index.html`, embedded in flash with `EMBED_TXTFILES`, served by `serve_asset()` in `preset_server.c`). Its inline JavaScript fetches `GET /info` and fills the table once on load (reload the page to refresh; there is no auto refresh). Neither route is authenticated (the network is controlled and exclusive). To move the page to SPIFFS/LittleFS later, only the `serve_asset()` callers change.
- `GET /info` returns the device information as JSON, unauthenticated. Fields: `project`, `version`, `build`, `idf`, `hw_version`, `partition`, `wifi` (`enabled`, and when enabled `iface` {`mac`,`ip`}, `ssid`, `rssi`), `ethernet` ({`mac`,`ip`,`link_up`,`speed_mbps`,`duplex`} or null; `speed_mbps` is 10 or 100 and `duplex` is `"half"` or `"full"`, both null while the link is down), `uptime_s`, `partition_usage` (`total_kb`, `used_kb`, `percent`), `heap` (`free`, `min_free`, and for the DMA-capable heap `dma_free`, `dma_min_free` = lowest ever, `dma_largest` = largest free block), `reset_reason`, `boot_count` (every boot, persistent), `panic_count` (cumulative boots after a panic or watchdog reset, persistent), `ota_state` (state of the running image: `new`, `pending_verify`, `valid`, `invalid`, `aborted`, `undefined`; null when no OTA state is recorded, e.g. after flashing over serial), `test_run` (`status`, `status_text`, `id`). Missing values are `null`. Keep the keys in sync with `main/index.html`.
- Remote log console (`log_server.c`): raw TCP on port 2323, read-only, one client. The port is **closed by default** (nothing for a network scanner to see). To use it:
  1. Call `GET` or `POST http://{IP}/console` with HTTP basic auth (`CONSOLE_AUTH_USER` / `CONSOLE_AUTH_PASS` constants in `preset_server.c`, defaults `admin` / `change-me-console`). The `X-API-Key` is not used here. In Postman: Authorization tab, type Basic Auth. A browser also works (it prompts for the credentials).
  2. The port opens for 30 s (`LOG_SERVER_WINDOW_S`) waiting for a connection. Calling `/console` again while waiting restarts the 30 s window.
  3. Connect with `nc {IP} 2323` or `telnet {IP} 2323`. As soon as a client connects the port is closed again, so a second client cannot connect.
  4. When the client disconnects, the session ends and the port stays closed. Call `/console` again to reopen.
  - Replies: `200` opened (or window restarted), `401` bad or missing credentials (with `WWW-Authenticate`), `409` a client is currently connected.
  - On connect it sends the most recent lines (4 KB ring buffer) and then streams new ones. Lines longer than 319 characters are truncated, and a "bytes dropped" marker appears if the client falls behind. Logs also still go to the UART.
  - The basic auth credentials travel in clear text over HTTP and there is no attempt limit, which is acceptable only on the controlled network.
- Console log lines are prefixed with `PRESET`.
- Request log (`main/http_log.c`): every HTTP request writes one line with tag `HTTP`: client IP, method, URI (with query, never headers or bodies), the status sent and the time spent, e.g. `192.168.1.20 GET /status -> 200 (3 ms)`. Status 400 and above is logged as a warning. It covers unknown paths and wrong methods too (`404`, `405`). `POST /iniciar` is logged when the worker sends the reply, so its time includes the preset download. Lines go to the UART and to the remote log console. To silence them: `esp_log_level_set("HTTP", ESP_LOG_WARN)`. New handlers must be registered with `http_log_register()` (not `httpd_register_uri_handler()`) and set non-200 statuses with `http_log_set_status()` / `http_log_send_err()`, otherwise they are logged as `200`.

## Admin endpoints (field maintenance)

Code: `main/admin_api.c` / `admin_api.h`, registered by `preset_server_start()` on the same HTTP server (port 80, Wi-Fi and Ethernet). The update logic is in `main/ota_button.c` (shared with the 10 s button flow). The web page (`GET /`) has a button for each action.

**Authentication:** HTTP basic auth with the same credentials as `/console` (`CONSOLE_AUTH_USER` / `CONSOLE_AUTH_PASS`). The `X-API-Key` is not used. Credentials travel in clear text, acceptable only on the controlled network.

**Extra header on every `POST`:** `X-Admin: 1` (any value). Browsers cache basic credentials, and a custom header cannot be added by a cross-site request without a CORS preflight, which the device never answers. Without it the reply is `403`. In Postman add it as a header.

| Endpoint | What it does |
|---|---|
| `GET /admin/status` | State of the current or last operation (JSON, see below) |
| `POST /admin/update/check` | Download the manifest and compare versions. Nothing is installed. `?channel=beta` uses the beta manifest |
| `POST /admin/update/apply` | Check, then download, decrypt and write the new image. Does **not** reboot: the image runs after the next reboot. `?channel=beta` as above |
| `POST /admin/format?confirm=yes` | Erase the NVS partition (saved settings, e.g. static IP), then reboot. Without `confirm=yes` the reply is `400`. After this a static IP is gone and the device returns on DHCP |
| `POST /admin/reboot` | Reboot |
| `GET /admin/history` | The last 20 `POST /iniciar` requests since boot, newest first, including rejected ones (JSON, see below) |
| `GET /admin/preset` | The last preset that was downloaded and decoded successfully: metadata plus the hex (JSON). `?format=text` returns only the hex string |
| `POST /console` | Open the log console port (unchanged, see above). Does not need `X-Admin` |

**Replies to the `POST`s** are JSON: `{"ok":true|false,"message":"..."}`.

| Code | Meaning |
|---|---|
| 202 | `check` or `apply` started. Poll `GET /admin/status` |
| 200 | `format` or `reboot` accepted. The action happens about 0.8 s later, after the reply is sent |
| 400 | `format` without `?confirm=yes` |
| 401 | Missing or wrong basic auth (with `WWW-Authenticate`) |
| 403 | Missing `X-Admin` header |
| 409 | A test run is in progress, or another admin operation is running |
| 500 | Not enough memory to start the operation |

**`GET /admin/status`** example:

```json
{"op":"idle","progress":0,"running":"1.0.0","remote":"1.0.1","update_available":true,
 "staged":false,"staged_version":"","test_running":false,
 "last":{"op":"check","ok":true,"message":"Update available: 1.0.1"}}
```

| Field | Description |
|---|---|
| `op` | `idle`, `checking`, `updating`, `formatting` or `rebooting` |
| `progress` | 0 to 100 while `op` is `updating` |
| `running` | Version of the running firmware |
| `remote` | Version seen in the last manifest (empty if none yet) |
| `update_available` | The last check found a newer version |
| `staged` / `staged_version` | A new image is already written and runs after a reboot |
| `test_running` | A test run (`/iniciar`) is in progress |
| `last` | Outcome of the last finished operation: `op` (`check`, `update`, `format`), `ok`, `message` |

**`GET /admin/history`** (both history and preset are RAM only: empty after a reboot or a format)

```json
{"now_s":8077,"entries":[
 {"seq":8,"uptime_s":8441,"id":"1235","product":"F02305200","cliente":"000417","estado":"MG",
  "result":"error","code":404,"level":"none","bytes":null,"crc32":null,"duration_ms":12400},
 {"seq":7,"uptime_s":7978,"id":"1234","product":"F02303100","cliente":"008249","estado":"SP",
  "result":"ok","code":200,"level":"customer","bytes":312,"crc32":"A1B2C3D4","duration_ms":3100}]}
```

| Field | Description |
|---|---|
| `now_s` | Device uptime in seconds when the reply was built (the device has no real clock) |
| `seq` | 1, 2, 3 ... since boot, one per request, also rejected ones |
| `uptime_s` | Uptime when the request arrived |
| `id`, `product`, `cliente`, `estado` | As received (ASCII only, truncated to 23 / 23 / 15 / 7 characters). `null` when missing or not parsed yet (e.g. a `401`) |
| `result` | `running`, `ok` (200), `error` (accepted but failed: 404 or 500) or `rejected` (never started: 400, 401, 409) |
| `code` | HTTP status sent, `null` while running |
| `level` | Which lookup matched: `customer`, `state`, `default`, or `none` (every location returned 404). `null` if not applicable |
| `bytes`, `crc32` | Size and CRC-32 (standard, as zlib / Python `binascii.crc32`, 8 hex digits) of the decoded preset. `null` if none |
| `duration_ms` | Time until the reply was decided, `null` while running |

**`GET /admin/preset`**

```json
{"available":true,"now_s":8077,"seq":7,"id":"1234",
 "source":"https://preset.ppa.com.br/agility123/008249/exemplo.conf","level":"customer",
 "bytes":312,"crc32":"A1B2C3D4","loaded_s":7978,"hex":"00112233AABB..."}
```

- `hex` is continuous uppercase hex, the same form as `exemplo.conf`, so it can be diffed against the server file.
- Before the first successful download since boot: `{"available":false,"now_s":N}` (with `?format=text`: `404`).
- A run whose hex fails to decode does **not** replace the stored preset: it keeps the last valid one. The `seq` field tells which run loaded it, so a newer failed request can be told apart from the content shown.
- The web page shows history and preset as two independent sections, each with its own refresh button. A history row whose CRC equals the viewer's gets an "in viewer" tag. Neither is loaded automatically, so opening the page does not trigger the password prompt.

**Rules between tests and maintenance**

- `check`, `apply`, `format` and `reboot` are refused (`409`) while a test run is in progress.
- `POST /iniciar` is refused (`409`, see the error table) while an update **download** (`apply`), a `format` or a `reboot` is in progress. A plain `check` does not block runs.
- The two checks are made so that a test run and an update can never start at the same time.

**Implementation notes**

- `check` and `apply` run in a temporary task (10 KB stack, TLS) and the HTTP task stays free.
- Updates need access to `download.citrox.com.br` (manifest and image, HTTPS) from the field network, including DNS.
- Only one admin operation runs at a time.
- Rollback: the new firmware is marked valid only after the HTTP server has started (`main.c`), not right after boot. If a new image crashes earlier, or the server fails to start, the bootloader goes back to the previous image on the next reset. It is not tied to getting an IP, so a unit booted without a cable keeps a good update.
- Not tested on hardware at the time of writing: the OTA download over Ethernet, and the rollback path with a deliberately bad image.

## Testing with Postman

Replace `{IP}` with the Prog IP (Wi-Fi or Ethernet). Add the header `X-API-Key: change-me-prog-key` to every request (or set it as a collection-level header).

### 1. Start a run

- Method: `POST`
- URL: `http://{IP}/iniciar`
- Headers: `X-API-Key`, `Content-Type: application/json`
- Body (raw, JSON):

```json
{
  "id": 1234,
  "produto": "F02303100",
  "cliente": "008249",
  "estado": "SP",
  "caminho": "https://preset.ppa.com.br/agility123/"
}
```

Expected: `200` and `{"id":1234,"status":1,"message":[]}`. The console shows the preset content and the simulated UART action.

### 2. Poll the status

- Method: `GET`
- URL: `http://{IP}/status`
- Header: `X-API-Key`

Expected: `status` 1 for about 5 s after the start, then 2. Before any run: `{"id":0,"status":0,"message":[]}`.

### 3. Error cases to try

| Test | How | Expected |
|------|-----|----------|
| Wrong or missing key | Remove or change `X-API-Key` | `401`, empty body |
| Invalid JSON | Send `{ "id": 1,` | `400`, status object, `status: 3` |
| Missing `id` or `caminho` | Remove the field | `400` |
| Path traversal | `"cliente": "../x"` | `400` |
| Busy | Send a second `POST` before `/status` shows 2 (use a long-running preset or shorten the wait between sends) | `409` with the current run's status object |
| Preset not found | Use a `caminho` where none of the 3 files exist | `404`, one message per tried URL |
| Download error | Use an unreachable host | `500`, one message per tried URL |
| Invalid hex | Serve an `exemplo.conf` with non-hex text or odd length | `500` |

Tip: to test without the real preset server, host an `exemplo.conf` (for example `00112233AABBCCDD`) on any local HTTP server, such as `python -m http.server 8000` in a folder, and use `"caminho": "http://{PC_IP}:8000/"`.
