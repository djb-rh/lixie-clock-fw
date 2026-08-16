# lixie-clock-fw

Firmware for Lixie-style edge-lit LED clocks running on a **Particle Photon 1 (Gen 2)**.

Each clock configures itself from a built-in web page, keeps time over NTP with real
timezone and DST handling, runs lighting effects on a sunrise/sunset-aware schedule,
and shows up in Home Assistant automatically over MQTT.

Replaces the earlier [djb-rh/lixie-clock](https://github.com/djb-rh/lixie-clock)
firmware, which had compile-time colors and four hardcoded modes.

## Status

**Phase 2 (web server + config UI) complete.** `shop-clock` keeps correct local time with
full DST handling, and every setting is configurable from a browser at
`http://<clock-ip>/`. Effects, scheduling and Home Assistant are Phases 3–5. See `docs/`
for per-phase results.

## Tests

Calendar, timezone, JSON and config-layout logic is Particle-free and tested on the host:

```bash
cd tests && make
```

`make asan` reruns everything under AddressSanitizer and UBSan — worth it for the JSON
scanner, which parses untrusted input off the network.

`make vectors` regenerates `tz_vectors.txt`, which cross-checks the device's POSIX parser
against the browser's Intl-derived rules across 38 zones (needs `node`).

## Hardware

| Clock | Device ID | Notes |
|---|---|---|
| `Clock2` | `270049000251353530373132` | Particle Product 42326 |
| `shop-clock` | `290028001047363333343437` | Developer device |

WS2812B, 20 LEDs per digit, data on `D0`. Digit count is configurable (2–6).

## Building

Requires the [Particle CLI](https://docs.particle.io/getting-started/developer-tools/cli/).
Device OS is pinned to **2.3.1**, the terminal 2.x LTS release — Gen 2 devices cannot
run 3.x or later.

```bash
python3 tools/build_web_assets.py        # regenerate src/web_assets.h
particle compile photon --target 2.3.1
```

The config page lives in flash as a gzipped blob, because the Photon 1 has no
filesystem and only 128 KB of application flash. **Run the asset script after any
change to `web/index.html`** — `src/web_assets.h` is generated and not committed.

To flash:

```bash
particle flash shop-clock
```

## Design notes

- **Cloud-optional.** The Particle Cloud is kept for OTA flashing only. Everything
  the clock does — timekeeping, schedule, web UI, Home Assistant — works with the
  cloud unreachable. If Particle ever retires Gen 2 cloud service, the clocks keep
  running; only OTA is lost.
- **Timezones carry no on-device database.** The browser derives a POSIX TZ rule
  (e.g. `EST5EDT,M3.2.0,M11.1.0/2`) and the device stores those 48 bytes. Re-saving
  the page picks up any rule changes your browser has learned.
- **MQTT is plaintext** on the LAN. TLS is not practical in the Photon 1's RAM budget.
- **No `String` in hot paths.** Fixed `char` buffers throughout; the Photon's `String`
  fragments the heap and these run for months at a time.
- **Optional web password** gates writes only, and is not real security over plain
  HTTP — it prevents accidents, not attackers.

## Licensing

MIT. The solar math in `src/solar.cpp` is written from the published NOAA algorithm
rather than vendored from the LGPLv3 `Sunrise` library the old firmware used, so the
whole tree stays permissively licensed with no copyleft entanglement.
