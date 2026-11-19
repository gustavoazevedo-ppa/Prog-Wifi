# Open questions

Everything still unanswered about the selector bridge and the master-mode (connect) protocol,
in one place. Items were collected from `selector-bridge-design.md` (sections 11 and 12),
`connect-protocol.md` (section 5) and `selector-protocol.md` (section 8), plus decisions that
came up later. Those documents now point here, so this is the only list to update.

How to read it:
- **Blocks** says what the answer changes. "Nothing now" means the current build does not depend on it.
- **How** says how to find the answer.
- Resolved items are logged at the end, so they are not asked again.

The frame tap (`selector_bridge_set_tap(true)`) is the main instrument for the bench items. It is
off by default and has no HTTP switch yet (see D4).

---

## A. Selector, on the bench with the real selector

| ID | Question | Blocks | How |
|---|---|---|---|
| S3 | Partly answered: the reply arrives about 60 ms after the screen frame is queued and about 30 ms after a poll (log resolution 10 ms), so the selector adds roughly 10 to 20 ms to the time on the wire. Still open: the exact latency (reply timeout is 80 ms, so the margin is about 20 ms) and whether the selector has a no-host timeout (blank screen, reset). | The defaults: reply timeout 80 ms, link timeout 500 ms. | Measure with the Saleae. Stop the host and watch the selector. |
| S4 | Does a short `'S'` frame clear the rest of the screen, or leave the old text? The decoder assumes it overwrites only the first characters. | Handling of short `S` frames sent by a central. | Send a short frame after a full one and look. |
| S6 | Is the backlight command `'L'` implemented? It appears only in the Word document. | Any translator that dims the backlight. | Send `'L'` and look. |
| S7 | Beep `'B'`: units of the on / off values (1 ms per the document). The reply carries key state like every reply (F3, resolved). | Translators that generate beeps. | Send a beep and measure. |
| S8 | Does the selector's package bond out the pins used for UART (P0.6 / P0.7)? | Probably moot, the hardware works. | Confirm with the board schematic. |
| S9 | Reference only: is the alarm's tick really 1 ms, and how does its `UART_Transmit()` behave? | Nothing now. | Alarm source. |

## B. Central, on the bench with a real central

| ID | Question | Blocks | How |
|---|---|---|---|
| C1 | Which functions does each central send (`'?'`, `'B'`, `'L'`, short or full `'S'`), and what is its reply deadline? | Reply timing of the panel port, decode of unusual frames. | Tap on the UART2 side, per central model. |
| C4 | Does a central that sends the `0x51` tail work through the bridge now that the panel port echoes the tail? Built, not yet tested on a real `0x51` central. | The tail echo (a capture of the central and of the Prog's reply would also show what the real selector answers when it is addressed with `0x51`). | Connect a `0x51` central, check the Saleae capture of UART2 and the menu. |
| C5 | Is the 10 ms reply delay (`CENTRAL_REPLY_DELAY_MS`) right? It is longer than the real selector (3 to 4 ms) and was chosen for centrals that need a gap; the central's reply deadline is not measured. | Reply timing toward every central (too long may make a central time out, too short may be ignored). | Try it on each central family; measure the deadline with the Saleae (C1). |

## C. Master mode (central firmware team, or a capture)

| ID | Question | Blocks | How |
|---|---|---|---|
| M1 | What are the values of the `R` result byte, and how is an error reported? Any value counts as success for now. | Error reporting of the save preset operation. | Central firmware team. |
| M2 | Is the Infos request needed in every session before `V` and `R`, or only to enter slave mode? | The save and read sequences. | Central firmware team, or try. |
| M3 | What resets the central's slave-mode timeout (any valid frame, or only some)? And its value per product (500 to 1500 ms)? | Keepalive rules; the revert wait limit (1500 ms plus a margin). | Central firmware team. |
| M4 | What is the format of the 2 KB preset extension (new command or extension of `R`, single or multiple frames)? | Anything above 249 bytes. | Central firmware team, in progress on their side. |
| M11 | Does the same tail rule (use the tail of the central's own frames) hold for the protected variant (start byte `0x5A`)? | Master mode on protected centrals. | Central firmware team, or a capture. |
| M6 | How does the Prog pick the plain or the protected variant for a given central? Current guess: try plain, then protected, and remember it. | Entering master mode on protected centrals. | Central firmware team, or per-model list. |
| M7 | Is the preset file exactly the data field of `R`, with no header or checksum to strip? Assumed yes ("send it raw"). | Save preset. | Stinger side: compare a file with a `V` read from the same central. |
| M8 | Are the `'C'` command numbers the same on every central, and does every central implement every command? | Commands from the Prog. | Central firmware team. |
| M9 | In the `'H'` reply, which byte holds which group of accessories and failures bits? | Showing status. | Compare with a central in a known state. |
| M10 | TX backup row of the spreadsheet: request length 6 with a note, or length 8 with a zero index as in the old code? | Nothing now, TX backup is out of scope. | Bench, if it is ever needed. |

## D. Decisions to confirm on the first run

| ID | Question | Notes |
|---|---|---|
| D3 | How to expose the frame tap and the bridge counters: an admin endpoint, a build option, or both? Counters could also go into `/info`. | Needed for the bench items above. Nothing is decided. |
| D4 | The endpoints for reading a config from a reference central (`POST /admin/central/read`, `GET /admin/central/config`) are proposals. | Confirm names and format before building them. |

## E. Stinger side

The unknowns about the Stinger's HTTP client, polling interval and `exemplo.conf` details are
listed in `preset-api-spec.md` ("Still unknown (Stinger side)").

## F. Macros (design in `selector-bridge-design.md` section 7c)

Nothing open. The answered questions are under Resolved, and a known issue is noted in the design (section 7c, Replay).

## G. Jig protocol (central firmware team, or a capture)
Base for porting the jig test (`jiga-protocol.md`). Not implemented yet. The jig test runs on the same central UART (UART2) as the selector bridge, so it cannot run at the same time as the bridge.
| ID | Question | Blocks | How |
|---|---|---|---|
| J1 | What is the timeout, per product, after which the central reverts to master mode when the jig stops requesting, and what restarts it (any valid request or only some)? For the connect protocol the timeout is 500 to 1500 ms (M3). | The longest gap the test link may have; the priority and period of the link task. | Central firmware team, or stop the requests and measure with the Saleae. |
| J2 | If the timeout happens while a motor command is active, does the motor stop or keep the last command? | Whether a stalled Prog is a hazard; the stop-on-every-exit rule. | Central firmware team. |
| J3 | Does the central accept jig requests at any time while it is in master mode, or only in a window after power-up? | Whether the Prog needs to control the central's power or reset on the line. | Central firmware team, or a capture of a jig run. |
| J5 | The jig protocol shares the central UART (UART2) with the selector dialect. How does the Prog hand the link over cleanly: what the central sends while it switches from master to slave mode, and what the real selector (UART1) sees meanwhile? | Design of the jig task and its coexistence with the bridge (see `selector-bridge-design.md` section 3). | Bench capture of a jig run with the Saleae, central firmware team. |
---

## Resolved

| Item | Answer |
|---|---|
| ESP32 module | ESP32-WROOM-32D, no PSRAM, GPIO16 / 17 are free |
| Underscore in `PROG_WIFI` | Renders correctly on the selector |
| S2: pulse bits and short taps | The pulse bits are a "last edge" indicator, redundant with the level, and do not record a tap that falls between two polls (Saleae capture, 46 presses, none of the 9 invisible ones left a trace). At a 100 ms poll only 65% of presses were reported. The host port now polls keys about every 40 ms (see `selector-protocol.md` section 4) |
| S3: reply latency and no-host timeout | The reply starts 2.9 to 4.2 ms after the host frame ends (median 3.5 ms). The selector has no no-host timeout: a screen stays on forever (project owner). The host port still refreshes the screen every 1 s |
| S5: unsolicited frames | None seen: no reply without a host frame, and both lines silent for the first 7.8 s of the capture |
| D6: hold a short tap for the central | Decided: leave the central path as a direct connection would behave (no latch). Only the Prog menu gets the faster key poll. Revisit after the real-central test (C2): if quick taps on the central's menus feel missed, the preferred fix is a queue that delivers each tap in its own reply, not a simple latch (a latch could merge taps of different keys into a false chord, for example the alarm's `+` and `-` factory-reset sequence) |
| Selector key debounce | A press needs about 20 to 30 ms of contact, a release is reported at once (Saleae capture, inferred) |
| Reply to the poll `'?'` | The selector echoes `'?'` and carries the key state, like the reply to `'S'` |
| D1: do polls count as "the central is talking"? | Closed. The standard is that a central sends its initial screen indefinitely until the selector replies, so polls are not the case to handle. The menu keeps taking the screen on any valid frame (phase 1 only. The macro design replaces this with the explicit `[ENTER CENTRAL]` entry, see `selector-bridge-design.md` section 7c) |
| D5: reply to the central with no selector connected | Keep replying (with idle keys). The central proceeds to its menu and a central can be tested without a selector |
| M5: splash text pattern | It has no stable pattern across models and firmware, so it is not parsed. Revert detection uses the first valid selector-dialect frame from the central. Product identification comes from the Stinger (`produto` in `POST /iniciar`) |
| C3: central with no selector present | The central keeps sending its initial screen (model and firmware version, a `'S'` frame) until a reply from the selector arrives. It then waits a short delay and shows the first page of its menu. It does not fall back to polls |
| S1: OK / SEL key mapping | Data1 bit 7 = `OK`, bit 4 = `SEL`, as in the header struct (the Word document had them swapped). Some selectors label them `<-` (OK) and `->` (SEL). The firmware now calls them `LEFT` (bit 7) and `RIGHT` (bit 4). The menu navigates with them |
| Ethernet MAC on the menu | Same derivation as the Ethernet manager (`ethernet_get_mac()`) |
| J4: which Prog UART reaches the product's jig port? | UART2 (GPIO17 / 16), the same central UART the selector bridge uses (project owner). The selector, jig and connect dialects share this one link, one at a time, and the central chooses the mode (master = selector dialect, slave = jig or connect). No extra hardware is needed. A jig run has to take the panel port from the bridge, see J5 |
| UART map | UART1 selector (GPIO25 / 26), UART2 central (GPIO17 / 16), hardware wired |
| Stop byte | Accept `0x81` and `0x51`. Toward the real selector send `0x81` (it accepts both, confirmed). Toward the central, echo the tail of the frame being answered, per frame (implemented in the panel port); master mode will use the tail of the central's own frames |
| What puts the central in slave mode | The Infos request (`'I'`, ID 2), sent in place of a reply |
| Reliability of `I` reply bytes | Not reliable, not used to identify the central (nor is the splash text, see M5) |
| Config block layout | Opaque. Field names in the old struct are unreliable and fields are reused across models |
| Reading before writing | Not needed |
| `R` result codes | Any code counts as success for now |
| Write wear | Not a concern, the central avoids repeated identical writes and levels wear |
| Central type `0xFF` GPIO mode, TX backup / restore | Out of scope |
| Master-mode source files | Kept in another repository, extracted into `connect-protocol.md` |
| D2: is the 3 s silence rule right in practice? | Yes (bench). `CENTRAL_SILENCE_MS` stays at 3000, now the presence threshold. `CENTRAL_KEY_IDLE_MS` (3 s with no key pressed, restarted by a held key) was added for macros |
| F1: central behavior when the Prog stops replying | On the bench central (`POP CONNECT/V0.1`) it repeats its start screen about every 1 s (1.09 s measured) and shows it again after about 1 s without communication. Other models may take longer, but certainly under 5 s, so `POPUP_RESET_WAIT_MS` (5000) stays. The 3 s presence threshold still has to exceed the retry interval of every model |
| F2: pop-up capture rule | Confirmed OK on the bench: first screen when the central goes from absent to present (boot included), or after an exit the first screen that differs from the last one forwarded |
| F4: central poll rate | Steady, so counting polls for macro timing is valid. Wait-for-screen is not needed for now |
| F8: does the 3 s key idle time get in the way while recording? | No (bench). Keep `CENTRAL_KEY_IDLE_MS` at 3000 |
| F3: do the replies to `'B'` and `'L'` carry key state? | Yes (project owner): every reply carries the pressed keys in its payload, so every reply the panel port sends counts as a poll for macros, whatever the function. Macros recorded before that change were counted the old way and must be recorded again |
| F5: press and release length each central needs | Varies per central. Not a concern for recorded macros, because the macro holds the values seen on the central it was recorded on. Only hand-edited macros need care |
| F6: does the central finish its last operation during the trailing wait? | Yes (project owner) |
| F7: which dropped-frame reasons fail a replay? | Bad size, bad stop byte, bad checksum, bad length and a frame cut off by the partial timeout fail it (`selector_drop_is_corruption()`). A frame for another id and an unknown function (jig / BLE) do not. Drop events are sent only while a replay runs; a drop whose event the bridge queue did not accept stays pending and is sent again. Bench-tested OK. Known issue noted in `selector-bridge-design.md` 7c: a decode drop reported at the end of a panel pass can be hidden behind a success |
| Version marker in the macro format | Decided: none. A change in what the syntax means (as the F3 poll count did) is handled by recording the macros again. Unknown tokens are still rejected, so new token types stay safe for old firmware |
| C2: does the central tolerate replies built from cached keys instead of live key state? | Yes: the central menus were operated through the bridge on the bench |
| F9: `POST /iniciar` refused with 409 while the Prog is in transparent mode, for every run | Decided by the project owner and bench-tested: every run is refused with 409 while the menu is transparent or replaying a macro from the menu |
| Timeout reverts the central to master mode | Confirmed (project owner): a timeout on the jig protocol or on the connect protocol returns the central to master mode (selector dialect). There is no return command. The Prog has to keep talking during a session. The timeout values are still open (M3, J1) |
| J3 (part): is the jig banner the start screen? | Yes (project owner): the banner the jig listens for is the central's start screen (the "pop-up" in the Prog code), a selector `'S'` frame repeated until the selector replies. On a valid selector reply the central moves to its first menu entry |
