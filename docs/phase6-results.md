# Phase 6 — Polish

```
Flash  78,252 / 131,072   Static RAM  13,780   Free heap  ~26,900
Page   36,397 bytes raw -> 12,343 gzipped
```

## The bug the Wi-Fi test found

The recovery ladder had been written in Phase 1 and carried as "unverified" ever since,
because testing it seemed to require taking down every access point in the house. It does
not: the clock can take **itself** off the air with `WiFi.off()`, which exercises exactly
the path the ladder handles.

To make that safe on a device nobody is standing next to, `armOutageTest()` also sets an
**independent hard-reset deadline in `loop()`** — deliberately sharing no logic with the
ladder under test, since a backstop built from the thing being tested is not a backstop.

The first run failed, and the failure was more interesting than a simple non-recovery:

```
baseline  boots=23 recoveries=0
  t+  4s  unreachable
  t+385s  BACK: boots=24 recoveries=1
RESULT: the ladder did NOT recover it; the independent backstop reset fired
```

`recoveries=1` says the ladder kicked the radio once. The ladder's own 300-second reset
**never fired** — which is only possible if `WiFi.ready()` had become true again. So Wi-Fi
*did* re-associate at about t+62 s. The clock stayed unreachable anyway.

**`WiFi.off()` tears down the interface and takes the `TCPServer`'s listening socket with
it, and re-associating does not bring it back.** The radio comes up, `WiFi.ready()` reports
true, the recovery ladder considers itself finished — and the clock silently stops
answering on port 80 until something reboots it.

This is exactly what a real router reboot would have done to both clocks. Worse, it would
have looked fine from Home Assistant: MQTT is an outbound connection and reconnects
normally, so the entities would have kept updating while the config page was dead.

Two changes fix it:

- `Httpd::tick()` watches for the Wi-Fi down→up edge and calls `server.begin()` again.
- `main.cpp` calls `Httpd::tick()` and `MqttHa::tick()` **unconditionally**, each doing its
  own Wi-Fi check. They were previously gated on `NetWatch::wifiUp()`, which meant the tick
  never ran while Wi-Fi was down and so could never observe the edge — the fix would have
  been silently inert.

A `rebinds` counter is reported in `/api/state`, so a clock repeatedly losing its socket is
visible rather than mysterious.

Re-run against the fix:

```
baseline  boots=25 recoveries=1 rebinds=1
  t+  4s  unreachable
  t+ 84s  BACK  boots=25 recoveries=2 rebinds=2 uptime=98 cloud=True mqtt=True
  no reboot        : True
  ladder kicked    : True
  socket recreated : True
PASS
```

Offline for 80 seconds, recovered without a reboot, cloud and MQTT both back on their own.

## Config format v2, migrated rather than reset

Adding the DST setting bumped the config version. A plain CRC mismatch would have cost
every clock its timezone, location, schedule and broker password on the next boot.

Instead the v1 layout is kept in `config.h` and copied forward field by field, with a
`static_assert` so a future reshuffle breaks the build rather than silently shifting a
password by two bytes. `tests/config_test.cpp` covers the round-trip, rejection of a
corrupt or unknown-version record, and that the migrated record validates under the v2 CRC.

Verified on hardware: the running clock kept its MQTT password, its timezone, its location
and its owner's chosen effect across the version bump.

v2 also carries **15 reserved bytes**, so the next field needs neither a migration nor a
version bump.

## Settings and UI

- **Observe daylight saving time.** Suppresses the rule's DST half rather than rewriting
  the stored rule, so switching back needs no re-derivation from the browser. Applied after
  the parse fallback, so it covers a corrupt rule too. Verified: `21:34 EDT` ↔ `20:34 EST`.
- **12/24-hour** already existed since Phase 1; no change needed.
- **Colour names.** Every swatch is now labelled in words — *amber*, *cyan*, *pale amber* —
  in the display picker and on each schedule row. A swatch alone is no use to a red-green
  colourblind reader and a hex code is barely better at a glance. (Pure yellow initially
  landed in the "lime" bucket; boundary corrected.)
- **Backup and restore.** Exports every setting except passwords as JSON. The clock will
  not read passwords back, so an export cannot leak one and a restore leaves the target's
  own untouched. Round-trip verified on hardware.

## mDNS: deliberately skipped

The `MDNS 2.0.0` library does not expose its type at global scope and would need
reverse-engineering to integrate. Against that, the value is small: the clock's IP is
already available from the Home Assistant sensor, the Particle cloud variable, and the page
header. Not worth a persistent multicast listener on a device expected to run for months
without a reboot. Recorded as a decision rather than dropped quietly; it is a contained
addition if wanted later.

## Documentation

- `docs/deploying.md` — build, flash (including the Product-claimed path for `Clock2`),
  configure, verify in Home Assistant, and roll back
- `docs/hardware.md` — wiring, digit mapping, power, platform constraints

## Soak

80 minutes of sampling at 2-minute intervals, spanning both outage tests. The final
**55-minute uninterrupted stretch** (`boots` constant at 26):

| | |
|---|---|
| Samples answered | 28 / 28 |
| Free heap | 26,760 bytes at every sample but one |
| Frames rendered | 100,520, steady at ~30 fps |
| Wi-Fi / cloud / MQTT | up throughout |
| Reboots, Wi-Fi recoveries | none |
| RSSI | −68 to −72 dBm |

One sample read 24,232 bytes free — about 2.5 kB below the flat line — and the next
returned to exactly 26,760. A transient allocation during request handling that releases
cleanly, not a leak: a leak does not come back to precisely its previous value. Worth
recording rather than smoothing over, since free memory is the number most likely to
reveal a slow problem on this platform.

The three unreachable samples in the log are the *first* outage test, before the
re-listen fix. There are none afterwards.

## Both clocks deployed

`Clock2` flashed with a plain `particle flash` — the Product claim needed no special
handling, which settles the one deployment unknown carried since Phase 0.

| Device | HA name | IP | RSSI |
|---|---|---|---|
| `shop-clock` | `Lixie Clock 343437` | 10.0.1.14 | −71 dBm |
| `Clock2` | `Lixie Clock 373132` | 10.0.1.167 | −39 dBm |

`Clock2` came up on defaults, which happen to be correct for this location (Eastern time,
Durham coordinates, 4 digits), synced NTP within 15 seconds, and connected to the broker on
the first attempt. Command and release were verified over MQTT and it was left exactly as
found.

Both clocks now appear as separate Home Assistant devices with eight entities each.

## Nothing outstanding

Every item carried through the six phases is closed.
