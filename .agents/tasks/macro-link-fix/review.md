# Review: macro link fix

Verdict: APPROVED. No blockers.

Evidence checked (build.log results taken from report.md, not re-run: build complete, 0 warnings, bin 50% free):

- Panel link (selector_port.c): Link UP only inside panel_handle_frame when `replied` (gate open). Link DOWN only from panel_task, when `!reply_gate_open` or 500 ms since last valid frame. Gate closed: `replied` false, no `port_set_link`, no log line. Host port code is unchanged.
- Threading: `reply_gate_open` is a single-byte volatile, one writer (bridge task), one reader (port task). Link events come only from the port task, so none is lost. Worst case is a frame handled with `replied` read just before the gate closes. It sends one reply and an UP, and the next loop (at most 10 ms) emits the DOWN. This is a harmless pair and not a regression. A close and re-open within one 10 ms loop emits no DOWN, which needs a key press in between and is not reachable in practice. Nothing blocks.
- Translator (selector_tr_menu.c): `menu_on_link` no longer clears or arms anything. Presence is set from every valid downstream frame in every state, using its own `last_central_ms`, and is lost in `menu_on_tick` after 3000 ms. Only presence loss and the reset-wait timeout clear the pop-up. A central that pauses more than 500 ms while answered keeps its pop-up.
- Capture: absent to present arms CAPTURE_FIRST (boot starts absent). `enter_menu` arms DIFF, and the ref is unchanged. Presence loss in CENTRAL: the tick clears the pop-up, then the exit rule runs `enter_menu` (DIFF, reset begins). The next frame arms FIRST and stores the pop-up. Gate re-open after a close is fine.
- Milestone A: gate closed at start, hand-back, forwarding, exit rules, reset wait and STATUS are untouched.
- Docs: 6.1, 7b, 7c, F1 and F2 are edited as required, only the needed lines, ASCII (no non-ASCII found), and the unverified marks are kept. Master mode is only noted.
- Scope: no touches to recording, replay, preset_server, web, NVS, partitions, host-side port, secrets, managed_components, enc28j60 or sdkconfig. `.kiro/steering/structure.md` shows as modified, but that comes from the earlier Milestone A work and not this fix.
- Conventions: Allman braces, TAG and ESP_LOGx, module prefixes are respected.

Minor note, not a blocker: during the few ms at boot before the first tick the gate is open. A central already talking can produce one UP/DOWN pair. It exists in Milestone A and does not recur.
