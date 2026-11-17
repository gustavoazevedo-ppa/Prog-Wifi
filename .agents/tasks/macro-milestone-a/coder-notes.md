# Milestone A: coder notes

Nothing committed (per the brief). No review.json existed when this was written.

## Files changed

- `main/selector_macro.c/.h` (new, pure C): validator, pop-up normalizer, run-length encoder, script iterator, `selector_macro_selftest()`.
- `main/CMakeLists.txt`: `selector_macro.c` added to SRCS.
- `main/selector_port.c/.h`: reply gate (`selector_port_set_reply_gate`), new `SELECTOR_EVT_REPLY` event with `keys` and `counted` fields in `selector_evt_t`.
- `main/selector_bridge.c/.h`: `on_reply` translator callback, `selector_bridge_set_reply_gate()`, event routing, macro self-test at start, `BRIDGE_IN_DEPTH` 8 to 16.
- `main/selector_tr_menu.c`: rewritten state logic (see below). Not split.

## What was run

`.\scripts\build.ps1` once after all edits. Log: `.agents/tasks/macro-milestone-a/build.log`. Evidence lines:

- `[11/14] Linking CXX executable prog_wifi.elf`
- `Generated C:/GitHub/Prog-Wifi/build/prog_wifi.bin`
- `prog_wifi.bin binary size 0xedec0 bytes. Smallest app partition is 0x1e0000 bytes. 0xf2140 bytes (50%) free.`
- `Project build complete.`
- 0 `warning:` lines in the log. `build\prog_wifi.bin` exists (974528 bytes).

Not run: flash, monitor, any hardware test. The self-test runs on target at boot only, so it has been checked by tracing the vectors by hand, not executed.

## Behavior implemented

- Port: when the gate is closed, a valid central frame still updates stats, link and `last_screen`, and is sent to the bridge as `SELECTOR_EVT_MSG`, but no reply is sent and no reply event is made. When open, the reply goes out as before, then `SELECTOR_EVT_REPLY` (keys = the cached mask actually sent, counted = function `'S'` or `'?'`) is queued before the message event. Gate is a `volatile bool` (one byte, one writer, one reader), open by default.
- Menu: states MENU, CENTRAL, REPLAY (state only), STATUS. No automatic MENU to CENTRAL. Pages: project, `[ENTER CENTRAL]`, `[RECORD MACRO]`, `[REPLAY MACRO]`, then IP/MAC pages. PLUS picks an entry. `[ENTER CENTRAL]` opens the gate, masks the held PLUS (hand-back) and enters CENTRAL. RECORD/REPLAY only show reasons; if RECORD is available, PLUS logs "not implemented yet".
- Reasons (first match): BUSY, NO MACRO (replay), NO CENTRAL (no pop-up stored), WAIT CENTRAL (stored but not matched yet). ENTER CENTRAL has no reason, as in the design table.
- Exit rules: `CENTRAL_SILENCE_MS` 3000 (CENTRAL and REPLAY), `CENTRAL_KEY_IDLE_MS` 3000 (CENTRAL only, last key change from the real selector). Both go through `enter_menu()`.
- Pop-up: normalized with `selector_macro_normalize_popup()`. Capture while in MENU (gate closed): first screen after central link up, or after an exit the first screen that differs from the last one before the gate closed. Cleared on central link down and on reset timeout. Match sets ready.
- Reset procedure: starts on every `enter_menu()` and at boot (first tick), `POPUP_RESET_WAIT_MS` 5000. Cancelled when CENTRAL is entered.
- Kept: phase 1 pages, held-key hand-back, STATUS (a valid central frame in STATUS returns to CENTRAL as before, and `on_mode(false)` too), forwarding in CENTRAL, replies with idle keys when no selector is connected (the panel port is untouched in that respect).

## Decisions

- **Open point (menu vs scratchpad and run state):** BUSY calls `preset_server_run_active()` directly (it takes a mutex for a struct copy only, no waiting on I/O). NO MACRO uses a local `macro_in_scratchpad()` stub that returns false, marked for replacement in milestone B. No preset_server change.
- Gate as a shared flag, not a queue message: avoids adding a non-wire type to `selector_msg_t`, and takes effect before the next central frame. Deviation from the "everything through queues" sentence in design section 8, limited to this one byte.
- `on_reply` is appended at the end of `selector_translator_t`, so existing designated initializers keep working. The menu sets it to NULL for now.
- Entry pages refresh every `ENTRY_POLL_MS` (100) so a reason like WAIT CENTRAL clears quickly. Other pages keep the 500 ms poll.
- Unverified rules (F1, F2, F8, D2) are marked in code comments.

## Ambiguities and possible design issues found

1. **Backlight in MENU.** With the gate closed the central keeps talking, so its `'L'` frames arrive while the Prog menu shows. Dropping them would lose the central's backlight (it is probably not repeated), so the menu forwards BACKLIGHT messages and drops SCREEN and BEEP. The design does not say this. Easy to change if the owner prefers.
2. **Boot window.** The gate is open from port start until the bridge task's first tick (milliseconds), so a central already talking at that moment can get a reply or two. Closing it earlier would need a config flag on the port. Left as is.
3. **Capture after a link flap inside CENTRAL.** Link down clears the pop-up; link up sets capture FIRST, but while in CENTRAL the start screen is forwarded, not captured. On the next exit `enter_menu()` sets capture DIFF, which only stores a screen that differs from the last one sent, so a central that just sits on one screen would stay NO CENTRAL until another link flap. This follows the rule as written (F2). Worth checking on the bench.
4. **Key idle exit with no real selector.** CENTRAL ends after 3 s without a key edge even if the selector is unplugged. This is what 7c says, but decision D5 (reply with idle keys with no selector) then only holds for 3 s after ENTER CENTRAL. A central can only be entered with the selector present anyway.
5. **Reply event queue pressure.** Each central frame now queues two events (reply and message), so `BRIDGE_IN_DEPTH` went to 16. If the central polls faster than expected, `bridge_queue_full`/port `queue_full` counters will show it.
6. **Frames dropped for framing errors during replay** (design: a replay failure reason) have no event yet. That belongs to milestone B (engine), noted so the port contract gets a drop event then.
7. **REPLAY state** is unreachable in this milestone. Its silence exit is shared with CENTRAL so it cannot hang once an engine enters it.
8. **Validator details not spelled out in 7c:** a header with no body is accepted; a BOM and blanks before `#` are accepted; `3P` or `PS` (no blank) are rejected as bad characters; the 2048 limit is checked first (the self-test checks the 2049 rejection with a 4-byte buffer and a lying length, and does not allow-test exactly 2048).
9. The encoder splits runs over 65535 into several numbers; a single run needing more than the 48-byte scratch is treated as a full buffer.

## Review round 2 fixes (review.json CHANGES_REQUESTED)
1. Startup reply leak: new `selector_port_cfg_t.reply_gate_closed` (default false = open) and `selector_translator_t.gate_closed_at_start`. The bridge passes the selected translator's flag to the panel port config, so the port starts with the gate closed before its task runs. The menu translator sets it true; identity keeps it open. This replaces ambiguity 2 above (the boot window is closed now). Files: selector_port.c/.h, selector_bridge.c/.h, selector_tr_menu.c.
2. Stale capture after reset timeout: the timeout branch now sets `capture = CAPTURE_NONE` before `popup_clear()`. A new capture is armed again only by a central link up (FIRST) or the next `enter_menu()` (DIFF).
Rebuilt with .\scripts\build.ps1 (log overwritten in build.log): 0 warning/error lines, `Generated C:/GitHub/Prog-Wifi/build/prog_wifi.bin`, `prog_wifi.bin binary size 0xedf00 bytes ... (50%) free`, `Project build complete.`; build\prog_wifi.bin is 974592 bytes. Nothing committed or flashed.
