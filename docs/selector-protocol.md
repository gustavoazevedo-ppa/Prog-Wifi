# Selector (XMC1100 display + keys) — protocol and integration notes

Reference for talking to the "selector": an XMC1100 board with a 16x2 HD44780 LCD,
four keys and a buzzer, used as a screen + keyboard for other products. The ESP32
would act as the **host** (the role the alarm firmware plays today).

Sources:
- `docs/seletor_prog.c` / `docs/seletor_prog.h` — host-side code from the alarm
  firmware (the code is the source of truth where sources disagree, except where
  noted in section 7).
- A Word document ("Prévia do Protocolo de Comunicação com o Inversor") describing
  the selector commands.
- The XMC1100 datasheet pin table (Table 9, Port I/O Functions), from a screenshot.

The selector's own (XMC1100) firmware has **not** been seen. Anything marked
**[inferred]** or **[unverified]** must be confirmed on real hardware.

Scope: the jig / production-test protocol and the BLE function codes found in the
same source files are out of scope and intentionally not documented here.

---

## 1. Purpose in this project

Show on the 16x2 LCD the IP and MAC addresses of Wi-Fi and Ethernet (rotating
pages when both are up), and read four keys, without spending ESP32 GPIOs on a
parallel LCD and buttons.

---

## 2. Serial settings

**9600 baud, 8 data bits, no parity, 1 stop bit (8N1)**, no flow control. Confirmed on the
alarm project (the alarm's `UART_0` configuration). Logic level 3.3V on the ESP32 side;
the selector side is expected to be 3.3V too (the XMC1100 supports 1.8 to 5.5V).

At this rate one byte takes about 1.04 ms (10 bits on the wire), so a 38-byte `'S'` frame
takes about 40 ms and an 8-byte reply about 8.3 ms.

---

## 2a. Frame format

```
[0]=Start  [1]=Size  [2]=ID  [3]=Function  [4..n]=Data  [Size-2]=Checksum  [Size-1]=Stop
```

| Field | Value |
|---|---|
| Start | `0x7E` |
| Size | Total frame length in bytes (start and stop included) |
| ID | `0x01` = selector |
| Function | ASCII letter, see section 3 |
| Data | 0 to 249 bytes (document), depends on function |
| Checksum | XOR of all bytes before it (Start..last Data), then complemented (`^ 0xFF`) |
| Stop | `0x81` when sending; accept both `0x81` and `0x51` when receiving (see note) |

**Note on the stop byte (history):** the intended value is `0x81`, the bit-flipped
version of the start byte `0x7E` (`0111 1110` becomes `1000 0001`). Some programmers
implemented it as the *decimal* number 81, which is `0x51`, and the mistake was found
only after thousands of products were already in the field. Since those units cannot be
fixed, receivers accept both values as the end of a frame (the alarm parser does the
same, `UART_STOP` and `UART_STOP2`). For the ESP32:
- **Receive:** accept `0x81` or `0x51` as the last byte.
- **Send:** use `0x81` (the documented value, and the one the alarm sends to the
  selector). Whether the selector would also accept `0x51` is unknown; do not rely on it.
- The checksum does not cover the stop byte, so both variants share the same checksum.

Size examples: 3 data bytes → `4 + 3 + 2 = 9`; 2 data bytes → 8; 1 data byte → 7.

### Checksum

```c
uint8_t checksum(const uint8_t *frame, uint16_t len) {
    uint8_t x = 0;
    for (uint16_t i = 0; i < len - 2; i++) x ^= frame[i];
    return x ^ 0xFF;
}
```

Known-good test vector (from the alarm code, `lcd_temp`):
`7E 08 01 3F B4 B4 B7 81` — checksum `0xB7`.

### The Word document's example checksums are wrong

Recomputed with the formula above (which matches the alarm code):

| Frame | Document says | Correct |
|---|---|---|
| `'S'` "PPA" (`7E 09 01 53 50 50 41`) | 0x6F | **0x9B** |
| Beep 2/2/3 (`7E 09 01 42 02 02 03`) | 0xEC | **0xC8** |
| Key poll (`7E 08 01 3F B4 B4`) | 0xEB | **0xB7** |
| Backlight 255 (`7E 07 01 4C FF`) | 0xD6 | **0x34** |
| `'S'` reply with `B4 B4` | 0xEB | **0xDB** |

The backlight example also shows data `0x127`, which is not a byte; use `0xFF` for 100%.

---

## 3. Functions

| Function | Char | Hex | Data | Meaning |
|---|---|---|---|---|
| Screen | `'S'` | 0x53 | text | Write text to the LCD |
| Keys | `'?'` | 0x3F | 2 bytes | Poll key state |
| Buzzer | `'B'` | 0x42 | 3 bytes | Beep |
| Backlight | `'L'` | 0x4C | 1 byte | Backlight level 0..255 (Word doc only; not used by the alarm code) |

### 3.1 Screen `'S'`

Full-screen form used by the alarm code (38 bytes, `UART_SIZE_LCD`):

```
7E 26 01 'S' <16 chars line 1> <16 chars line 2> CHK 81
```
- Data is 32 bytes: line 1 at offsets 4..19, line 2 at offsets 20..35.
- Lines are space-padded to 16 characters; longer text is truncated to 16.
- The alarm always sends the whole screen; there is no cursor positioning,
  clear command or partial update in the alarm code.

Short form from the document: `7E 09 01 'S' 'P' 'P' 'A' CHK 81` (text only, 3 chars).
**Confirmed by the project owner:** both forms work. The short form is accepted, but
the result is not well aligned. **[inferred]** The selector most likely treats the data
as one continuous 32-character buffer starting at the top-left (characters 0..15 on
line 1, 16..31 on line 2), so short text always starts at the first character of
line 1 and nothing positions it. To put text on line 2, or to align it, fill the gap
with spaces. That is what the alarm's padding does.

**Use the full padded 38-byte frame for anything that needs a layout.** It is the form
used by the shipping alarm firmware and overwrites the whole display. The short form is
only suitable for simple first-line messages.

**[unverified]** Whether a short frame clears the rest of the screen or leaves old
characters in place (relevant when the text changes length). Test on the real selector.

Character set is the HD44780 ROM: use plain uppercase ASCII (the alarm's menu
strings are uppercase ASCII, no accents).

### 3.2 Key poll `'?'`

Fixed poll frame the alarm sends only while the selector is not answering (idle):

```
7E 08 01 3F B4 B4 B7 81
```
The `B4 B4` data bytes are filler (same pattern as the idle key state).

### 3.3 Buzzer `'B'`

```
7E 09 01 'B' <on> <off> <count> CHK 81
```
- `on` and `off` in units of 1 ms (per document), `count` = number of beeps.
- The alarm's `beepLCD()` takes `unsigned int` arguments but stores them in single
  bytes, so each value is effectively 0..255.
- Alarm usage on every accepted key press: `beepLCD(30, 30, 1)`.

### 3.4 Backlight `'L'`

```
7E 07 01 'L' <level> CHK 81      level: 0 = off, 255 = 100%
```
**[unverified]** Only in the Word document. The header declares a
`UART_BACKLIGHT_T` struct but no function code or code path uses it.

---

## 4. Replies from the selector

Every reply echoes the function that was sent and always carries the key state:

```
7E 08 01 <function> <Data0> <Data1> CHK 81
```
- Reply to `'S'` is `'S'`, reply to `'?'` is `'?'` (per document; the alarm code
  handles both identically).
- This acts as an ACK for each command.
- **[unverified]** Whether the selector ever sends frames unsolicited, and its
  reply latency.
- In the alarm code only `'S'` and `'?'` replies update the key state. Replies to
  `'B'` (and `'L'`) are accepted as "link alive" but their key bytes are ignored.

### Key bytes

Bits are **active low** (0 = pressed, 1 = released/idle). Idle value observed: `0xB4`
= `1011 0100`.

Data0 (consistent between code, header and document):

| Bit | Meaning |
|---|---|
| 7 | Key `-` level (Nivel) |
| 6 | Key `-` pulse low |
| 5 | Key `-` pulse high |
| 4 | Key `+` level (Nivel) |
| 3 | Key `+` pulse low |
| 2 | Key `+` pulse high |
| 1:0 | unused |

Data1 — **the sources disagree on which key is which**:

| Bits | Header struct (`UART_KEY_Type`) | Word document |
|---|---|---|
| 7 (level), 6, 5 | `OK` | `SEL` |
| 4 (level), 3, 2 | `SEL` | `OK` |
| 1:0 | unused | unused |

The alarm code calls Data1 bit 7 "left" and bit 4 "right". Its menu text suggests
bit 7 (left) is the "yes / confirm" key and bit 4 (right) is "no / next":
`"[ZONA 1]:(S) (N)"` is answered with left = S (sim), right = N (nao), and in the
status screen right goes to the next menu and left to the previous one. That favours
the header's naming (bit 7 = OK, bit 4 = SEL). **[unverified]**: confirm by pressing
the physical keys and reading the bits, and by checking the labels printed on the
selector board.

Only the **level** bits are used by the alarm code. The pulse high/low bits may let the
selector report a tap shorter than one poll interval **[unverified]**.

### Host-side debounce used by the alarm

- One 8-bit shift register per key, shifted left once for each `'S'` or `'?'` reply
  that arrives (so it is driven by the reply rate, about 10 Hz in the alarm).
- The key's level bit is inverted into the register's LSB (pressed = 1).
- A press event fires when the last three samples are `001` (a fresh press after at
  least two released samples) and again on every sample while the register is `0xFF`
  (key held for 8 consecutive samples, about 800 ms at 100 ms/sample).
- The registers are not reset when the link is lost.
- If the ESP32 samples faster or slower than 100 ms, the time constants change;
  keep a fixed sample rate or rewrite the debounce in milliseconds.
- Multiple keys can be read at the same time (the alarm uses PLUS+MINUS together for
  factory reset).

Alarm key usage on the status screen, as a reference for typical key roles:
PLUS alone = arm, MINUS alone = disarm, PLUS+MINUS = factory-reset sequence,
right = next menu, left = previous menu.

---

## 5. Host behaviour in the alarm firmware (reference)

State machine (`prog_sm()`), time source `SYSTIMER_GetTickCount()` (assumed to be 1 ms):

1. **IDLE** (selector not answering): send the key poll frame every **100 ms**
   (`PROG_STATUS_DELAY`). When a valid frame has been received in the last 500 ms,
   go to INIT.
2. **INIT**: show the splash screen (`" ALARME C4 V1.1 "`, line 2 empty) every 100 ms
   for about 10 frames (`PROG_STATUS_INIT = 8`, compared with a post-increment, so
   9 increments plus the final one; about 1 s), then go to the normal screens.
3. **Normal screens**: refresh the full 38-byte `'S'` frame every **100 ms**.
   **The `'?'` poll is not sent in this phase; the `'S'` replies carry the key
   state.** So the ESP32 only needs the poll while the selector is not answering.
4. In every state: if no valid frame has arrived for **500 ms**
   (`PROG_STATUS_TIMEOUT`), go back to IDLE and resume polling.
5. Each accepted key press triggers `beepLCD(30, 30, 1)`.

The remaining ~1400 lines of `seletor_prog.c` (lines ~980 to 2380) are the alarm's menu
(remotes, sensors, times, zone types, ...). They were scanned by pattern only; all
display output goes through `putsLCD()`, `printLCD()` and `beepLCD()`, and no other
protocol use was found there.

### Receive parser (alarm side)

The ISR pushes bytes into a 128-byte ring buffer; `prog_service()` copies them to a
128-byte work buffer, scans for `0x7E`, reads the Size byte, waits for the full frame,
checks that the last byte is `0x81` or `0x51`, verifies the checksum, then handles the
frame. Any valid frame (any function) refreshes the "last seen" tick.

What it does **not** check:
- The **ID byte** is never validated. Any ID is accepted.
- The frame **length is not validated against the function**; `'S'`/`'?'` handlers read
  Data0/Data1 at fixed offsets 4 and 5 without checking Size.

### Parser hazards found (do not copy these into the ESP32 parser)

1. **Possible permanent stall.** The check "is the frame complete yet?"
   (`rx_in - offset < Size` → wait for more bytes) runs *before* the check that rejects
   frames that cannot fit in the 128-byte buffer. If a stray `0x7E` is followed by a Size
   larger than the buffer can ever hold (≥ 128, for example `0x81`, which is also the
   stop byte), the parser waits forever because the buffer can never get that many
   bytes. A noise byte, or a checksum byte equal to `0x7E` followed by the stop byte
   `0x81`, could trigger this after a resync.
2. **Size 0 or 1 is not rejected up front.** Size 0 reads one byte *before* the
   frame (index -1 when at the start of the buffer) and, if the byte pattern happens to
   pass the stop/checksum tests, the scan offset advances by 0 and the loop never ends.
   Size 1 is rejected only because the byte at that position is the start byte.
3. A bad checksum or bad stop byte only moves the scan forward by one byte, which resyncs
   but also re-examines every following `0x7E` inside the payload. Payload bytes of the
   frames we use (ASCII text, key bytes) rarely contain `0x7E`, but the *checksum byte*
   can.
4. Frames that fit `Size + offset >= 128` are dropped one byte at a time. Effective
   maximum frame size is 127 bytes, not the 255 the Size byte can express.
5. The ring buffer's full test (pointer arithmetic on volatile pointers) is odd
   at the wrap point; not relevant if the ESP-IDF UART driver is used instead.

For the ESP32: clamp Size to a sane range (for example 7..64), reject bad stop or
checksum immediately, resync by searching for the next `0x7E`, and discard a partial
frame after a timeout of a few character times.

---

## 6. `printLCD()` helper and number formatting (alarm-local)

`printLCD(line1, line2, value1, value2)` builds the same 38-byte `'S'` frame, but first
substitutes `%d` / `%0d` placeholders in the text. The selector never sees `%`; it is
the alarm's own formatting. It is **not** safe to reuse:

- The substitution loop runs over `sizeof(sequence)` = 64 bytes while only 38 are set, so
  it scans uninitialised stack bytes, and reads `sequence[i+1]` / `[i+2]` past the end
  at the last index.
- Substitution *overwrites characters before the `%`*: values ≥ 100 overwrite the
  preceding character, values ≥ 1000 overwrite the two preceding characters, and `%0d`
  always writes four digits starting one character before the `%`. A `%` at index 0 would
  write outside the buffer.
- `%0d` writes four digits with `% 100`/`% 1000` arithmetic; values above 9999 give wrong
  digits.
- The status screen uses it to print zone states as a four-digit number where each digit
  is 0 or the zone number (for example `1010`).

For the ESP32, format the text with `snprintf` into a 16-char buffer and pad to 16.

---

## 7. Inconsistencies in the source

- **Data1 key naming** differs between the header struct and the Word document
  (section 4).
- **Splash length:** the constant `PROG_STATUS_INIT = 8` actually yields about 10
  splash frames because of the `stateCount++ > 8` post-increment test.
- **Header structs are not wire layouts.** `UART_HEAD_Type` uses `enum` fields for ID
  and Function, which are normally 4 bytes each in C unless the compiler is told
  otherwise, and nothing in the files packs the structs. They are also never used in
  the `.c` file (frames are built by hand in byte arrays). Use the frame layout in this
  document, not `sizeof` of those structs.
- `UART_KEY_Type` uses `uint16_t` bitfields; the bit order only matches the wire
  layout if the compiler allocates bitfields LSB-first (true for ARM GCC).
- **Function code `'B'`** is defined twice in the header enum (`UART_FUNCTION_BUZZER`
  and `UART_FUNCTION_BLE_BACKUP`, a different device). Only the buzzer meaning applies
  to the selector; the `switch` only handles the second name, which is why `'B'` replies
  fall through to an empty case.
- `UART_START_PROTECT 0x5A` and `UART_ID_BLE`/the other function enum values are defined but
  never used for the selector.
- The header includes no standard headers (uses `uint8_t`, `uint16_t` without
  `<stdint.h>`) and the `.c` uses `strlen`/`NULL` without including `<string.h>` itself;
  both rely on `wifi.h` being included first.
- `putsLCD()`/`beepLCD()`/`printLCD()` each declare a `static` checksum variable that
  serves no purpose (a local would do).
- The Word document's checksum and backlight examples are wrong (section 2a).

---

## 8. Still unknown / to find out

1. ~~Baud rate and serial format~~ — resolved: **9600 baud, 8N1** (confirmed on the
   alarm project, see section 2).
2. XMC1100 selector firmware: reply timing, USIC receive FIFO depth, any watchdog or
   no-host timeout, whether short `'S'` frames clear the rest of the screen, whether pulse bits allow
   sub-poll-interval taps, whether `'L'` is implemented, which Data1 key is which.
3. Whether the selector can send key frames unsolicited.
4. Whether the tick in the alarm is really 1 ms (the constants are commented as ms).
5. How `UART_Transmit()` behaves in the alarm (blocking, interrupt or DMA); it matters
   only to understand how back-to-back frames were tolerated on the selector side.

---

## 9. Hardware link (ESP32 <-> selector)

### ESP32 pins (proposed)

| Peripheral | Use | Pins |
|---|---|---|
| UART0 | Console / logs | GPIO1 (TX), GPIO3 (RX) |
| UART1 | Selector (this protocol) | GPIO25 (TX), GPIO26 (RX) |
| UART2 | DUT | GPIO17 (TX), GPIO16 (RX) |

- UART1 defaults to GPIO9/10 (flash pins), so it must always be remapped with
  `uart_set_pin(UART_NUM_1, GPIO_NUM_25, GPIO_NUM_26, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)`.
- GPIO25/26 are clean at boot (no strapping, no boot glitches).
- Add a 10k pull-up to 3.3V on the ESP32 RX line so it does not float if the selector
  is unpowered. Share GND. Both sides at 3.3V.

Other pins already in use in this firmware: GPIO4/5/18/19/23 (ENC28J60), GPIO32/33
(LEDs), GPIO34 (OTA button), GPIO6–11 (flash). Free if needed: 13, 14, 21, 22, 27
(outputs OK), 35/36/39 (input only). Avoid strapping pins 0, 2, 12, 15; GPIO14 glitches
at boot.

### XMC1100 pins (VQFN-24, per datasheet Table 9)

| Pin | Usable for UART? |
|---|---|
| P0.0 | No (only `SELO0` outputs and `DX2A` input) — plain GPIO |
| P0.5 | No (only `CCU40.OUT0`) — plain GPIO |
| P0.6 | Yes: TX = `U0C1.DOUT0` (ALT7), RX = `U0C1.DX0C` |
| P0.7 | Yes: TX = `U0C1.DOUT0` (ALT7), RX = `U0C1.DX0D` |
| P0.14 / P0.15 | SWD programming/debug — keep reserved |

Recommended: use USIC0 channel 1 (U0C1) with TX on P0.6 and RX on P0.7 (or swapped).
Wiring: ESP32 TX (GPIO25) → XMC RX; ESP32 RX (GPIO26) → XMC TX.

**[unverified]** That the VQFN-24 package bonds out P0.6 and P0.7; check the package pin
table. The data above comes from a datasheet screenshot showing only P0.0–P0.8.

---

## 10. Design notes for the implementation

### Timing budget (9600 8N1, confirmed)

| Item | Bytes | Time on the wire |
|---|---|---|
| Full `'S'` frame (host to selector) | 38 | about 40 ms |
| Reply (selector to host) | 8 | about 8.3 ms |
| Key poll `'?'` | 8 | about 8.3 ms |
| Beep `'B'` | 9 | about 9.4 ms |

In the alarm's 100 ms refresh cycle the link is busy for about 48 ms (full screen +
reply), so roughly half the time. A beep in the same cycle adds about 10 ms plus its
reply (about 8 ms), which is still within the cycle but leaves little slack; avoid
sending several frames back to back. Because replies are ACKs, the ESP32 can send only on change plus
periodically, instead of every 100 ms. Short `'S'` frames can save bandwidth but do
not align text (section 3.1); keep the full padded frame for layouts.
If the screen is only sent on change, keys still need a periodic frame (a poll `'?'` or
a screen refresh) to be sampled, since key state only arrives in replies.

### Key handling

- Keys are only sampled when a reply arrives (about 10 Hz if sending every 100 ms).
  Taps shorter than the sample interval can be missed unless the pulse bits work.
- No key-up events from the selector; derive press/release/long-press on the ESP32 from
  successive samples.
- Alarm autorepeat starts after about 800 ms of holding.
- Only `'S'` and `'?'` replies carry usable key state in the alarm code; if the ESP32
  sends `'B'` or `'L'`, treat their replies as link-alive only unless verified otherwise.

### Display pages (16x2)

- IP up to 15 chars (`192.168.100.200`) fits on a line.
- MAC with colons is 17 chars and does **not** fit; use 12 hex chars without colons
  (`AABBCCDDEEFF`).
- Four pages: Wi-Fi IP, Wi-Fi MAC, Ethernet IP, Ethernet MAC. Rotate every few seconds
  when both interfaces are up; show only the active one otherwise. PLUS/MINUS could
  step pages manually.

### Robustness checklist

- Retry/timeout per command using the reply as ACK.
- Link supervision like the alarm: 500 ms without a valid reply means "selector gone";
  go back to polling with `'?'` and redraw when it returns (it may have reset).
- Bounded parser (see section 5: length clamp, immediate stop/checksum rejection,
  partial-frame timeout).
- Validate the ID byte and expected length per function.
- Build screen text from a snapshot of network state; never block the UART task on
  HTTP/log-server work.
- Self-test the checksum against the `0xB7` test vector at startup/in a unit test.

### Possible later idea (not planned)

Bridging selector and DUT UARTs directly in the ESP32 GPIO matrix (pad-to-pad, no CPU)
or in a software relay between UART1 and UART2. Not tested; the `SIG_IN_FUNC` routing
would need bench verification.
