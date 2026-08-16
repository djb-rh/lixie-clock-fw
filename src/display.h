#pragma once
#include "Particle.h"
#include "effects.h"
#include "tz.h"

// Digit rendering.
//
// Physical layout, carried over from the original firmware: each digit panel has
// 20 LEDs, two per numeral, and lighting numeral n on panel p means LEDs
// p*20 + n*2 and p*20 + n*2 + 1. Panel 0 is leftmost.
//
// Brightness is applied here in our own colour math rather than through
// NeoPixel's setBrightness(), so effects composite against a linear buffer and
// the global dimmer behaves identically in every mode -- including when Home
// Assistant is driving it.

namespace Display {

void begin();
void setDigitCount(uint8_t n);      // 2..MAX_DIGITS
uint8_t digitCount();
uint16_t ledCount();

void clear();
void setNumeral(uint8_t panel, uint8_t numeral, Rgb8 c);
void show(uint8_t brightness_pct);
uint16_t ledIndex(uint8_t panel, uint8_t numeral);

// Lays out hh:mm(:ss) across the panels and colours each lit numeral through the
// given effect. Pass FX_SOLID for a flat colour.
void renderClock(const LocalTime &lt, uint8_t effect, Rgb8 base,
                 uint8_t brightness_pct, uint32_t t_ms);

// Diagnostics. Nobody can see the clock from a terminal, so the status endpoint
// reports how many frames have actually been pushed and what colour the
// leftmost lit panel last received -- enough to tell a running effect from a
// stalled one without being in the room.
uint32_t frameCount();
Rgb8 lastLitColor();

}  // namespace Display
