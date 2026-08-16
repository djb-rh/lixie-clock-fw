# Phase 0 — Feasibility results

Measured on `shop-clock` (Photon 1, Device OS 2.3.1), 2026-08-16.

## Gate: PASSED, with far more room than expected

| | Gate | Measured | Headroom |
|---|---|---|---|
| Flash | ≤ 109 KB of 128 KB | **39,220 bytes (30%)** | 70% |
| Free heap | ≥ 12 KB | **~40,200 bytes** | 3.3× the gate |
| Static RAM | — | 2,604 bytes | — |

The plan estimated 85–95 KB of flash. The real figure is less than half that: LTO plus
`--specs=nano.specs` (newlib-nano) collapses the float/trig and `snprintf` costs that
dominated the estimate.

**Consequence:** the austerity measures the plan budgeted for are no longer forced.
ArduinoJson, mDNS, and a substantially richer config page are all affordable. The
no-`String`-in-hot-paths rule stays — that is about heap *fragmentation* over months of
uptime, not about total free bytes, and 40 KB of free heap does not fix fragmentation.

## What the spike proved on real hardware

- **HTTP server + gzip** — the served page decompresses byte-identical to
  `web/index.html`. Streaming the blob in 512-byte chunks works.
- **Hand-rolled UDP NTP client** — synced against `pool.ntp.org` within ~30 s of boot
  (`ntp_age: 0`). Dropping the `ntp-time` library costs nothing and buys the staleness
  tracking the status page and HA diagnostic sensor need.
- **EEPROM config + CRC16** — bad CRC from the old firmware's EEPROM contents correctly
  fell through to defaults.
- **`retained` backup SRAM** — `resetReason` read back 70 (`RESET_REASON_UPDATE`) after
  the OTA, so the reboot-cause diagnostic works.
- **NeoPixel at 120 LEDs** — links and runs, sized for the 6-digit worst case.
- **OTA to a developer device** — `particle flash shop-clock` works unattended.

## Bugs and gotchas found by running it

1. **`Time.day()` is day-of-month, not day-of-year.** Passing it to the solar function
   silently produced *January* sunrise times that looked entirely plausible (12:25 UTC).
   Fixed with an explicit `dayOfYear()` helper. This is exactly the class of bug the live
   sunrise/sunset preview in the web UI is meant to catch.
2. **Sunset can exceed 1440 minutes UTC**, rolling into the next UTC day (Durham's
   2026-08-16 sunset is 1445 = 00:05 UTC on the 17th). Local-time conversion must handle
   the rollover rather than assume a 0–1439 range.
3. **Post-OTA readiness checks must be version-specific.** Polling until `/api/state`
   answers is useless — the *old* firmware answers it too, so the first verification read
   stale values from firmware that was about to be replaced. Poll for a field that only
   the new build emits.
4. **Solar accuracy is ~3–4 minutes** against almanac values with the simplified NOAA
   formula (computed 06:33/20:05 EDT vs actual 06:37/20:04 for Durham). The plan's
   verification bar is ≤2 minutes, so Phase 4 likely needs the fuller NOAA solar-position
   terms rather than the low-precision approximation used here.

   > **Corrected in Phase 4.** The "actual" figures above were recalled, not looked up,
   > which made this claim unreliable in both directions. Measured against `astral`
   > across 69 sample days and 9 locations, the simplified formula is worst-case 347 s
   > (5.8 min) and mean 76 s — worse than stated at high latitude, better than stated at
   > Durham. The full NOAA implementation now in `src/solar.cpp` is worst-case 72 s, mean
   > 20 s. See docs/phase4-results.md.
5. **`shop-clock` sits at −71 dBm RSSI.** Workable, but the weaker end of usable — worth
   watching as a suspect if that clock ever shows connectivity trouble.

## Open items — all resolved

| Item | Resolution |
|---|---|
| MQTT broker in Home Assistant | Confirmed: Mosquitto is running. Phase 5 unblocked. |
| LED wiring on both clocks | Confirmed: `D0`, 80 LEDs, 4 digits, both clocks. |
| `Clock2` development-device status | Reachable and online via the Product; verify a direct OTA before deploying to it. |
| ArduinoJson vs hand-rolled JSON | Headroom makes ArduinoJson affordable; the decision is now preference, not necessity. |

## Note on the spike

`src/lixie-clock-fw.ino` is currently the throwaway spike, not the real firmware, and is
what `shop-clock` is running. The old firmware can be restored at any time from the
`photon_firmware_*.bin` files in the original repo.
