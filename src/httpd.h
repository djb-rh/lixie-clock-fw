#pragma once
#include "Particle.h"

// Minimal HTTP/1.0 server for the config page and its REST API.
//
// Serves one client at a time with no keep-alive. That is not a limitation
// worth engineering around here: the page is a single gzipped blob plus a
// handful of small JSON calls, and the clock's real job is rendering. Nothing
// in here blocks for more than the read timeout.

namespace Httpd {

void begin();
void tick();

uint32_t requestCount();
uint32_t rejectedAuth();
uint32_t rebinds();   // times the listening socket was recreated after a Wi-Fi drop

}  // namespace Httpd
