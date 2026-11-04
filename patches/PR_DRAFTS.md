# Upstream PR drafts — espressif/esp-eth-drivers (enc28j60)

Target repo: https://github.com/espressif/esp-eth-drivers
Component path in repo: `enc28j60/`
Base version at time of writing: 1.1.0 (upstream commit `968c6188e9d2ea554348af8987492d173705bb93`)

Submit as **three independent PRs** (one per concern) so the low-risk memory-safety
fix can merge without waiting on the more debatable ones. Each maps 1:1 to a patch
file in this directory.

Prerequisites before opening PRs:
- Fork `espressif/esp-eth-drivers`, branch off `master`.
- Sign the Espressif Contributor License Agreement (CLA) — required for external merges.
- Run the repo's formatter / pre-commit hooks (astyle) so CI formatting passes.
- Commit messages use Conventional Commits; the per-component CHANGELOG and version
  bump are generated from them.

---

## PR 1 — RX status-vector bounds checks  (patch 0001)

**Priority:** highest. Pure memory-safety hardening, no happy-path behavior change.

**Branch:** `fix/enc28j60-rx-bounds`

**Commit message:**
```
fix(enc28j60): validate RX status vector before reading packet

The receive path trusts the length and next-packet address from the chip's
Receive Status Vector without validation. A corrupt RSV (SPI glitch or
buffer-pointer desync) can report an rx_len larger than the caller's buffer,
causing a heap overflow in enc28j60_read_packet(). Add guards for rx_len and
next_packet_addr; discard the bad packet but still advance the packet counter.
```

**PR body:**
> ### Problem
> `emac_enc28j60_receive()` reads `rx_len` and `next_packet_addr` from the Receive
> Status Vector and immediately reads `rx_len` bytes into the caller-supplied buffer.
> Nothing validates that `rx_len` fits the buffer or that `next_packet_addr` is within
> the RX region. A corrupt RSV — which can happen on a marginal SPI link or a chip
> buffer-pointer desync — leads to an out-of-bounds write (heap overflow) and/or a
> desynced RX ring.
>
> ### Fix
> Two guards at the top of the receive routine:
> - `rx_len > *length` → log, discard, advance to `pktdec`.
> - `next_packet_addr > ENC28J60_BUF_RX_END` → log, discard, advance to `pktdec`.
>
> A new `pktdec:` label ensures the packet counter is still decremented so the RX
> pipeline does not stall on a discarded packet.
>
> ### Testing
> Built and run on ESP32 + ENC28J60 (SPI @ 20 MHz). No happy-path change observed;
> malformed frames are now dropped instead of corrupting the heap.

**CHANGELOG entry (under a new version, e.g. 1.1.1):**
```
### Bug Fixes
* **enc28j60:** validate RX status vector length/address to prevent heap overflow
```

---

## PR 2 — RX no-mem backpressure / WDT starvation  (patch 0002)

**Priority:** medium. Reliability fix; maintainers may want to discuss the strategy.

**Branch:** `fix/enc28j60-rx-nomem-backpressure`

**Commit message:**
```
fix(enc28j60): avoid busy-spin and WDT starvation under RX pressure

On DMA heap exhaustion the RX drain loop kept retrying heap_caps_malloc()
immediately, which can starve the task watchdog and never yields for lwIP to
free buffers. Delay briefly and break out on allocation failure, yield each
iteration under sustained floods, and NULL the buffer after ownership transfer.
```

**PR body:**
> ### Problem
> In `emac_enc28j60_task()`, when `heap_caps_malloc(..., MALLOC_CAP_DMA)` returns
> NULL the code logs "no mem for receive buffer" but stays in the
> `do { ... } while (emac->packets_remain)` loop, retrying immediately. Under
> sustained RX pressure with an exhausted DMA heap this is a tight busy-loop that can
> trip the task watchdog and gives lwIP no chance to free buffers.
>
> ### Fix
> - On alloc failure: `vTaskDelay(5ms)` then `break` out of the drain loop. The
>   interrupt re-triggers once buffers are available.
> - `taskYIELD()` at the end of each loop iteration to avoid WDT starvation during
>   sustained packet floods.
> - Set `buffer = NULL` after `stack_input()` (ownership transfer) and after each
>   `free()` to make double-free protection explicit.
>
> ### Testing
> Built and run on ESP32 + ENC28J60. Under induced memory pressure the previous
> version produced watchdog-adjacent stalls and a flood of "no mem" logs; with this
> change RX backs off and recovers.
>
> ### Open question for maintainers
> The 5 ms delay and break-vs-retry choice are heuristic. Happy to adjust to a
> preferred backpressure strategy.

**CHANGELOG entry:**
```
### Bug Fixes
* **enc28j60:** back off instead of busy-spinning when RX DMA buffer allocation fails
```

---

## PR 3 — TX state-machine recovery (errata #12/#13)  (patch 0003)

**Priority:** highest value to affected users, most scrutiny expected.

**Branch:** `fix/enc28j60-tx-recovery`

**Commit message:**
```
fix(enc28j60): recover from stuck TX state machine (errata #12/#13)

When the TX state machine latches up, TXRTS stays set and EIR_TXIF never fires,
so tx_ready_sem is never given and every subsequent transmit fails the TXRTS
guard permanently. Reset the TX logic in-line when TXRTS is still set after the
wait, clear stale TX flags, and continue, turning a permanent hang into a
self-healing event.
```

**PR body:**
> ### Problem
> The TX-done handshake depends on the `EIR_TXIF` interrupt giving `tx_ready_sem`.
> If the TX state machine latches up (ENC28J60 errata #12/#13), `ECON1.TXRTS` stays
> set and the TX-done interrupt never fires. The existing `EIR_TXERIF` recovery lives
> in the RX/IRQ task and only runs on an interrupt that is never delivered in this
> state. `emac_enc28j60_transmit()` then times out on `tx_ready_sem`, fails the
> `!(econ1 & ECON1_TXRTS)` check, and returns `ESP_ERR_INVALID_STATE` for every
> packet thereafter. TX wedges permanently while the link still appears up.
>
> Observed on ESP32 + ENC28J60 (SPI @ 20 MHz) as a permanent failure after hours of
> uptime — repeating `tx_ready_sem expired` / `last transmit still in progress`, with
> UDP, the app protocol, and MQTT all collapsing because nothing could be sent:
> ```
> W enc28j60: tx_ready_sem expired
> E enc28j60: emac_enc28j60_transmit(1011): last transmit still in progress
> ... (repeats indefinitely, never recovers)
> ```
>
> ### Fix
> When `TXRTS` is still set after the semaphore wait, reset the TX logic in-line
> (pulse `ECON1.TXRST`), clear the stale `EIR_TXERIF | EIR_TXIF` flags, and continue
> to transmit the current packet — which re-arms the TX interrupt and re-asserts
> `TXRTS`, restoring the handshake. A permanent hang becomes a logged, self-healing
> event.
>
> ### Testing
> Built and run on ESP32 + ENC28J60. After the change, the wedge condition is
> replaced by a single `TX stuck (TXRTS set); resetting TX state machine` warning
> followed by traffic resuming.
>
> ### Note for maintainers
> This may co-occur with marginal power/SPI on some modules; the change is a software
> safety net that keeps the link alive regardless. Open to gating it or moving the
> recovery point if preferred.

**CHANGELOG entry:**
```
### Bug Fixes
* **enc28j60:** reset TX state machine when it latches up so TX self-recovers (errata #12/#13)
```

---

## Suggested git/gh commands (run against your fork)

```powershell
# after forking on github and cloning your fork:
git remote add upstream https://github.com/espressif/esp-eth-drivers.git
git fetch upstream

# PR 1
git checkout -b fix/enc28j60-rx-bounds upstream/master
#   apply the change from patches/0001-*.patch to enc28j60/src/esp_eth_mac_enc28j60.c
git add enc28j60/src/esp_eth_mac_enc28j60.c
git commit   # paste the PR 1 commit message
git push -u origin fix/enc28j60-rx-bounds
gh pr create --repo espressif/esp-eth-drivers --base master --fill

# repeat for fix/enc28j60-rx-nomem-backpressure (0002) and fix/enc28j60-tx-recovery (0003)
```
