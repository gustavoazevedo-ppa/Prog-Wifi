# Review A (round 2): Milestone A (macro foundations)
Verdict: APPROVED. No blockers.

## Evidence
- Build: coder-notes record the rebuild after the round 2 fixes: `Project build complete.`, bin 0xedf00 bytes, 0 warnings. Not rerun.
- Scope: changes under main are CMakeLists.txt (SRCS), selector_bridge.c/.h, selector_port.c/.h, selector_tr_menu.c and new selector_macro.c/.h. No preset_server, NVS, partition, sdkconfig or index.html change. No recording and no replay engine: `macro_in_scratchpad()` returns false, RECORD/REPLAY PLUS only logs, REPLAY state is unreachable, `on_reply` is NULL in the menu.
- Prior blockers fixed:
  1. Startup leak: `selector_translator_t.gate_closed_at_start` (menu = true) is passed through `selector_port_cfg_t.reply_gate_closed` and applied in `selector_port_start()` (`reply_gate_open = !cfg->reply_gate_closed`) before the task runs. `s_tr` is set before the cfg is built in `selector_bridge_start()`. Identity keeps the gate open.
  2. Stale capture: the reset-timeout branch sets `capture = CAPTURE_NONE` before `popup_clear()`. It is re-armed only by link up (FIRST) or `enter_menu()` (DIFF).
- Self-test: `selector_macro_selftest()` has round trips (waits and PLUS, chord order `M P`, run over 65535 split as `65535 4465`), full-buffer, header-does-not-fit, pop-up normalization (including control chars) and 13 accept/reject vectors plus the 2049-byte limit. I traced the encoder output for the first three by hand and they match the expected strings and iterator output. It is called in `selector_bridge_start()` next to the wire self-test and a failure returns false.
- Design 7c / 6.1 / F: page order and line 2 texts, reason order BUSY, NO MACRO, NO CENTRAL, WAIT CENTRAL, ENTER CENTRAL without reason, gate closed in MENU and from first frame, opened only by [ENTER CENTRAL], reset procedure on every `enter_menu()` with 5000 ms, pop-up cleared on link down and timeout, both 3000 ms exits through `enter_menu()`, reply event carries the sent mask and `counted` for 'S'/'?'. Link accounting (`last_valid_ms`, `frames_ok`, link events) sits after the gated block, so the link stays up with the gate closed.
- Phase 1 behavior: SEL/OK paging with wrap, MINUS ignored, `handback_mask` taken from `user_keys` (already updated) before the PLUS pick, so the picking PLUS stays masked until released. Idle `KEYS` on `enter_menu()` reach the panel cache (the port also drains `from_bridge` in its idle path, not only on replies). STATUS and `on_mode(false)` paths still go to CENTRAL via `enter_central()`. CENTRAL forwards everything unchanged.
- FreeRTOS: no blocking in the bridge task. `port_send_evt` is a 0-tick send. The gate is one volatile byte, one writer, one reader. `BRIDGE_IN_DEPTH` 16 covers the two events per central frame. `preset_server_run_active()` takes `s_lock` only around a struct copy, and `s_lock` is created before the bridge starts in the boot order.
- Conventions: Allman, 4-space, TAG, module prefixes, include guard `SELECTOR_MACRO_H`, `selector_macro` has no ESP-IDF dependency.

## Notes (not blockers)
- D5: with the gate closed in MENU (design row 25), a central gets no replies until a user picks [ENTER CENTRAL], so "test a central without a selector" (D5, 6.1) only holds once the gate is open. This follows the approved design. Worth a line in the docs.
- The menu forwards BACKLIGHT in MENU (coder item 1); small and reasonable.
- The `preset_server_run_active()` call would assert if `preset_server_start()` failed on mutex creation (`s_lock` NULL). Only under heap exhaustion at boot, and the design mandates the call. Not filed.
