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
- `GET /` and `GET /index.html` return an HTML info page for identifying the device on the network. It is **not authenticated** (the network is controlled and exclusive). It shows project name, firmware version, build date and time, ESP-IDF version, hardware version, running OTA partition, Wi-Fi and Ethernet MAC and IP, Wi-Fi SSID and RSSI, uptime, free and minimum free heap, reset reason, and the current test run status and id. No JSON `/info` endpoint for now.
- Console log lines are prefixed with `PRESET`.

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
