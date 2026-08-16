#pragma once
#include <stdint.h>

#include "config.h"
#include "tz.h"

// Schedule evaluation.
//
// Entries are TRANSITION POINTS, not intervals: at its moment an entry's
// settings become the clock's settings and hold until the next entry fires.
// There is no "end time" and no concept of overlap.
//
// Everything here is a pure function of (entries, timezone, location, now), so
// it is Particle-free and tested on the host -- day masks crossed with sun
// anchors is exactly the sort of logic that looks right and is not.
//
// Boot-time catch-up falls out of the design rather than being a special case:
// evaluation always looks BACKWARD for the most recent entry that would have
// fired, so a clock rebooting at 03:00 resumes the overnight setting instead of
// waiting until morning for the next transition.

struct Resolved {
    bool active;          // false when no entry applies (empty schedule)
    uint8_t mode;
    uint8_t effect;
    uint8_t r, g, b;
    uint8_t brightness;
    int64_t since_utc;    // when the applying entry fired
    int8_t entry;         // index into the schedule, -1 when inactive
};

Resolved scheduleEvaluate(const ScheduleEntry *entries, uint8_t count,
                          const TzInfo &tz, double lat, double lon,
                          int64_t now_utc);

// When the next entry fires, or 0 if none is scheduled within the search
// horizon. Used only to display "next change at ..." -- evaluation itself never
// depends on it.
int64_t scheduleNextFire(const ScheduleEntry *entries, uint8_t count,
                         const TzInfo &tz, double lat, double lon,
                         int64_t now_utc);
