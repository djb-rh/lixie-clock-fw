#pragma once
#include <stdint.h>

// Sunrise and sunset from latitude and longitude.
//
// Implements the NOAA solar-position algorithm, written from the published
// method rather than vendored from the LGPLv3 Sunrise library the old firmware
// used -- see the licensing note in the README. Particle-free, so it is
// validated on the host against an independent reference.
//
// Results are UTC epoch seconds, not minutes-of-day. That is deliberate: sunset
// frequently falls on the *next* UTC day (Durham's mid-August sunset is just
// after 00:00 UTC), and a minutes-of-day API invites callers to lose the
// rollover. Epochs cannot be got wrong that way.

struct SunTimes {
    int64_t rise_utc;
    int64_t set_utc;
    bool valid;        // false when the sun neither rises nor sets that day
    bool always_up;    // meaningful only when !valid: midnight sun vs polar night
};

// `y`/`m`/`d` identify the UTC date whose solar noon is used. Callers working in
// local time should pass the UTC date containing local noon -- see
// sunTimesForLocalDate().
SunTimes sunTimesUtcDate(double lat, double lon, int y, unsigned m, unsigned d);

// Convenience for the scheduler: given a local civil date and the UTC offset in
// effect (seconds WEST of UTC, POSIX convention), return that local day's events.
SunTimes sunTimesForLocalDate(double lat, double lon, int y, unsigned m, unsigned d,
                              int32_t utc_offset_west);
