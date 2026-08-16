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
};

struct Settings {
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

}  // namespace Control
