#pragma once
#include "Particle.h"

#include "config.h"
#include "schedule.h"
#include "solar.h"

// The layer stack that decides what the clock actually shows.
//
//   base config  (default effect/colour/brightness)
//        v  overridden by
//   schedule     (most recent fired entry, including sun anchors)
//        v  overridden by
//   Home Assistant override                              [Phase 5]
//        v
//   render target -> effect engine -> global brightness
//
// Resolution is centralised here rather than spread through main.cpp so that
// exactly one place knows the precedence, and the status page can name the layer
// that won instead of the user guessing why the clock looks like it does.

namespace Control {

enum Source : uint8_t {
    SRC_DEFAULT = 0,     // the configured base settings
    SRC_SCHEDULE = 1,    // a schedule entry
    SRC_HA = 2,          // Home Assistant, sticky until released
};

struct Settings {
    bool on;             // Home Assistant can turn the display off entirely
    uint8_t mode;
    uint8_t effect;
    uint8_t r, g, b;
    uint8_t brightness;
};

void begin();
void tick();             // cheap; re-resolves on a timer
void invalidate();       // call after any config or schedule change

Settings active();
Source source();
const char *sourceName();
int8_t scheduleEntry();  // index of the applying entry, -1 if none
int64_t since();         // when the current source took effect, 0 if unknown
int64_t nextChange();    // 0 if nothing is scheduled

// Today's sun times, for the status page. valid=false at polar latitudes.
SunTimes sunToday();

// --- Home Assistant override ---
//
// Sticky by design: once Home Assistant sets something it holds indefinitely,
// and only an explicit release hands control back to the schedule. Nothing
// silently undoes what an automation did -- a schedule transition arriving
// later must not quietly stomp it.
//
// The override deliberately does NOT survive a reboot. That looks like an
// oversight and is not: a clock that comes back lit is the owner's clearest
// signal that it restarted. Persisting an "off" would make a dark clock
// ambiguous -- crashed, or off as intended? -- and that ambiguity costs more
// than the brief disagreement with Home Assistant, which re-syncs at the next
// automation event anyway. Decided deliberately; do not "fix" it without asking
// whoever is living with the clock.
void setOverride(const Settings &s);
void releaseOverride();
bool overridden();
int64_t overrideSince();

}  // namespace Control
