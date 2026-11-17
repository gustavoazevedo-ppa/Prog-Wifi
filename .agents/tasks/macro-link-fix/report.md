# Macro link fix: report
Nothing committed.

## Files changed
- `main/selector_port.c/.h`: panel link is now a session. Link UP only when a reply is sent (gate open). The port task polls the gate flag and the 500 ms timeout in its loop and emits Link DOWN when the gate is closed or the timeout passes. Gate closed: no link events, no link log lines. Host port untouched.
- `main/selector_bridge.h`: comment only.
- `main/selector_tr_menu.c`: presence tracking and new capture rule (below).
- `docs/selector-bridge-design.md` (6.1 Link bullet, 7b transition note, 7c reply gate bullet, capture, new presence bullet, clear). `docs/open-questions.md` (F1 measurement, F2 rule).

## Decisions on the open points
1. **Gate close detection:** no new mechanism. `reply_gate_open` is already a `volatile bool` (one writer, one reader). The panel task checks `link_up && !reply_gate_open` every loop (at most 10 ms, the read timeout) and calls `port_set_link(false)`. Non blocking, no generation counter needed. Open then closed within one loop pass produces no event, which is harmless.
2. **Menu on link events:** `menu_on_link()` no longer clears the pop-up or arms captures. It only keeps the selector side bookkeeping (redraw on selector link up). The central side is a comment, no logic.
3. **Presence in the menu:** `central_present` plus `last_central_ms` (set on every valid downstream frame in every state). `menu_on_tick` declares it lost after `CENTRAL_SILENCE_MS`: logs "Central lost", clears the pop-up, cancels the reset wait and capture. Absent to present (including boot, since it starts absent) logs "Central present" and arms `CAPTURE_FIRST`. The initial capture value is now `CAPTURE_NONE`. The DIFF rule (set by `enter_menu`) and the reset timeout are unchanged. If presence is lost in the same tick as a CENTRAL silence exit, `enter_menu` arms DIFF afterwards, and the next absent to present transition overrides it with FIRST.
4. **Master mode (6.3):** not implemented. Noted in the design doc: a central silent on purpose in master mode would count as absent after 3 s, so the translator should hold presence on `on_mode` when master mode is built.

## Build
`.\scripts\build.ps1` once after all code edits, log in `.agents/tasks/macro-link-fix/build.log`:
- `Generated C:/GitHub/Prog-Wifi/build/prog_wifi.bin`
- `prog_wifi.bin binary size 0xedf90 bytes. Smallest app partition is 0x1e0000 bytes. 0xf2070 bytes (50%) free.`
- `Project build complete. To flash, run:`
- 0 `warning:` / `error:` lines. `build\prog_wifi.bin` exists (974736 bytes).
Not run: flash, monitor, hardware.

## Bench checklist
- Prog menu showing, real central connected: no `Link UP/DOWN` lines, no repeated `Pop-up stored` (one "Central present" and one "Pop-up stored" at start).
- `[ENTER CENTRAL]`: one `SEL_PANEL: Link UP` when replies start.
- Exit to the menu (3 s silence or key idle): one `Link DOWN`.
- Power the central off: after about 3 s "Central lost: pop-up cleared", macro entries show NO CENTRAL. Power on: "Central present", pop-up stored again.
- RECORD and REPLAY entries do not flicker between available and NO CENTRAL.
- Unverified: a central that pauses polling for over 500 ms while answered keeps its pop-up (presence timer only).
