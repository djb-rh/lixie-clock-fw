#pragma once
#include "Particle.h"

// Home Assistant integration over MQTT.
//
// Publishes HA MQTT Discovery configs so both clocks appear automatically as a
// light plus a handful of diagnostic sensors and a "Return to schedule" button.
// No custom HA component and no YAML to hand-write.
//
// Plaintext on the LAN: TLS is not practical in the Photon 1's RAM budget, and
// the README says so plainly rather than implying otherwise.

namespace MqttHa {

void begin();

// Recompute the Home Assistant identity from config and reconnect under it.
void refreshIdentity();
void tick();

bool connected();
bool configured();            // false when no broker host is set
uint32_t connectCount();
uint32_t commandCount();
uint32_t rxStalls();      // times the inbound path was found dead
const char *lastError();

// Push state to HA now, rather than waiting for the heartbeat. Called whenever
// something changes the display, so the HA entity does not lag reality.
void notifyChanged();

}  // namespace MqttHa
