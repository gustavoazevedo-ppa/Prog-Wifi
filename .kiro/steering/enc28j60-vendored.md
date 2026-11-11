---
inclusion: fileMatch
fileMatchPattern: ['components/enc28j60/**', 'patches/**', 'main/idf_component.yml', 'main/CMakeLists.txt', 'dependencies.lock', 'main/ethernet_manager.*']
---

# Vendored & patched ENC28J60 driver

The ENC28J60 Ethernet driver is **vendored** at `components/enc28j60/` and carries **local patches**. It is intentionally NOT a managed dependency.

## Rules

- Edit the driver only in `components/enc28j60/`. Never edit or recreate `managed_components/espressif__enc28j60/`.
- Do **not** add `espressif/enc28j60` to `main/idf_component.yml`. The Component Manager would download an unpatched copy into `managed_components/`, giving two copies and losing the fixes.
- Keep `enc28j60` in `PRIV_REQUIRES` in `main/CMakeLists.txt`. Local components under `components/*` are not auto-resolved from the manifest, so `main` must declare it.
- Never overwrite, revert, or "clean up" the local patches below when refactoring or refreshing dependencies.
- Any new driver fix must also be saved as a numbered diff in `patches/` (`NNNN-enc28j60-<topic>.patch`) and documented in this file and in `patches/PR_DRAFTS.md`.
- Build only via `.\scripts\build.ps1` (see the build steering).

## Why it's vendored

`components/*` is owned by us and is never touched by the IDF Component Manager, so local fixes can't be silently overwritten when dependencies are refreshed.

## Base version

Upstream `espressif/esp-eth-drivers`, component `enc28j60`, version **1.1.0** (commit `968c6188e9d2ea554348af8987492d173705bb93`).

## Local patches

All changes are in `components/enc28j60/src/esp_eth_mac_enc28j60.c`. Reviewable diffs are in `patches/` at the repo root.

1. `patches/0001-enc28j60-rx-bounds-checks.patch`, in `emac_enc28j60_receive()`: reject an RX status vector whose `rx_len` exceeds the caller buffer or whose `next_packet_addr` is out of range (prevents heap overflow). Discard via a `pktdec:` label so the packet counter still advances.
2. `patches/0002-enc28j60-rx-nomem-backpressure.patch`, in `emac_enc28j60_task()`: on DMA alloc failure, delay 5 ms and break instead of busy-spinning. Call `taskYIELD()` each iteration. NULL the buffer after ownership transfer or free (WDT-starvation fix).
3. `patches/0003-enc28j60-tx-statemachine-recovery.patch`, in `emac_enc28j60_transmit()`: when `ECON1.TXRTS` is still set after the `tx_ready_sem` wait, reset the TX state machine (pulse `ECON1.TXRST`) and clear stale TX flags instead of aborting. This recovers from the permanent TX hang (ENC28J60 errata #12/#13).

Runtime signature of patch 3 firing: log `TX stuck (TXRTS set); resetting TX state machine`, followed by traffic resuming. Without the patch you see endless `tx_ready_sem expired` / `last transmit still in progress`.

## Upstreaming

PR drafts (titles, commit messages, bodies, CHANGELOG entries) are in `patches/PR_DRAFTS.md`. Target repo: `espressif/esp-eth-drivers`. When a patch is merged and released, drop the corresponding local change. Once all are upstream, consider returning to a managed dependency at the new version.

## Taking a newer upstream version

1. Fetch the new `enc28j60/` source from `esp-eth-drivers` at the desired tag.
2. Replace the contents of `components/enc28j60/` with it.
3. Re-apply every patch in `patches/` not yet merged upstream. They are readable diffs; apply by hand or adapt, since line numbers may have moved.
4. Update the base version in this file.
5. Run `.\scripts\build.ps1 fullclean`, then `.\scripts\build.ps1`. Success is `Project build complete.` plus `build\modulo_wifi.bin`.
