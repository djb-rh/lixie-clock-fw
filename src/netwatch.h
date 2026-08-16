#pragma once
#include "Particle.h"

// Wi-Fi and cloud supervision.
//
// The display must never depend on the network, so nothing here blocks. This
// module only observes and, when something has clearly been wedged for long
// enough, escalates: re-associate, then reboot.

namespace NetWatch {

void begin();
void tick();

bool wifiUp();
bool cloudUp();
uint32_t wifiDownSeconds();     // 0 when up
int rssi();                     // cached; WiFi.RSSI() is slow to call per frame
uint32_t bootCount();
int lastResetReason();
uint32_t wifiRecoveries();      // radio restarts since boot

}  // namespace NetWatch
