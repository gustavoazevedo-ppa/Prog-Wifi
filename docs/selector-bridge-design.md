# Selector bridge: design

Design for putting the Prog between a product's central (the DUT) and the real selector,
so the Prog can forward the selector protocol transparently now and alter it later.

Protocol reference: `docs/selector-protocol.md` (frame format, functions, key bytes, timing).
Anything marked **[unverified]** there is still unverified here.

The master-mode ("connect") protocol is specified in `docs/connect-protocol.md`. It was extracted
from the original source files (current protocol with an encrypted variant, the discontinued
protocol, and the encoder / decoder), which are kept in another repository.

Status: phase 1 (sections 5 to 8, 7b, 9 and 10) is implemented in `main/selector_*.c` and builds
cleanly. It has not been tested on hardware yet. The master-mode parts (5.4 frame profiles, 6.3,
7a) are design only. The old throwaway `main/selector.c` test was removed.

---

## 1. Goals and non-goals

Goals
- Sit between the central and the selector, each side seeing a complete, well-behaved peer.
- Be transparent by default (everything the central sends reaches the selector, and the
  keys come back), with zero product knowledge in the core.
- Provide one place (the translator) where all known commands can later be inspected,
  changed, suppressed or generated: screens, key states, beeps, backlight.
- Keep protocol details in the ports so a future protocol only needs a new port.

Non-goals (for now)
- Menu injection, macros, parameter presets, value extraction, per-product profiles.
  They are translators built later on top of this core.
- Jig / BLE function codes. They are not known selector commands and are dropped in the
  selector role. (The master-mode "connect" protocol is a separate dialect, see section 6.3.)
- Any change to HTTP / admin APIs.
- Implementing the master-mode command set (read / save config). The design reserves room for
  it (sections 6.3, 7a) and the protocol is specified in `docs/connect-protocol.md`.

## 2. Decisions

| # | Decision |
|---|---|
| 1 | Frame format and baud are fixed: 9600 8N1, same frame layout always. Content and timing vary per product. |
| 2 | The Prog fully emulates both sides: a selector toward the central, a host toward the real selector. The two sides are decoupled and only meet in the translator. |
| 3 | The translator sees typed messages for every known command, not raw bytes. |
| 4 | Unsupported or malformed frames are dropped, counted and logged. |
| 5 | A new protocol is implemented in the port layer; the translator and bridge do not change. |
| 6 | Timing values are configuration, not constants. |
| 7 | Key state travels as level masks (lossless for transparent mode). Press / hold / release events are derived by a helper, not by the ports. |
| 8 | The central can be switched from master to slave with a special command, so the panel port has a dynamic role (selector or master). Master is the central's default. |
| 9 | The only way back to master is the central's own timeout, variable per product, mostly 500 to 1500 ms. There is no return command. |
| 10 | The switch command is sent in place of a reply, and all master-mode traffic is strict half-duplex (some centrals are full-duplex, others are not): send, wait for the reply, never transmit while receiving. |
| 11 | Frame layout exceptions (slightly different header and tail, minor field differences, plus an encrypted protocol variant) are handled with frame profiles. They are not implemented in phase 1. |
| 12 | The first planned use of master mode is saving presets to the central at initialization. The central de-duplicates writes and does its own wear leveling, so repeated saves of the same content are not a concern. |
| 13 | The Infos request (`'I'`, ID 2) is what puts the central in slave mode. Its reply bytes are not reliable and are not used to identify the central. There is no reliable automatic identification: the splash text has no stable pattern either. The product comes from the Stinger (`produto` in `POST /iniciar`). |
| 14 | Receivers accept stop bytes `0x81` and `0x51`. Toward the real selector the Prog sends `0x81` (the selector accepts both). Toward the central, the panel port echoes the tail of the frame it answers, per frame, and master mode will use the tail of the central's own frames. `0x81` is the default when none has been seen. |
| 15 | The ID byte distinguishes the dialects (1 = selector, 2 = connect). The framer accepts a set of IDs per role. |
| 16 | The preset is an opaque block. The Stinger provides the right file per central. The Prog decodes it, computes its size, and sends it raw in an `R` frame. Any result code counts as success for now. No read before write. |
| 17 | The encrypted variant is `byte ^ checksum ^ 0xAA` over ID, Function, Data and Counter (verified against `PROTECT.c`). It is implemented as a frame profile transform, later. |
| 18 | A later admin action reads the configuration from a reference central and returns it as hex, the source of the preset files. |
| 19 | Out of scope: the central type `0xFF` GPIO-only mode, and TX backup / restore. |
| 20 | The Prog menu is the default screen from boot. The central takes the screen on its first valid frame, and the menu returns after 3 s without one (section 7b). Navigation is manual with the arrow keys (`SEL` next, `OK` previous). |
| 21 | The menu shows all interfaces enabled in the firmware, even when down (IP `0.0.0.0`, real MAC), after a project / version page. During master mode the selector shows `PROGRAMMING...`. |
| 22 | The host port polls keys about every 40 ms and sends screens only on change plus a 1 s refresh. The selector has no no-host timeout, and measured data shows a 100 ms poll reports only 65% of quick presses. |
| 23 | The key state given to the central is not latched: a tap shorter than the central's own poll is lost, as with a direct connection. Revisit after the real-central test (`docs/open-questions.md`, D6). |

## 3. Hardware

| Peripheral | Use | Pins |
|---|---|---|
| UART0 | Console / logs | GPIO1 TX, GPIO3 RX |
| UART1 | Real selector (Prog is host) | GPIO25 TX, GPIO26 RX |
| UART2 | Central / DUT (Prog is selector) | GPIO17 TX, GPIO16 RX |

- UART1 defaults to the flash pins, so it is always remapped with `uart_set_pin()`.
- 10k pull-up to 3.3V on both RX lines (GPIO26, GPIO16). Shared ground. 3.3V on both sides.
- GPIO16/17 are PSRAM pins on WROVER modules. The module is an ESP32-WROOM-32D (no PSRAM), so
  they are free.

## 4. Architecture

```
central <-UART2-> [panel port] --downstream--> [bridge: translator] --downstream--> [selector port] <-UART1-> selector
(DUT)                          <--upstream----                      <--upstream----
```

Downstream = central to selector (screen, poll, beep, backlight).
Upstream = selector to central (key state).

| Layer | Owns | Does not know about |
|---|---|---|
| Wire | frame layout, checksum, stop byte rules, resync, length limits, message encode / decode | UARTs, tasks |
| Port (x2) | one UART, a protocol role, reply timing, link supervision | what the screens mean |
| Bridge | the task, queues, tick, drop statistics, tap | the wire format |
| Translator | message policy: pass, change, drop, generate | frames, baud, ACKs |

No shared mutable state between tasks: everything moves through queues (section 8).

## 5. Wire layer

Files: `selector_wire.c/.h`. Pure C, no ESP-IDF dependency (so it can be built and
replayed off-target).

### 5.1 Frame

```
[0]=0x7E  [1]=Size  [2]=ID  [3]=Function  [4..Size-3]=Data  [Size-2]=Checksum  [Size-1]=Stop
```
Checksum, stop bytes, ID and the test vector (`7E 08 01 3F B4 B4 B7 81` -> `0xB7`) are as in
`selector-protocol.md` section 2a.

### 5.2 Framer (byte-fed state machine)

API shape:

```c
typedef struct selector_framer selector_framer_t;

void selector_framer_init(selector_framer_t *f);
// Feeds one byte; returns true and fills `out` when a valid frame completed.
bool selector_framer_feed(selector_framer_t *f, uint8_t b, uint32_t now_ms,
                          selector_frame_t *out);
selector_drop_reason_t selector_framer_take_drop(selector_framer_t *f); // last drop, or NONE
```

Rules (avoids the alarm parser's hazards from `selector-protocol.md` section 5):
- Wait for `0x7E`. Next byte is Size; accept only `6 <= Size <= SELECTOR_MAX_FRAME` (64).
  Reject immediately otherwise.
- Collect until Size bytes. Accept stop `0x81` or `0x51`. Verify checksum and ID (`0x01`).
- On any failure, discard the first byte and rescan the already-buffered bytes for the next
  `0x7E` (resync), instead of waiting for more input.
- Partial frame timeout: if no byte arrives for `partial_timeout_ms` (default 25 ms,
  about 24 character times), drop the partial frame.
- The framer never waits for a size it cannot hold.

Drop reasons: `BAD_SIZE`, `BAD_STOP`, `BAD_CHECKSUM`, `BAD_ID`, `BAD_LENGTH`
(wrong length for the function), `UNKNOWN_FUNCTION`, `PARTIAL_TIMEOUT`. A buffer overflow cannot
happen: the size check bounds the buffer.

### 5.3 Messages

```c
typedef enum
{
    SELECTOR_MSG_SCREEN,     // downstream: 2 x 16 characters
    SELECTOR_MSG_POLL,       // downstream: request for key state
    SELECTOR_MSG_BEEP,       // downstream: on, off, count
    SELECTOR_MSG_BACKLIGHT,  // downstream: level 0..255
    SELECTOR_MSG_KEYS,       // upstream: key level mask
} selector_msg_type_t;

typedef struct
{
    selector_msg_type_t type;
    union
    {
        struct { char row[2][16]; } screen;
        struct { uint8_t on_ms, off_ms, count; } beep;
        struct { uint8_t level; } backlight;
        struct { uint8_t pressed; } keys;   // see SELECTOR_KEY_* bits, 1 = pressed
    } u;
} selector_msg_t;
```

Codec functions: `selector_decode(frame, msg)` and `selector_encode(msg, buf)`, plus
`selector_encode_reply(function, keys, buf)` for the 8-byte reply.

Mapping notes
- `SCREEN`: the full 38-byte frame maps to two 16-character rows. A short `'S'` frame
  writes its characters from the start and leaves the rest unchanged. **[unverified]**
  whether the real selector clears the rest instead; confirm on hardware and adjust.
- `KEYS`: canonical bits `SELECTOR_KEY_MINUS`, `PLUS`, `OK`, `SEL` (pressed = 1). The wire is
  active-low (idle `0xB4 0xB4`). Data1 bit 7 is `OK` and bit 4 is `SEL`, confirmed on a real
  selector. On some selectors these two keys are labelled `<-` (OK) and `->` (SEL). The mapping
  lives in one place in the codec. The pulse bits are not forwarded: on a real selector they only
  repeat the level (a "last edge" indicator, see `selector-protocol.md` section 4), and the
  replies to the central are rebuilt with the idle pulse-bit pattern.
- Messages forwarded to the real selector are re-encoded with stop `0x81`, even if the central's
  frame used `0x51` (the selector accepts both). Replies to the central are the exception: they
  echo the tail of the frame they answer (`selector_encode_reply()` takes the stop byte).

### 5.4 Frame profiles (planned, not in phase 1)

The layout is almost always the same, with exceptions: a slightly different header and tail,
minor field differences, and an encrypted variant of the protocol. The framer is therefore
parameterized by a profile instead of using `if` branches:

- Profile fields: start byte (`0x7E` or `0x5A`), accepted IDs, size semantics (total length),
  checksum algorithm (XOR then `^ 0xFF`, over all bytes before it), accepted stop bytes
  (`0x81`, `0x51`), maximum size (per profile, at most 255), and an optional transform. The
  encrypted variant has a counter byte before the checksum and applies
  `byte ^ checksum ^ 0xAA` to ID, Function, Data and Counter (details in
  `docs/connect-protocol.md`, section 2). The size range must be checked before decoding.
- The selector layout is the default profile. Each exception is another profile entry.
- In the selector role the profile is fixed. In master mode, parsing is request-scoped: the
  command table says which profile the reply uses, so the Prog never has to guess a frame
  type before parsing.
- Phase 1 implements only the default profile, but keeps the framer interface open for this.

## 6. Ports

Files: `selector_port.c/.h`. Two instances of one type, configured by role.

```c
typedef enum { SELECTOR_ROLE_SELECTOR, SELECTOR_ROLE_HOST } selector_role_t;

typedef struct
{
    uart_port_t uart;
    int tx_pin, rx_pin;
    uint32_t baud;               // 9600
    selector_role_t role;
    uint32_t partial_timeout_ms; // 25
    uint32_t link_timeout_ms;    // 500
    uint32_t reply_delay_ms;     // selector role: delay before the reply to the central, 10
    uint32_t poll_ms;            // host role: key poll interval while the selector answers, 40
    uint32_t down_poll_ms;       // host role: poll interval while it does not answer, 100
    uint32_t refresh_ms;         // host role: resend the screen after this long unchanged, 1000
    uint32_t reply_timeout_ms;   // host role: time allowed after the end of the host frame, 30
} selector_port_cfg_t;
```

A new protocol later means a new implementation behind the same inbound / outbound
message interface. It does not touch the bridge or the translators.

### 6.1 Panel port (UART2, plays the selector toward the central)

- Feeds RX bytes into the framer. For every valid downstream frame:
  1. Decode it to a message and queue it to the bridge.
  2. **Reply** `reply_delay_ms` after the end of the central's frame (`CENTRAL_REPLY_DELAY_MS` in
     `selector_bridge.c`, default 10 ms; the real selector takes 3 to 4 ms, and 0 means at once)
     with the 8-byte
     reply: the received function echoed, the cached key state from the last upstream
     `KEYS` message. Default cache is idle (no key). The reply ends with the same stop byte as
     the frame it answers (`0x81`, or the legacy `0x51`): a central that sends `0x51` needs
     the answer with `0x51`. Decided per frame, no state, and the checksum is not affected.
- The reply is sent whether or not the translator forwards the message, so the central
  never waits on the Prog's logic.
- Link: DOWN until the first valid frame; UP on every valid frame; DOWN after
  `link_timeout_ms` without one. Reports changes to the bridge.
- Upstream `KEYS` messages from the bridge only update the cache. Nothing is sent
  unsolicited.
- Unknown / malformed frames: no reply, count the drop.
- A central with no selector sends its initial screen (model and firmware version) until a reply
  arrives, then moves on to its menu after a short delay. Because this port replies to every valid
  frame at once, the central proceeds even when the real selector is not connected to the Prog.
  This is intended: the Prog keeps replying with idle keys while the selector link is down, so a
  central can be tested without a selector.

### 6.2 Selector port (UART1, plays the host toward the real selector)

Runs its own schedule, independent of the central. One request at a time: the next frame goes out
when the reply to the previous one is complete (or timed out) and its slot has come. A reply
is expected `reply_timeout_ms` after the end of the host frame (the selector answers 3 to 4 ms
after it, measured).

- State DOWN: send a `'?'` poll every `down_poll_ms` until a valid reply arrives, then UP.
- State UP: a slot every `poll_ms`. The default frame is the `'?'` poll, because keys are only
  reported in the reply to a screen or a poll, and the poll exchange is short (about 20 ms). Instead
  of the poll, in this priority:
  1. a pending beep (`'B'`), then
  2. a changed backlight (`'L'`), then
  3. the screen (`'S'`, always the full 38-byte frame) when it changed since the last one sent,
     or after `refresh_ms` without one. The selector has no no-host timeout, so the refresh is only
     there to recover from errors.
- Replies to `'S'` and `'?'` carry the key state; queue `KEYS` upstream when it changes (and
  once on link UP). Replies to `'B'` and `'L'` only count as link alive.
- No valid reply for `link_timeout_ms` goes to DOWN. On return to UP, resend backlight and
  screen (the selector may have reset).
- Keeps its own copy of the latest screen / backlight / pending beep, updated from the
  bridge's outbound queue. Poll messages toward the selector are ignored in this version.

### 6.3 Master mode (panel port, planned)

The central can be switched from master to slave, so the Prog can send it commands (first use:
saving presets). The panel port role is therefore dynamic.

```
SELECTOR -> SWITCHING -> MASTER -> WAIT_REVERT -> SELECTOR
```

| State | Behavior |
|---|---|
| `SELECTOR` | Default. The central is master and the Prog answers each frame (6.1). |
| `SWITCHING` | A switch request is pending. The next downstream frame from the central is answered with the **Infos request (`'I'`, ID 2)** instead of the normal reply. TX stays strictly half-duplex. No valid Infos reply within the timeout means the central does not speak this protocol (or uses the other variant), and the operation fails. |
| `MASTER` | The central is slave. The Prog sends one request and waits for its reply (or a per-request timeout) before the next. The central answers with a variable delay and ignores requests sent while it is processing, so there is no fixed gap and no overlap. It never transmits while receiving. |
| `WAIT_REVERT` | The Prog has gone silent. The central returns to master by timeout. Silence is expected here, not a fault. |

Rules
- **Stop byte.** Requests in master mode carry the tail of the central's own frames: the tail of the
  frame that was replaced by the Infos request, kept for the whole session (`0x81` if none was
  seen). Replies from the central are accepted with either tail.
- **No return command.** The central reverts by timeout only. The timeout is variable per
  product, mostly 500 to 1500 ms. It must be a per-product configuration value.
- **Revert indicator:** once back in master mode the central sends its initial screen (product
  name and firmware version, an ordinary `'S'` frame) repeatedly until the selector replies. So the
  first valid selector-dialect frame from the central is the sign that it has reverted. The text is
  not parsed: it has no stable pattern across models and firmware. The port goes back to
  `SELECTOR` and replies at once, as in 6.1.
- **Wait limit:** `WAIT_REVERT` ends in failure if no valid frame arrives within the longest
  expected timeout (1500 ms) plus a margin (to be set once the timeouts are measured).
- **Lease:** the slave-mode timeout varies by product (500 to 1500 ms). **[unverified]**: what
  resets it (any valid frame, or only some), and whether a request is needed in every session
  after the Infos. Until known, keep requests back to back and avoid long idle gaps.
- **Reboot safety:** if the Prog resets mid-sequence it boots in `SELECTOR` role and the central
  reverts by itself, so no recovery step is needed beyond the central's own timeout.
- **Side effects:** the selector port is unaffected (it keeps the real selector alive and can
  show a status screen). The translator gets a mode event so it does not mistake the central's
  silence for a link failure (section 7).
- **Transitions need a clean handover:** wait for TX idle, flush RX, apply a guard time.

## 7. Translator

Files: `selector_bridge.h` (contract), `selector_tr_*.c` (implementations).

```c
typedef enum { SELECTOR_DIR_DOWNSTREAM, SELECTOR_DIR_UPSTREAM } selector_dir_t;
typedef enum { SELECTOR_SIDE_CENTRAL, SELECTOR_SIDE_SELECTOR } selector_side_t;

typedef struct
{
    void *ctx;
    void (*on_msg)(void *ctx, selector_dir_t dir, const selector_msg_t *msg);
    void (*on_link)(void *ctx, selector_side_t side, bool up);
    void (*on_mode)(void *ctx, bool central_slave); // planned: master mode, see 6.3
    void (*on_tick)(void *ctx, uint32_t now_ms);   // every ~10 ms
} selector_translator_t;

// Only valid from inside the translator callbacks (bridge task).
void selector_bridge_emit(selector_dir_t dir, const selector_msg_t *msg);
```

- A translator is a stateful, time-driven object. One user key may become many central keys
  over time, and some keys may be swallowed.
- It may pass, change, suppress or generate any message in either direction.
- **Identity translator** (default): `on_msg` forwards the message unchanged in its own
  direction. Link events are ignored.
- **Prog menu translator** (first real feature, phase 1): identity while the central is
  talking, and the Prog's own menu (project / version page, then IP and MAC pages) when the
  central is silent. Specified in section 7b. It replaces the old `selector.c`.
- **Key helpers** (`selector_keys.c/.h`): edge detection to press / release events in
  milliseconds (needed in phase 1 by the menu). Later, press / hold detection and timed
  synthetic presses (hold a key long enough for the central's own debounce, about 3
  consecutive replies for a press and 8 for a hold in the alarm). The hold time is
  configuration.

Planned later translators: parameter presenter / binding (for example the 10 / 30 / 60 s
preset over a seconds setting), navigator / macro engine, extraction. They need no change
to the layers above.

## 7a. Central command service (planned)

A layer next to the translator that owns multi-step operations on the central in master mode,
for example "save presets" (and later "read parameters").

- It owns the whole sequence: request the switch, wait for it, send the commands, check the
  replies, go silent, wait for the revert (6.3), and report the result.
- Callers: the on-connect initialization, the preset flow (it would replace the body of
  `uart_task` in `preset_server.c`), and HTTP. Long work runs in its own task, and HTTP callers
  use the `http_log_async_*` helpers.
- Trigger on connection: wait until the central link is up and settled, then run the
  sequence. Run it again after a central reset (link down, then up).
- Operations (see `docs/connect-protocol.md`, section 4): **save preset** (Infos, then `R` with
  the opaque block, then go silent and wait for the revert) and, later, **read config**
  (Infos, then `V`, returned as hex through an admin endpoint).
- Time budget for a save today: the block is one frame (47 bytes for one central type, at most
  249), about 55 ms on the wire at 9600 baud, plus the Infos exchange and the revert wait of 0.5
  to 1.5 s. `/status` stays at `1` for the whole sequence.
- The 2 KB preset extension is being defined by the central firmware team. The service treats the
  block as an opaque length-prefixed blob so a multi-frame transfer can be added without
  changing its callers.
- Gating: refuse while a test run is active unless the sequence is part of that run, and
  authenticate any HTTP trigger, as for other admin actions.
- Status of a command is reported as ok / failed with a reason, never silently dropped.

## 7b. Prog menu translator (phase 1)

Shows the Prog's own information on the selector when the central is not talking, and gives the
central the screen back as soon as it talks again. Later it can host settings and other
information about the Prog itself.

### States

| State | Screen shown | Upstream keys |
|---|---|---|
| `MENU` (initial, from boot) | the Prog menu | not sent to the central |
| `CENTRAL` | the central's screens, forwarded unchanged | forwarded unchanged |
| `STATUS` | `PROGRAMMING...` | not sent to the central |

Transitions
- `MENU` to `CENTRAL`: on the first valid downstream frame from the central. No delay.
- `CENTRAL` to `MENU`: when 3000 ms have passed since the last valid downstream frame, and the
  central is not in master mode. The translator measures this itself with its tick. The port's own
  link-down event (500 ms) is not used for this. The menu always restarts at page 0.
- Any state to `STATUS`: when the central command service reports master mode (`on_mode`, see
  6.3 and 7a). Back to `CENTRAL` when the central is talking again (its first valid frame after
  the revert).

### Key handling

- In `MENU`, rising edges of `SEL` (`->`, right arrow) and `OK` (`<-`, left arrow) navigate: `SEL`
  goes to the next page, `OK` to the previous, wrapping at both ends. Other keys, including
  PLUS and MINUS, are ignored. There is no auto-rotation.
- On entering `MENU` the translator sends an idle `KEYS` upstream once, so the panel port's cached
  key state is idle while the central is silent.
- **Hand-back:** on `MENU` or `STATUS` to `CENTRAL`, any key that is still held is masked
  (reported as released to the central) until the selector reports it released. After that, keys
  are forwarded unchanged.

### Pages

1. **Project page.** Line 1 is the CMake project name, read at runtime from the app description,
   in uppercase (`PROG_WIFI`). Line 2 is `V<firmware version> HW<n>`, for example `V1.0.0 HW2`,
   using the firmware version from the app description (the same value `/info` reports, with a
   `V` added when it starts with a digit) and `HW_VERSION` from `device_info.h`. If it does not fit in 16 characters, the version is
   truncated and the `HW<n>` suffix is kept. The underscore renders correctly on the
   selector's display (checked on the bench).
2. **One IP page and one MAC page per interface enabled in the firmware**, in this order: WIFI IP,
   WIFI MAC, ETH IP, ETH MAC. Line 1 is the label (`WIFI IP`, `WIFI MAC`, `ETH IP`, `ETH MAC`) and
   line 2 is the value, with no page indicator.
   - An interface counts as enabled when `wifi_is_enabled()` / `ethernet_is_enabled()` returns
     true. A disabled interface has no pages.
   - An interface that is enabled but down or without an address shows IP `0.0.0.0`.
   - The MAC is shown in full, as 12 hex digits without colons (17 characters with colons do not
     fit in 16 columns). It comes from the chip, not from the netif, so it is correct even when
     the interface is down or its netif is missing: `esp_read_mac(ESP_MAC_WIFI_STA)` for Wi-Fi and
     `ethernet_get_mac()` for Ethernet. The Ethernet manager derives the ENC28J60 MAC from the
     eFuse MAC (first byte `0x02`), and `ethernet_get_mac()` exposes exactly that derivation, which
     `ethernet_init()` now uses too.
   - If no interface is enabled, only the project page exists.

The IP is read from the netif at a slow rate (for example every 500 ms) and the screen is
re-emitted when a value or the page changes. The selector port repeats the latest screen at its
own cadence. Reading the network state must never block the bridge task.


## 8. Bridge and tasks

Files: `selector_bridge.c/.h`.

| Task | Role | Priority | Stack |
|---|---|---|---|
| panel port | UART2 RX, framer, immediate reply | 5 | 3072 |
| selector port | UART1 cadence, RX, framer | 5 | 3072 |
| bridge | translator logic, ticks | 4 | 3072 |

Queues (copy by value, depth 8):
- `bridge_in`: messages and link events from both ports to the bridge task.
- `panel_in`: upstream `KEYS` from the bridge to the panel port.
- `selector_in`: downstream `SCREEN` / `BEEP` / `BACKLIGHT` from the bridge to the selector port.

The bridge task waits on `bridge_in` with a 10 ms timeout and calls `on_tick` on timeout. All
translator logic runs in that one task, so it needs no locks. The ports keep their own
caches, so there is no shared state between tasks. If an outbound queue is full the message is
dropped and counted, and the ports never block on the bridge.

### Statistics and tap

- Counters per port per drop reason, queue-full drops, valid frame counts, and link UP / DOWN
  transitions. Read with `selector_bridge_get_stats()`.
- Drops are logged at warning level, rate limited.
- A tap logs each valid frame with `ESP_LOG_BUFFER_HEX` at debug level, tagged with the port, so
  the log console shows real traffic. Needed to settle the unverified items.

## 9. Integration

- `selector_bridge_start()` replaces `selector_start()` at the same place in the boot sequence.
  It must not block boot, and a failure only logs. Firmware validation stays gated on the HTTP
  server, as today.
- Naming and style follow the project conventions (module prefix `selector_`, `#define TAG`,
  `ESP_LOGx`, Allman braces).
- Files to add to `SRCS` in `main/CMakeLists.txt`: `selector_wire.c`, `selector_port.c`,
  `selector_bridge.c`, `selector_keys.c`, `selector_tr_menu.c`. `driver` is already needed for
  UART, and the menu needs `esp_netif`, `esp_app_format` (app description) and `esp_system` (MAC).
- `selector.c` / `selector.h` were removed. The checksum and the frame builder moved into the
  wire layer.
- Gating of key injection into the central (refuse while a test run is active, authenticate the
  HTTP path) is the caller's responsibility when a translator action is triggered from HTTP.
  It is not part of this core.
- NVS layout is not touched.

## 10. Phase 1 (first slice)

1. Wire layer: framer, codec, checksum self-test vector.
2. Selector port and panel port with link supervision.
3. Bridge, queues, tick, identity translator.
4. Drop statistics and tap.
5. Key helper (edge detection) and the Prog menu translator (section 7b) replacing `selector.c`.

Implementation notes (phase 1, built, not yet run on hardware):
- Files: `selector_wire`, `selector_keys` (pure C), `selector_port`, `selector_bridge`,
  `selector_tr_menu`. The wire layer has a self-test (checksum vector and a poll frame round trip)
  that runs at start.
- Ports read one byte at a time with a 10 ms timeout, so a frame is handled as soon as its stop
  byte arrives and the reply to the central goes out right after it.
- Timing defaults (in `selector_bridge.c`): partial frame timeout 25 ms, link timeout 500 ms, host
  key poll 40 ms (100 ms while the selector does not answer), screen refresh 1000 ms, reply
  timeout 30 ms. Central side: reply delay 10 ms (`CENTRAL_REPLY_DELAY_MS`; sleeps for the coarse
  part and busy-waits the last ticks, because the FreeRTOS tick is 10 ms). The screen is sent only
  on change plus the refresh, since the selector keeps it on
  forever; the poll is what samples the keys (see `selector-protocol.md` section 4 for why 40 ms).
- `selector_bridge_get_stats()` returns the per-port counters (valid frames, drops per reason,
  queue-full drops, reply timeouts, link changes). Drops are also logged, at most once per
  second per port.
- `selector_bridge_set_tap(true)` logs valid frames in hex at INFO level on both ports (tags
  `SEL_PANEL` and `SEL_HOST`). A frame identical to the previous one in the same direction on the
  same port is skipped, and the next different frame says how many repeats were skipped
  (`TAP_CHANGES_ONLY` in `selector_port.c`, set it to 0 to log every frame). The default in
  `selector_port.c` is currently on (`s_tap`), and there is no HTTP switch yet.
- Observed on the bench with the tap: after the fixed-rate change the host replies arrive exactly
  100 ms apart, a key reply is followed by the new menu screen about 40 ms later (within one
  cycle), and the link stayed up with the screen unchanged for about 85 s (846 identical replies).
- Ethernet is currently disabled in the firmware (`ETHERNET_ENABLED` is 0), so the menu has the
  project page and the Wi-Fi pages only.

Acceptance (on the bench, real central and real selector):
- At boot the selector shows the Prog menu. The first central frame replaces it immediately.
  After 3 s without central frames the menu returns on its first page.
- `SEL` and `OK` (the arrow keys) navigate the pages with wrap-around. Disabled interfaces have no pages. A down
  interface shows `0.0.0.0` and its MAC.
- A key held while the central takes the screen back does not reach the central until released.
- Screens and key presses behave exactly as with the central wired directly to the selector.
- The central never loses its link under normal operation.
- Injected garbage and unknown function codes are dropped, counted and logged without
  breaking the link.
- Unplugging the selector or the central moves only that side to DOWN, and it recovers on
  reconnect (screen redrawn after the selector returns).
- The project builds with `.\scripts\build.ps1`.

## 11. Open items

All open questions, for the selector side, the central side and master mode, are kept in
`docs/open-questions.md`, with the resolved ones.

## 12. Master-mode protocol

Analysed and written up in `docs/connect-protocol.md`. Its open items are in
`docs/open-questions.md` (section C).
