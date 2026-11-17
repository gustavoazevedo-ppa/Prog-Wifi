# Explicit central hand-off and macro-format foundations

Milestone A replaces automatic central hand-off with `[ENTER CENTRAL]`, adds the panel reply gate and per-reply event contract, and introduces pure-C macro format utilities. The timers, reset procedure, pop-up tracking, and held-key hand-back follow sections 7b and 7c while preserving section 6.1 link accounting. Recording, replay execution, scratchpad integration, `/iniciar`, and `preset_server.c` changes are absent as required. The previous pass's startup reply leak and stale post-timeout capture are fixed.

Watch for: **confirmed** F1, F2, F8, and D2 remain hardware/tuning questions by design; the code marks these assumptions as unverified.

**Verdict**: APPROVED

## High-level view

MENU owns the selector from boot, with the reply gate closed before the panel task starts. `[ENTER CENTRAL]` opens the gate and masks its selecting PLUS until release. Central silence and real-selector key inactivity use separate 3000 ms clocks and return through the MENU reset path.

Every MENU arrival closes the gate and waits up to 5000 ms for the normalized stored pop-up. A match restores readiness; central link loss and timeout clear the pop-up. Timeout also disarms capture, preventing a later unrelated screen from restoring readiness.

Each open-gate reply produces an event with the key mask sent to the central. `S` and `?` replies are counted; `B` and `L` remain uncounted pending F3/S7. Link supervision and downstream delivery continue with the gate closed.

The macro module implements the specified validator, pop-up normalizer, run-length encoder, and iterator. Fixed vectors exercise round trips, chords, long runs, buffer exhaustion, normalization, malformed input, BOM handling, and the size guard.

RECORD and REPLAY expose availability only. REPLAY remains unavailable through a local scratchpad stub, while an otherwise available RECORD selection only logs that execution is deferred.

<details>
<summary>Issues (0)</summary>

No actionable issues found.

</details>

<details>
<summary>Details</summary>

## MENU ownership and transparent-mode exits

The menu's `gate_closed_at_start` policy reaches `selector_port_start()` before `panel_task` is created, removing the previous interval in which the central could receive an early reply. The identity translator leaves the new field false, preserving open-by-default behavior outside the menu translator.

The PLUS edge updates `user_keys` before `enter_central()` snapshots `handback_mask`, so the selecting key stays absent from central replies until release. Entry initializes both exit clocks. Valid decoded central messages refresh the silence clock, and real-selector key-state changes refresh the key-idle clock. Both exits call `enter_menu()`, which returns to page zero, closes the gate, idles the panel key cache, and starts reset.

REPLAY remains unreachable scaffolding and omits the key-idle exit as designed. STATUS keeps its planned master-mode transition back to CENTRAL.

## Pop-up capture and reset readiness

Normalization trims both LCD rows, drops blank rows, joins retained rows with `/`, and substitutes non-printable bytes. Storage and comparison use this same representation. Initial capture uses the first screen after central link-up; later exits preserve the stored pop-up and require a normalized match after gate closure.

Link-down clears validity and readiness. Reset timeout clears the pop-up and sets capture to `CAPTURE_NONE`; only a later link-up or `enter_menu()` arms capture again. This closes the stale-capture path found in the previous review.

The first-screen/different-screen choice, 5000 ms reset deadline, and 3000 ms exit values remain the explicitly unverified F1/F2/F8/D2 bench questions.

## Reply-gate and reply-event contract

For an open gate, the panel drains the upstream key cache before queuing its UART reply, then emits one event containing that exact mask. `counted` is true only for `SELECTOR_FUNC_SCREEN` and `SELECTOR_FUNC_POLL`, matching section 7c and the unresolved F3/S7 treatment of beep and backlight.

A closed gate suppresses both the UART reply and reply event. The decoded central message still increments frame statistics, refreshes link supervision, drives link changes, and enters the bridge queue. The bridge exposes reply events through an optional translator callback, which remains null for Milestone A.

## Macro utilities and self-tests

Validation enforces the 2048-byte ceiling before reading input, accepts the optional BOM and leading blanks, requires a `#` header, and rejects unknown body tokens and decimal counts outside 1 through 65535. Iteration starts released, applies key tokens to the next reply, supports chords, and adds adjacent numeric runs under the same mask.

Encoding emits changed keys in `M P O S` order, splits counts above 65535 into additive tokens, preserves initial and trailing waits, and appends releases for held keys. Buffer exhaustion retains a syntactically valid prefix ending at a complete group.

`selector_macro_selftest()` runs during bridge startup beside the wire self-test. Its vectors cover waits and PLUS, a chord, a 70000-reply split, iterator output, full-buffer preservation, header capacity, normalization, malformed tokens, number bounds, BOM input, and the 2049-byte rejection. This review did not rerun the build; coder notes record a successful post-fix build and no hardware or on-target self-test run.

## Milestone boundary

The only preset-server dependency is the existing `preset_server_run_active()` query for `BUSY`. `macro_in_scratchpad()` returns false, RECORD/REPLAY selection performs no transition or data mutation, and the menu's reply callback is null. No changed file adds preset storage/fetch, web, NVS, recording, replay, or status behavior.

</details>

<details>
<summary>File map</summary>

- `main/CMakeLists.txt` — adds the macro utility module.
- `main/selector_macro.c` — implements normalization, validation, encoding, iteration, and self-tests.
- `main/selector_macro.h` — defines macro limits and the utility API.
- `main/selector_port.c` — adds startup-aware reply gating and reply events.
- `main/selector_port.h` — extends the port event and configuration contracts.
- `main/selector_bridge.c` — routes reply events, controls the gate, enlarges the inbound queue, and runs the macro self-test.
- `main/selector_bridge.h` — extends the translator contract.
- `main/selector_tr_menu.c` — adds explicit entry, placeholder macro pages, dual exits, pop-up storage, and reset readiness.

Full review input: `git diff HEAD -- main`, plus untracked `main/selector_macro.c` and `main/selector_macro.h`.

</details>
