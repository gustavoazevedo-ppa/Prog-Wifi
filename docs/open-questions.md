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
| S2 | What do the pulse bits mean, and are short taps lost between polls? The bridge does not forward them. | Missed short key taps; fidelity of transparent mode. | Tap a key briefly with the tap on and compare level and pulse bits. |
| S3 | What is the selector's reply latency, and does it have a no-host timeout (blank screen, reset)? | The defaults: reply timeout 80 ms, link timeout 500 ms. | Measure on the tap. Stop the host and watch the selector. |
| S4 | Does a short `'S'` frame clear the rest of the screen, or leave the old text? The decoder assumes it overwrites only the first characters. | Handling of short `S` frames sent by a central. | Send a short frame after a full one and look. |
| S5 | Does the selector ever send frames on its own? | Whether the host port must accept frames without a request. | Watch the RX side with the tap while idle. |
| S6 | Is the backlight command `'L'` implemented? It appears only in the Word document. | Any translator that dims the backlight. | Send `'L'` and look. |
| S7 | Beep `'B'`: units of the on / off values (1 ms per the document) and whether the reply carries key state. | Translators that generate beeps. | Send a beep and measure. |
| S8 | Does the selector's package bond out the pins used for UART (P0.6 / P0.7)? | Probably moot, the hardware works. | Confirm with the board schematic. |
| S9 | Reference only: is the alarm's tick really 1 ms, and how does its `UART_Transmit()` behave? | Nothing now. | Alarm source. |

## B. Central, on the bench with a real central

| ID | Question | Blocks | How |
|---|---|---|---|
| C1 | Which functions does each central send (`'?'`, `'B'`, `'L'`, short or full `'S'`), and what is its reply deadline? | Reply timing of the panel port, decode of unusual frames. | Tap on the UART2 side, per central model. |
| C2 | Does the central tolerate replies built from cached keys, about 100 ms old, instead of live key state? | The whole decoupled design. | Operate the menus of a real central through the bridge. |
| C3 | What does a central do with no selector present? Does it keep sending polls (`'?'`) instead of screens? | The menu rule: see D1. | Unplug the selector, watch the central. |

## C. Master mode (central firmware team, or a capture)

| ID | Question | Blocks | How |
|---|---|---|---|
| M1 | What are the values of the `R` result byte, and how is an error reported? Any value counts as success for now. | Error reporting of the save preset operation. | Central firmware team. |
| M2 | Is the Infos request needed in every session before `V` and `R`, or only to enter slave mode? | The save and read sequences. | Central firmware team, or try. |
| M3 | What resets the central's slave-mode timeout (any valid frame, or only some)? And its value per product (500 to 1500 ms)? | Keepalive rules; the revert wait limit (1500 ms plus a margin). | Central firmware team. |
| M4 | What is the format of the 2 KB preset extension (new command or extension of `R`, single or multiple frames)? | Anything above 249 bytes. | Central firmware team, in progress on their side. |
| M5 | Does the splash text (product name and firmware version) have a stable pattern across models and firmware? | Revert detection and product identification. | Capture the splash of each model. |
| M6 | How does the Prog pick the plain or the protected variant for a given central? Current guess: try plain, then protected, and remember it. | Entering master mode on protected centrals. | Central firmware team, or per-model list. |
| M7 | Is the preset file exactly the data field of `R`, with no header or checksum to strip? Assumed yes ("send it raw"). | Save preset. | Stinger side: compare a file with a `V` read from the same central. |
| M8 | Are the `'C'` command numbers the same on every central, and does every central implement every command? | Commands from the Prog. | Central firmware team. |
| M9 | In the `'H'` reply, which byte holds which group of accessories and failures bits? | Showing status. | Compare with a central in a known state. |
| M10 | TX backup row of the spreadsheet: request length 6 with a note, or length 8 with a zero index as in the old code? | Nothing now, TX backup is out of scope. | Bench, if it is ever needed. |

## D. Decisions to confirm on the first run

| ID | Question | Notes |
|---|---|---|
| D1 | Should a central that sends only polls count as "talking"? Today any valid frame, including `'?'`, takes the screen. The screen then stays on the last menu page, because no screen arrives. | Depends on C3. The alternative is to switch to the central only on a `'S'` frame. |
| D2 | Is the 3 s silence rule right in practice? A central that pauses briefly may flash the menu. | Tune `CENTRAL_SILENCE_MS` in `selector_tr_menu.c`. |
| D3 | How to expose the frame tap and the bridge counters: an admin endpoint, a build option, or both? Counters could also go into `/info`. | Needed for the bench items above. Nothing is decided. |
| D4 | The endpoints for reading a config from a reference central (`POST /admin/central/read`, `GET /admin/central/config`) are proposals. | Confirm names and format before building them. |

## E. Stinger side

The unknowns about the Stinger's HTTP client, polling interval and `exemplo.conf` details are
listed in `preset-api-spec.md` ("Still unknown (Stinger side)").

---

## Resolved

| Item | Answer |
|---|---|
| ESP32 module | ESP32-WROOM-32D, no PSRAM, GPIO16 / 17 are free |
| Underscore in `PROG_WIFI` | Renders correctly on the selector |
| S1: OK / SEL key mapping | Data1 bit 7 = `OK`, bit 4 = `SEL`, as in the header struct (the Word document had them swapped). Some selectors label them `<-` (OK) and `->` (SEL). The menu navigates with them |
| Ethernet MAC on the menu | Same derivation as the Ethernet manager (`ethernet_get_mac()`) |
| UART map | UART1 selector (GPIO25 / 26), UART2 central (GPIO17 / 16), hardware wired |
| Stop byte | Transmit `0x81` only, accept `0x81` and `0x51` |
| What puts the central in slave mode | The Infos request (`'I'`, ID 2), sent in place of a reply |
| Reliability of `I` reply bytes | Not reliable, not used to identify the central. The splash text identifies it |
| Config block layout | Opaque. Field names in the old struct are unreliable and fields are reused across models |
| Reading before writing | Not needed |
| `R` result codes | Any code counts as success for now |
| Write wear | Not a concern, the central avoids repeated identical writes and levels wear |
| Central type `0xFF` GPIO mode, TX backup / restore | Out of scope |
| Master-mode source files | Kept in another repository, extracted into `connect-protocol.md` |
