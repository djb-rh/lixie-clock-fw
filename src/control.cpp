#include "control.h"

#include "timekeep.h"

namespace {

// Schedule transitions land on minute boundaries, so re-resolving every few
// seconds is far more often than needed. It is cheap regardless: the solar cache
// means a full evaluation costs at most one sun computation per day scanned.
const uint32_t RESOLVE_INTERVAL_MS = 5000;

uint32_t g_lastResolve = 0;
bool g_dirty = true;

Control::Settings g_active{};
Control::Source g_source = Control::SRC_DEFAULT;
int8_t g_entry = -1;
int64_t g_since = 0;
int64_t g_next = 0;
SunTimes g_sun{};

bool timeUsable() { return (uint32_t)Time.now() > 1735689600UL; }   // after 2025

void resolve() {
    // The base layer is whatever the config page last stored.
    g_active.mode = cfg.mode;
    g_active.effect = cfg.effect;
    g_active.r = cfg.r;
    g_active.g = cfg.g;
    g_active.b = cfg.b;
    g_active.brightness = cfg.brightness;
    g_source = Control::SRC_DEFAULT;
    g_entry = -1;
    g_since = 0;
    g_next = 0;
    g_sun = SunTimes{};

    // Without a trustworthy clock, a schedule cannot be evaluated at all --
    // falling back to the base settings is the only honest answer.
    if (!timeUsable()) return;

    const int64_t now = (int64_t)Time.now();
    const TzInfo &tz = Timekeep::tz();

    LocalTime lt = localFromUtc(tz, now);
    g_sun = sunTimesForLocalDate(cfg.lat, cfg.lon, lt.year, lt.month, lt.day,
                                 tzOffsetFor(tz, now));

    Resolved r = scheduleEvaluate(cfg.schedule, MAX_SCHEDULE, tz,
                                  cfg.lat, cfg.lon, now);
    g_next = scheduleNextFire(cfg.schedule, MAX_SCHEDULE, tz,
                              cfg.lat, cfg.lon, now);

    if (!r.active) return;

    g_active.mode = r.mode;
    g_active.effect = r.effect;
    g_active.r = r.r;
    g_active.g = r.g;
    g_active.b = r.b;
    g_active.brightness = r.brightness;
    g_source = Control::SRC_SCHEDULE;
    g_entry = r.entry;
    g_since = r.since_utc;
}

}  // namespace

void Control::begin() {
    g_dirty = true;
    tick();
}

void Control::invalidate() { g_dirty = true; }

void Control::tick() {
    uint32_t now = millis();
    if (!g_dirty && (now - g_lastResolve) < RESOLVE_INTERVAL_MS) return;
    g_lastResolve = now;
    g_dirty = false;
    resolve();
}

Control::Settings Control::active() { return g_active; }
Control::Source Control::source() { return g_source; }
int8_t Control::scheduleEntry() { return g_entry; }
int64_t Control::since() { return g_since; }
int64_t Control::nextChange() { return g_next; }
SunTimes Control::sunToday() { return g_sun; }

const char *Control::sourceName() {
    switch (g_source) {
        case SRC_SCHEDULE: return "schedule";
        default: return "default";
    }
}
