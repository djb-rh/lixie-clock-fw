# Phase 4 — Solar and scheduler

Sunrise/sunset accurate to well under a minute, and a schedule that resolves sun-anchored
entries to the second.

```
Flash  69,204 / 131,072   Static RAM  11,472   Free heap  ~31,900
```

## Solar accuracy: measured, not asserted

`src/solar.cpp` implements the NOAA solar-position algorithm, written fresh from the
published method (the old firmware's `Sunrise` library is LGPLv3 — see the README).

Validating our maths against our own maths would prove nothing, so the expected values
come from **`astral`**, a separate, widely used implementation.

| | Worst | Mean |
|---|---|---|
| This implementation | **72 s** (Reykjavík, summer solstice) | 20 s |
| Phase 0's approximation | 347 s | 76 s |

69 sample days across 9 locations spanning the equator, both hemispheres, and latitudes
up to Tromsø at 69.6°N. The plan's bar was ≤2 minutes.

**Two things had to be fixed to get there, and one claim had to be retracted.**

*Iteration.* Evaluating the sun's position once at noon is not good enough: declination
and the equation of time both move over a day, and sunrise can be eight hours from noon
at high latitude. One-shot-at-noon measured 1250 s out at Tromsø; refining at the event
time collapses it.

*Reference disagreement.* I started with `api.sunrise-sunset.org`, which put us 70 s out
in a consistent direction. A third opinion settled it: `astral` and this firmware agree
to ~13 s, so the API is the outlier — it appears to use a different horizon constant.
Where two references disagree, the tie-break is which one matches the published method.
The original vectors are kept in `tests/solar_vectors_sunrisesunset.txt` for comparison
rather than deleted.

*Retraction.* `docs/phase0-results.md` claimed the simplified formula was "3–4 minutes
off" against almanac values. Those values were recalled, not looked up, and the claim was
wrong in both directions — worse than stated at high latitude, better at Durham. That
note is now corrected in place.

Sun times are returned as **UTC epochs, not minutes-of-day**, because sunset routinely
falls on the next UTC day (Durham's mid-August sunset is after 00:00 UTC) and a
minutes-of-day API invites callers to lose the rollover — exactly the Phase 0 gotcha.

## The scheduler

Entries are transition points: at its moment an entry's settings become the clock's and
hold until the next fires. There is no end time and no overlap.

Evaluation always looks **backward** for the most recent entry that would have fired, so
boot-time catch-up is a property of the design rather than a special case. It scans the
whole 8-day window and takes the global maximum rather than stopping at the first day
with a hit — a sun-anchored entry with a large offset can fire on the following calendar
day, so "later day" does not reliably mean "later instant".

`tests/schedule_test.cpp` covers 26 cases including sparse day masks reaching back six
days, sunset+6h crossing midnight, polar night (sun anchors cannot fire, clock anchors
still do), and both DST boundaries. On spring-forward night a 02:30 entry fires at 01:30
local, because 02:30 does not exist that night — there is no correct answer, and what the
test pins is that it fires *exactly once* and never disappears.

## The control layer

`src/control.{h,cpp}` owns the precedence:

```
base config  →  schedule  →  [Home Assistant override, Phase 5]
```

Rendering reads the resolved result, never `cfg` directly, so exactly one place knows the
ordering and the status page can name the layer that won.

## Two bugs found on hardware

**1. `%lld` is not supported by newlib-nano's printf.** Passing an `int64_t` emitted the
literal `ld`, then desynchronised every argument after it — `/api/state` started returning
corrupt JSON with trailing garbage bytes. Epochs are now cast to `unsigned long`, which is
valid until 2106. The compiler gives no warning for this; only parsing the response
catches it.

**2. The status endpoint reported the wrong effect.** It read `cfg.mode`/`cfg.effect`
instead of the resolved layer, so it said "Solid" while a schedule entry was visibly
running Breathe. The `lit` colour was modulating in the same response, which is what gave
it away. A diagnostic that lies is worse than no diagnostic.

## Verified on hardware

- Sunrise 06:34:56 EDT, sunset 20:04:12 EDT for Durham on 2026-08-16 — within **13 s** of
  `astral`
- A sunset-anchored entry scheduled to fire 75 s out fired at **exactly** the predicted
  instant (0 s error), with `since` advancing from yesterday's instance to today's and
  `next` rolling to tomorrow
- **Boot-time catch-up:** 8 s after a reboot the clock had already resumed its schedule
  entry rather than falling back to defaults
- A schedule entry running Breathe modulates on-device (`lit` 237 → 64 → 113 across polls)

## Still outstanding

Nobody has looked at the clock. And the router-reboot recovery from Phase 1 remains
unverified — still waiting on either someone present or the Phase 6 soak.
