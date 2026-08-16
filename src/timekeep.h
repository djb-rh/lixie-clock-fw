#pragma once
#include "Particle.h"
#include "tz.h"

// NTP client and current-time access. The timezone math itself lives in tz.h,
// which is Particle-free so it can be tested on the host.
//
// Particle's Time stays UTC throughout; nothing here calls Time.zone().

namespace Timekeep {

void begin();
void tick();

bool syncNow();               // blocking NTP round trip, ~1.5 s worst case
bool everSynced();
uint32_t secondsSinceSync();  // 0 if never synced
bool isStale();               // no successful sync in 24 h
uint32_t failedSyncs();

LocalTime now();
const TzInfo &tz();
bool tzValid();               // false if the stored rule was unparseable
bool setTz(const char *posix);   // validates before storing; does not save

}  // namespace Timekeep
