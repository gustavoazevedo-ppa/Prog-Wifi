# Local patches to the vendored ENC28J60 driver

These patches are already applied to the vendored copy at
`components/enc28j60/src/esp_eth_mac_enc28j60.c`. They are kept here as reviewable
diffs for two reasons: to re-apply after taking a newer upstream version, and as the
basis for upstream pull requests to `espressif/esp-eth-drivers`.

See `.kiro/steering/enc28j60-vendored.md` for the full context (why the component is
vendored, base version, and the upgrade procedure).

## Files

| File | Concern | Function | Upstream PR priority |
|------|---------|----------|----------------------|
| `0001-enc28j60-rx-bounds-checks.patch` | Reject corrupt RX status vector (heap-overflow guard) | `emac_enc28j60_receive()` | High (memory safety) |
| `0002-enc28j60-rx-nomem-backpressure.patch` | Back off instead of busy-spinning on DMA alloc failure (WDT) | `emac_enc28j60_task()` | Medium |
| `0003-enc28j60-tx-statemachine-recovery.patch` | Recover from stuck TX state machine (errata #12/#13) | `emac_enc28j60_transmit()` | High value, more scrutiny |

`PR_DRAFTS.md` — ready-to-paste PR titles, commit messages, bodies, and CHANGELOG
entries for submitting these upstream.

## Note on format

These are human-readable unified diffs with context markers, not strict
line-numbered `git apply` hunks. They document intent and are meant to be applied by
hand or adapted when line numbers shift in a newer upstream. The authoritative,
compiled version of each change is the vendored source itself.
