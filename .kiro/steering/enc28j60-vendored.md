# Vendored & patched ENC28J60 driver

The ENC28J60 Ethernet driver is **vendored** at `components/enc28j60/` and carries
**local patches**. It is intentionally NOT a managed dependency.

## Why it's here and not in managed_components/

`components/*` is owned by us and is never touched by the IDF Component Manager.
The driver was moved out of `managed_components/espressif__enc28j60/` so our local
fixes can't be silently overwritten when dependencies are refreshed.

Do **not** re-add `espressif/enc28j60` to `main/idf_component.yml`. If you do, the
Component Manager will download an unpatched copy into `managed_components/` and
you'll have two copies / lose the fixes.

Because it's a plain local component, `main` must declare it explicitly:
`main/CMakeLists.txt` lists `enc28j60` in `PRIV_REQUIRES`. Keep that.

## Base version

Vendored from upstream `espressif/esp-eth-drivers`, component `enc28j60`, version
**1.1.0** (upstream commit `968c6188e9d2ea554348af8987492d173705bb93`).

## Local patches (all in `components/enc28j60/src/esp_eth_mac_enc28j60.c`)

Reviewable diffs live in `patches/` at the repo root:

1. `patches/0001-enc28j60-rx-bounds-checks.patch` — `emac_enc28j60_receive()`:
   reject an RX status vector whose `rx_len` exceeds the caller buffer or whose
   `next_packet_addr` is out of range (prevents heap overflow); discard via a
   `pktdec:` label so the packet counter still advances.
2. `patches/0002-enc28j60-rx-nomem-backpressure.patch` — `emac_enc28j60_task()`:
   on DMA alloc failure, delay 5ms and break instead of busy-spinning; `taskYIELD()`
   each iteration; NULL the buffer after ownership transfer/free (WDT-starvation fix).
3. `patches/0003-enc28j60-tx-statemachine-recovery.patch` — `emac_enc28j60_transmit()`:
   when `ECON1.TXRTS` is still set after the tx_ready_sem wait, reset the TX state
   machine (pulse `ECON1.TXRST`) and clear stale TX flags instead of aborting
   (recovers from the permanent TX hang, ENC28J60 errata #12/#13).

Runtime signature of patch 3 firing: log `TX stuck (TXRTS set); resetting TX state
machine` followed by traffic resuming (instead of endless `tx_ready_sem expired` /
`last transmit still in progress`).

## Upstreaming

PR drafts (titles, commit messages, bodies, CHANGELOG entries) are in
`patches/PR_DRAFTS.md`. Target repo: `espressif/esp-eth-drivers`. If/when a patch is
merged and released, drop the corresponding local change and, once all are upstream,
consider returning to a managed dependency at the new version.

## Taking a newer upstream version later

1. Fetch the new `enc28j60/` source from `esp-eth-drivers` at the desired tag.
2. Replace `components/enc28j60/` contents with it.
3. Re-apply any patches in `patches/` that haven't been merged upstream yet (they are
   readable diffs; apply by hand or adapt — line numbers may have moved).
4. `.\scripts\build.ps1 fullclean` then `.\scripts\build.ps1` to verify
   (`Project build complete.` + `build\modulo_wifi.bin`).
