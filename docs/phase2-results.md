# Phase 2 — Web server and config UI

Every setting is now changeable from a browser at `http://<clock-ip>/`, and survives a
power cycle.

```
Flash  52,704 / 131,072   Static RAM  11,408   Free heap  ~31,500
Page   29,143 bytes raw -> 10,060 gzipped
```

## Endpoints

| Method | Path | Notes |
|---|---|---|
| `GET` | `/` | The config page, gzipped, straight out of flash |
| `GET` | `/api/state` | Live status: time, NTP, Wi-Fi, memory, boots, reset reason |
| `GET`/`POST` | `/api/config` | All settings. POST fields are individually optional |
| `GET`/`POST` | `/api/schedule` | Whole-list replace, up to 24 entries |
| `POST` | `/api/action` | `resync`, `reboot`, `factory` |

`POST` is gated by the optional config password via an `X-Auth` header, compared in
constant time. `GET` stays open. Secrets are never read back — the config response says
`has_mqtt_pass: true` rather than returning the value.

## JSON without the heap

ArduinoJson 7 is heap-only (v7 removed `StaticJsonDocument`). Since these clocks are
meant to run for months, fragmentation matters more here than convenience, so `src/json`
is a non-allocating scanner that reads the caller's buffer in place. It is Particle-free
and tested against 16 malformed inputs, escape handling, nested paths, arrays, and a
buffer with no NUL terminator — under ASan/UBSan via `make asan`.

It is a scanner, not a validator: `{"a":1,,,}` still resolves `a`. That is pinned by a
test so it stays a deliberate choice rather than drifting into an assumption.

## Timezones: browser and firmware cross-validated

`tools/gen_tz_vectors.mjs` extracts the derivation code **from the shipping page**, runs
it over 38 zones, and records what Intl says each offset should be. `tests/tzvec_test.cpp`
then checks the device's own parser reproduces them.

**38 zones, 1,104 sampled instants, zero mismatches** — including samples one minute
either side of every DST transition, and the awkward cases:

| Zone | Derived rule |
|---|---|
| Australia/Lord_Howe | `<+1030>-10:30<+11>-11,M10.1.0,M4.1.0` (30-minute DST shift) |
| Pacific/Chatham | `<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45` |
| America/St_Johns | `<-0330>3:30<-0230>,M3.2.0,M11.1.0` |
| Africa/Cairo | `<+02>-2<+03>,M4.5.5/0,M10.5.5/0` (last Friday, midnight) |
| Antarctica/Troll | `GMT0<+02>-2,M3.5.0/1,M10.5.0/3` (two-hour shift) |

Nothing else in the suite proved the two halves agree, and a disagreement would silently
put a clock hours off.

## Coordinate entry

As specified: in-page help for finding coordinates on iPhone (Compass app) and Android
(Google Maps blue dot), a parser that accepts decimal *or* degrees-minutes-seconds *or* a
pasted `lat, lon` pair, an explicit warning that western longitudes are negative, and a
live sunrise/sunset preview that recomputes as you type.

`Auto-detect` tries browser geolocation but says plainly when it cannot: a page served
over `http://` is not a secure context, so Chrome and Safari block the API outright.

## Four bugs, all found by running it

**1. A malformed POST body returned `200 OK`.** No fields matched, so nothing was applied
— and the endpoint cheerfully reported success. `applyConfig` now counts applied fields
and returns 400 when none are recognized. Silent success is the worst answer a config
endpoint can give.

**2. `GET /api/schedule` truncated at 24 entries.** The 1536-byte response buffer could
not hold ~2.3 kB of schedule, so it emitted JSON that stopped mid-token. Worse, the
`p += snprintf(p, end - p, ...)` accumulator is itself wrong: `snprintf` returns the
length it *would* have written, so on truncation the pointer walks past the buffer and
every later call gets a negative size. Replaced with a bounded appender that tracks
overflow and turns it into a 500.

**3. `POST /api/schedule` rejected a full schedule with `413`.** The 2048-byte body limit
was under the ~2.8 kB a 24-entry save actually sends, so the clock refused a save from
its own config page. Both buffers are now derived from `MAX_SCHEDULE` rather than picked
as round numbers.

**4. I tested stale firmware twice** by polling `/api/state` until it answered — which
the *outgoing* build also does. This is the same trap recorded in Phase 0, walked into
again. `/api/state` now reports a per-compile `build` stamp (`__DATE__ " " __TIME__`),
and the verification loop waits for that string to *change*.

Bugs 2 and 3 only appear at full capacity; a schedule of two entries works perfectly.
Testing with the largest legal input rather than a plausible one is what surfaced them.

## Verification

- 4 host test binaries, also run clean under ASan/UBSan (`cd tests && make asan`)
- Static check that every `$('#id')` in the page exists, every schedule-row class is in
  the template, and every endpoint and action the page calls is actually routed
- On hardware: 24-entry schedule round-trip with zero field mismatches, auth accept and
  reject, transactional rollback on a bad timezone, `413` on oversize bodies, `404` on
  unknown endpoints, and the gzipped page arriving byte-identical to `web/index.html`

## Not yet verified

The page has not been opened in a real browser — this environment blocks private IPs in
the preview pane, so it was checked statically and through its API instead. Worth a look
on a phone, which is also the fastest way to sanity-check the coordinate help.
