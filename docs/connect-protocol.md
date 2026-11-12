# Connect protocol (master mode toward the central)

Protocol the Prog uses to talk to a central (gate opener, alarm panel) when the Prog is the
master and the central is the slave. First use: save a preset to the central. Second use: read
the central's configuration into a file (the source of the preset files).

This is separate from the selector protocol (`docs/selector-protocol.md`), where the central is
the master and the Prog plays the selector. The two share the frame envelope. The ID byte tells
them apart.

Sources (none of them are stored in this repo; the code files live in another repository):
- The current implementation used in the field (`connect` new protocol, with the encrypted
  variant).
- The discontinued implementation (`connect` old protocol; it still works, but was dropped
  because of bugs in the central firmware). It has the read / write config code.
- `PROTECT.c/.h`: the encoder / decoder of the encrypted variant.
- A spreadsheet of the messages (screenshot, not stored in the repo): the most complete and most
  recent description of the message set. Where it disagrees with the header files, it wins.
- Statements from the project owner, marked **[owner]**.

What was extracted is recorded here and in Appendix A.

Marks: **[inferred]** is my reading of the code. **[unverified]** needs a bench check.

Implementation status: not implemented. See `docs/selector-bridge-design.md` for where it fits.

---

## 1. Roles and session

- The central is the master by default (it drives the selector). **[owner]** The Infos request
  (`'I'`) makes the central go slave. **[owner]**
- The Prog sends the Infos request in place of a reply to the central's next frame
  (half-duplex, see the design doc, section 6.3). **[owner]**
- In slave mode the central answers each request, with a variable delay. It ignores any
  request sent while it is still processing the previous answer, so the Prog always waits for
  the reply (or a timeout) before the next request. There is no minimum gap between commands.
  The 250 ms and 500 ms delays in the old code are arbitrary. **[owner]**
- The central returns to master by a timeout only. There is no return command. The timeout varies
  by product, mostly between 500 and 1500 ms. **[owner]** The first sign that it is back is its
  initial screen (product name and firmware version, an ordinary selector `'S'` frame), which it
  sends repeatedly until the selector replies. **[owner]** The text has no stable pattern across
  models and firmware, so it is not parsed: the first valid selector-dialect frame is the signal.
- **[unverified]** What resets the central's slave-mode timeout.
- **[unverified]** Whether `'I'` is needed in every session before `'V'` or `'R'`, or only to
  enter slave mode.

## 2. Frame

```
[0]=Start  [1]=Size  [2]=ID  [3]=Function  [4..Size-3]=Data  [Size-2]=Checksum  [Size-1]=Stop
```

| Field | Value |
|---|---|
| Start | `0x7E` (standard), `0x5A` (protected variant) |
| Size | Total frame length |
| ID | `0x02` for this protocol |
| Function | ASCII letter, case-sensitive (`'M'` and `'m'` differ) |
| Checksum | XOR of all bytes before it, then `^ 0xFF` (same as the selector protocol) |
| Stop | Send `0x81` only. Accept `0x81` or `0x51` on receive. **[owner]** |

Note: the new code's header swaps the two stop constants and transmits `0x51`. The project
decision is to transmit `0x81` always. **[owner]**

The old code computes the checksum as XOR of bytes `1..` with `^ 0x81`. For `0x7E`-start frames
that is identical to the formula above (`0x7E ^ 0xFF = 0x81`), so it is not a bug. It would be wrong
for `0x5A` frames.

Request frames computed with the formula (not copied from a capture):

| Request | Frame |
|---|---|
| `'I'` Infos | `7E 06 02 49 CC 81` |
| `'V'` read config | `7E 06 02 56 D3 81` |
| `'H'` status | `7E 06 02 48 CD 81` |
| `'M'` maximum | `7E 06 02 4D C8 81` |
| `'m'` minimum | `7E 06 02 6D E8 81` |
| `'A'` TX actuation | `7E 06 02 41 C4 81` |

### Protected variant (encrypted)

```
0x5A, Size (7 + n), ID, Function, Data (n), Counter, Checksum, Stop
```

- A counter byte is added before the checksum. The new code increments it per frame.
- The checksum is computed on the plaintext frame (including the `0x5A` start byte and the
  counter), then the transform is applied.
- The transform covers ID, Function, Data and Counter. Start, Size, Checksum and Stop stay in
  clear.
- The transform (`encoder()` / `decoder()` in `PROTECT.c`), verified by simulating the original
  code for all byte and checksum values (no mismatches):

  ```
  byte = byte ^ checksum ^ 0xAA
  ```

  The same operation encodes and decodes. It is obfuscation and carries no secret. If the
  checksum equals `0xAA` the key is `0x00` and the data goes out unchanged.
- The receiver decodes first and then verifies the checksum. Check the Size range before
  decoding (the original computes `Size - 4` without validation).
- The new code detects the variant by trying: after a timeout waiting for the Infos reply, it
  toggles between plain and protected and tries again. **[inferred]**

## 3. Messages

Request and reply, as in the spreadsheet. "Config size" depends on the central type. **[owner,
spreadsheet]**

| Func | Name | Request | Reply |
|---|---|---|---|
| `I` | Infos | Size 6, no data | Size 10: central type, automation model, firmware version, firmware revision |
| `C` | Command | Size 10: command + 3 stuff bytes | Size 10: command echo, result, 2 stuff bytes |
| `V` | Read config | Size 6 | Size 6 + config size: the values stored in the central |
| `R` | Write config | Size 6 + config size: the values | Size 7: one result byte |
| `H` | Status / failures | Size 6 | Size 16: cycles (4 bytes), infos, status, accessories (2), failures (2) |
| `M` | Maximum values | Size 6 | Size 6 + config size: maximum values |
| `m` | Minimum values | Size 6 | Size 6 + config size: minimum values |
| `A` | TX actuation | Size 6 | Size 8: index of the actuated TX (2 bytes) |
| `B` | TX backup | quantity query, or Size 8 with a 2-byte index | quantity (2 bytes), or Size 11: index (2) + TX code (3) |
| `T` | TX restore | Size 8 with the maximum index, or Size 11 with index (2) + code (3) | Size 7: result |

- Not all rows are used by the Prog. In scope for now: `I`, `R`, `V`. The rest are listed for
  completeness. TX backup / restore is out of scope for now. **[owner]**
- **[unverified]** The first TX backup row of the spreadsheet shows Size 6 with a note about
  "TEM TX GRAVADO", while the old code sent Size 8 with a zero index. Settle on the bench if TX
  backup is ever needed.
- **[unverified]** The spreadsheet's column headings run up to Data 10. Rows with other sizes are
  annotated in the sheet and should be taken from the Size column.

### `I` Infos

- The only goal of the Infos request is to enter slave mode and prove the central speaks this
  protocol. **[owner]**
- The four reply bytes are **not reliable**: several models return the same values. Do not use
  them to identify the model or firmware. Log them only. **[owner]**
- There is no reliable automatic identification of the model or firmware: neither the reply bytes
  nor the splash text (no stable pattern) can be used. The product comes from the Stinger
  (`produto` in `POST /iniciar`).

### `V` read config / `R` write config

- The data field is an opaque block of N bytes. N depends on the central type (47 for one type
  in the old code). **[owner, spreadsheet]**
- **Do not interpret the block.** Field names, offsets and meanings in the old `CONFIG_t` struct are
  unreliable, and fields are reused across models and firmware versions (for example the field
  named `gIdioma` is reused for other features). **[owner]** The Prog sends and reads the bytes
  verbatim.
- `R` overwrites the whole block. The old flow was read-modify-write, but the Prog does not
  read first. **[owner]**
- The central skips writes with identical content and does its own wear leveling, so repeated
  writes are not a concern. **[owner]**
- `R` reply: Size 7, one result byte. Any result code is treated as success for now, and the
  code is logged and reported. Several centrals always return 0. **[owner]**
- Size limit: the Size byte allows a block of at most 249 bytes. A larger block needs the
  extension below.

### 2 KB preset extension (future)

The central firmware team is adding an extension of the command, or a new command, to carry
presets up to 2 KB. **[owner]** Nothing is specified yet. The Prog design must keep:
- The block as an opaque length-prefixed blob.
- The maximum frame size as a per-profile value.
- The sequence logic independent of whether the transfer is one frame or several.

## 4. Operations built on the messages

### 4.1 Save a preset (first use)

Input: the preset file served by the Stinger for this central. The file is hex text of the
binary block, as in `docs/preset-api-spec.md`. **[owner]** The Stinger provides the right file
for each central. The Prog downloads it, computes its size, and sends it raw. **[owner]**

1. Decode the hex into N bytes (the preset flow already does this).
2. Reject N above 249 until the extension exists.
3. Replace the reply to the central's next frame with the Infos request. Wait for the Infos
   reply. No valid reply within the timeout is a failure of the operation.
4. Build `R`: `7E, Size = 6 + N, 02, 'R', <N bytes>, CKS, 81`, compute the checksum and send it.
5. Wait for the 7-byte reply. Record the result byte. Any value counts as success for now.
6. Go silent. The central reverts to master by timeout. Wait for its first valid selector-dialect
   frame (its initial screen), up to the longest expected timeout (1500 ms) plus a margin.

If the variant is protected, apply the transform to each frame. Reading the central first is
not needed. **[owner]** A read-back compare is optional later.

### 4.2 Read a configuration into a file (reference central)

An HTTP action to read the configuration from a reference central, to produce the preset files
manually. **[owner]**

1. Enter slave mode with the Infos request (step 3 above).
2. Send `V`, read the reply, and take the data field (N bytes) as the block.
3. Go silent and wait for the revert (the central's first valid frame). The text of that first
   screen can be kept as free text, for the operator to read, as provenance of the file. It is
   not parsed.
4. Expose the result as uppercase hex without separators, the same form as the preset files, so
   a captured file can be fed back into the save operation (round trip). Include the length,
   the CRC-32 and the first screen text next to it.

Proposed endpoints, in the style of the existing admin endpoints (`docs/preset-api-spec.md`):

| Endpoint | What it does |
|---|---|
| `POST /admin/central/read` | Start the read. `202`, then poll. |
| `GET /admin/central/config` | The last result (JSON), or `?format=text` for the hex only |

They need basic auth and the `X-Admin` header, are refused while a test run is active, and run
one admin operation at a time. The browser or Postman saves the response to a file, so the Prog
needs no file system.

## 5. Open items

Tracked in `docs/open-questions.md` (section C), with the resolved ones.

---

## Appendix A. Reference extracted from the original sources

The source files live in another repository and are not stored here. This appendix keeps the
parts that may be needed later. Everything below comes from the headers and comments of the
original code. The config block layout is deliberately not recorded (see section 3).

### A.1 `'C'` command codes

The data byte of a `'C'` request (the first data byte). **[unverified]** whether every central
implements every code, or uses the same numbering.

| Value | Name (Portuguese) | Meaning |
|---|---|---|
| 0 | IDLE | none |
| 1 | ABRIR | open |
| 2 | FECHAR | close |
| 3 | TRAVAR | lock / block |
| 4 | PARAR | stop |
| 5 | GRAVAR | learn / record a transmitter |
| 6 | LUZ | garage light |
| 7 | TRAVA | lock (relay module) |
| 8 | APAGAR | erase all transmitters |
| 9 | PADRAO | factory default |
| 10 | RESET | path reset |
| 11 | LIMPAR | clear failures |
| 12 | ACIONAR | drive motor |
| 13 | EXCLUIR | delete one transmitter |
| 14 | RELE | relay |

The old code treated a `PADRAO` (factory default) command as a reason to go back to listening
instead of continuing the status loop. **[inferred]** The central probably restarts.

### A.2 `'H'` status reply (10 data bytes)

| Bytes | Content |
|---|---|
| 0 to 3 | cycles, 32-bit, big-endian |
| 4 | infos (bit field) |
| 5 | status (value) |
| 6 to 7 | accessories, 16-bit, big-endian (bit field) |
| 8 to 9 | failures, 16-bit, big-endian (bit field) |

Infos bits: 0 programming, 1 button, 2 TX triggered.

Status values: 1 open, 2 opening, 3 closed, 4 closing, 5 semi-open, 6 locked (0 means error).

Accessories bits (header comment): 0 light, 1 lock, 2 open, 3 close, 4 photocell 1, 5 photocell 2,
6 opening reed, 7 closing reed, and a second group of 8 whose bit 0 is the pushbutton.
Failure bits (header comment): 0 overheat, 1 low voltage, 2 high voltage, 3 overcurrent,
4 encoder failure, 5 no EEPROM, 6 empty EEPROM. The remaining bits are unused.
**[unverified]** Which group of 8 maps to which byte: the commented-out old code tested the
failure bits as `Failures & (1 << n)` on the 16-bit value, so the first group is likely the low
byte.

The header also declares a different, older status layout (position letter, TX count, sensors).
The handler code does not use it. Use the table above.

### A.3 Session sequences in the original code

Old (`discontinued`): wait for any valid frame, then `I`, `V`, `H`, TX quantity (`B`), then `R` if
a write is pending, then `C` if a command is queued, then loop on `H`. A wait over 2000 ms for a
reply goes back to listening. The delay between steps was 250 ms.

New (current): wait for any valid frame, then `I`, then loop on `H`, with a `C` between polls
when a command is queued. The delay between steps was 500 ms. A wait over 2000 ms for the
`I` reply toggles the protected variant on or off and goes back to listening. A wait over
2000 ms for an `H` or `C` reply resets the module. The new code dropped `V`, `R` and the TX
quantity request, even though the central still supports `V` and `R`.

### A.4 Other notes

- Jig mode: a frame with ID `'J'` and function `'T'` made the module reply with a jig status
  frame (ID `0x47`), and checksum errors on received jig frames were ignored. This is a
  production-test feature of the old module. Not used by the Prog.
- Central type `0xFF` made the old module drive its pins as plain GPIO instead of speaking the
  protocol. Out of scope. **[owner]**
- The old module republished status changes to a cloud service (Tuya data points). Not relevant.
