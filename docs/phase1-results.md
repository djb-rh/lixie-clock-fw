# Phase 1 — Core

`shop-clock` is telling correct local time on the new firmware.

```
Flash  21,460 / 131,072   Static RAM  3,044   Free heap  ~42,100
```

## What landed

| Module | Responsibility |
|---|---|
| `src/tz.{h,cpp}` | Calendar math and POSIX TZ parsing. **No Particle dependency** — tested on the host. |
| `src/config.{h,cpp}` | Packed EEPROM struct, CRC, validation, defaults. Header is Particle-free. |
| `src/display.{h,cpp}` | Digit→LED mapping, frame buffer, brightness, clock layout. |
| `src/timekeep.{h,cpp}` | UDP NTP client, sync scheduling, staleness tracking. |
| `src/netwatch.{h,cpp}` | Wi-Fi/cloud supervision ladder, retained diagnostics. |
| `src/main.cpp` | Orchestration and the temporary Particle-function config surface. |

`main.cpp` is deliberately **not** a `.ino`: Particle's preprocessor injects generated
prototypes outside any namespace, which collides with anonymous-namespace statics. A
plain `.cpp` compiles as written.

## Timezones without a database

The device stores a ~48-byte POSIX TZ rule (`EST5EDT,M3.2.0,M11.1.0/2`) and interprets
it itself. Particle's `Time` stays UTC; `Time.zone()` is never called. Supports `Mm.w.d`,
`Jn` and `n` rules, quoted `<-03>` zone names, half-hour offsets, and implicit DST
offsets.

Verified **on hardware**, not just in tests:

| | Result |
|---|---|
| Spring forward | `01:59:00 EST` → `03:00:00 EDT` (02:00 correctly skipped) |
| Fall back | `01:59:00 EDT` → `01:00:00 EST` (01:00 correctly repeated) |
| January / July, New York | `EST dst=0` / `EDT dst=1` |
| January / July, Sydney | `AEDT dst=1` / `AEST dst=0` (southern hemisphere) |
| Live switch to Sydney | `2026-08-17 06:48:52 AEST`, correct across the date line |

`tests/` runs 40 host checks in about a second — DST transitions can be verified in
August without waiting for March or flashing anything.

```bash
cd tests && make
```

## Two real bugs, both caught on hardware

**1. `retained` variables inherit the previous firmware's memory.** Backup SRAM is not
zero-initialized, so flashing a build with a different retained layout silently adopts
whatever the old build left at that address. This shipped reporting **70 Wi-Fi
recoveries** thirty seconds after first boot — it was reading the spike's old
`lastResetReason`. Fixed with a magic-word guard (`RETAINED_MAGIC`) that reinitializes
the block whenever the layout changes.

**2. The config CRC covered the CRC field itself.** `Config` contains a `float`, so it is
4-byte aligned: `crc` sits at offset 556 while `sizeof(Config)` is 560. Writing the CRC
length as `sizeof(Config) - sizeof(uint16_t)` gives **558** — two bytes past `crc`'s
offset. The checksum computed on save could never be reproduced on load, so *every
reboot silently reverted to defaults* while settings appeared to apply perfectly at
runtime. Fixed with `offsetof(Config, crc)`, and `tests/config_test.cpp` now asserts the
round-trip and demonstrates that the buggy length fails.

The second one is worth remembering: it was invisible from the outside. Settings applied,
the status string confirmed them, nothing errored. Only a deliberate set-reboot-recheck
cycle exposed it.

A third, smaller issue: `tzat` wrote its result into the same buffer the 2-second status
refresh owns, so probe results were clobbered before they could be read. Now has its own
`tzprobe` variable.

## Temporary control surface

Particle functions, replaced by the REST API in Phase 2. Names are capped at 12
characters on Gen 2.

```bash
particle call shop-clock settz "EST5EDT,M3.2.0,M11.1.0/2"
particle call shop-clock setcolor "255,136,0"
particle call shop-clock setbright 60
particle call shop-clock sethourfmt 12
particle call shop-clock tzat 1772953200   # preview local time at any UTC epoch
particle get  shop-clock status
```

`ip`, `reset` and `status` stay permanently as the plan's out-of-band rescue path.

## Not yet verified

**Router-reboot recovery.** The netwatch ladder (re-associate at 60 s, reboot at 5 min)
is written and compiles, but forcing a Wi-Fi outage remotely risks losing contact with a
clock nobody is standing next to. This needs either a deliberate test with someone
present or the Phase 6 soak to exercise it naturally. Everything else in the Phase 1
completion bar is confirmed.
