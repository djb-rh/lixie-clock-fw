#include "schedule.h"

#include "solar.h"

namespace {

// How far back to look for the most recent transition. A schedule can have
// entries on Sundays only, so a week is the minimum; eight days gives a day of
// slack for sun-anchored entries whose offsets push them across midnight.
const int LOOKBACK_DAYS = 8;
const int LOOKAHEAD_DAYS = 8;

// Computing sunrise and sunset costs a handful of double-precision trig calls,
// and a full evaluation touches every day in the window. Caching per local date
// turns that into at most one solar computation per day rather than one per
// entry per day.
struct SolarCache {
    int32_t day = INT32_MIN;
    bool valid = false;
    SunTimes times{};
};

SunTimes solarFor(SolarCache &cache, double lat, double lon,
                  const TzInfo &tz, int32_t localDay) {
    if (cache.valid && cache.day == localDay) return cache.times;

    int y; unsigned m, d;
    civilFromDays(localDay, y, m, d);

    // Use the offset in effect around local noon, so a DST transition day does
    // not shift the solar day by an hour.
    int64_t noonGuess = (int64_t)localDay * 86400 + 12 * 3600;
    int32_t off = tzOffsetFor(tz, noonGuess);

    cache.day = localDay;
    cache.times = sunTimesForLocalDate(lat, lon, y, m, d, off);
    cache.valid = true;
    return cache.times;
}

// The UTC instant at which `e` fires on the given local day, or false if it does
// not fire that day (wrong weekday, or no sunrise/sunset at that latitude).
bool fireTime(const ScheduleEntry &e, const TzInfo &tz, double lat, double lon,
              SolarCache &cache, int32_t localDay, int64_t &out) {
    if (!e.enabled) return false;

    int dow = dowFromDays(localDay);
    if (!(e.days_mask & (1u << dow))) return false;

    if (e.anchor == ANCHOR_CLOCK) {
        int64_t localMidnight = (int64_t)localDay * 86400;
        int64_t wall = localMidnight + (int64_t)e.minutes * 60;
        // Convert wall-clock local to UTC using that day's noon offset, which is
        // unambiguous even on a transition day.
        //
        // On the two nights a year when local time is not a function, an entry
        // in the affected hour is off by an hour: a 02:30 entry fires at 01:30
        // local on spring-forward night, because 02:30 EDT does not exist. There
        // is no correct answer there -- what matters is that it fires exactly
        // once and never disappears, which is what this guarantees.
        int32_t off = tzOffsetFor(tz, localMidnight + 12 * 3600);
        out = wall + off;
        return true;
    }

    SunTimes s = solarFor(cache, lat, lon, tz, localDay);
    if (!s.valid) return false;      // polar day or night: sun anchors cannot fire

    int64_t base = (e.anchor == ANCHOR_SUNRISE) ? s.rise_utc : s.set_utc;
    out = base + (int64_t)e.minutes * 60;
    return true;
}

int32_t localDayOf(const TzInfo &tz, int64_t utc) {
    int64_t local = utc - tzOffsetFor(tz, utc);
    int32_t days = (int32_t)(local / 86400);
    if (local < 0 && local % 86400 != 0) days--;
    return days;
}

}  // namespace

Resolved scheduleEvaluate(const ScheduleEntry *entries, uint8_t count,
                          const TzInfo &tz, double lat, double lon,
                          int64_t now_utc) {
    Resolved out{};
    out.active = false;
    out.entry = -1;
    out.since_utc = 0;

    if (!entries || count == 0) return out;

    SolarCache cache;
    int32_t today = localDayOf(tz, now_utc);
    int64_t best = INT64_MIN;

    // Scan every day in the window rather than stopping at the first day that
    // yields a candidate. A sun-anchored entry with a large offset can fire on
    // the following calendar day, so "later day" does not reliably mean "later
    // instant" -- taking the global maximum is the only correct answer.
    for (int back = 0; back <= LOOKBACK_DAYS; back++) {
        int32_t day = today - back;
        for (uint8_t i = 0; i < count; i++) {
            int64_t when;
            if (!fireTime(entries[i], tz, lat, lon, cache, day, when)) continue;
            if (when > now_utc) continue;
            if (when > best) {
                best = when;
                out.active = true;
                out.entry = (int8_t)i;
                out.since_utc = when;
                out.mode = entries[i].mode;
                out.effect = entries[i].effect;
                out.r = entries[i].r;
                out.g = entries[i].g;
                out.b = entries[i].b;
                out.brightness = entries[i].brightness;
            }
        }
    }
    return out;
}

int64_t scheduleNextFire(const ScheduleEntry *entries, uint8_t count,
                         const TzInfo &tz, double lat, double lon,
                         int64_t now_utc) {
    if (!entries || count == 0) return 0;

    SolarCache cache;
    int32_t today = localDayOf(tz, now_utc);
    int64_t best = INT64_MAX;

    for (int fwd = -1; fwd <= LOOKAHEAD_DAYS; fwd++) {
        int32_t day = today + fwd;
        for (uint8_t i = 0; i < count; i++) {
            int64_t when;
            if (!fireTime(entries[i], tz, lat, lon, cache, day, when)) continue;
            if (when <= now_utc) continue;
            if (when < best) best = when;
        }
    }
    return (best == INT64_MAX) ? 0 : best;
}
