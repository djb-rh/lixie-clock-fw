#pragma once
#include "Particle.h"
#include "tz.h"

// Digit rendering.
//
// Physical layout, carried over from the original firmware: each digit panel has
// 20 LEDs, two per numeral, and lighting numeral n on panel p means LEDs
// p*20 + n*2 and p*20 + n*2 + 1. Panel 0 is leftmost.
//
// Brightness is applied here in our own colour math rather than through
// NeoPixel's setBrightness(), so the effect engine in Phase 3 can composite
// against a linear buffer and the global dimmer behaves identically in every
// mode -- including when Home Assistant is driving it.

struct Rgb { uint8_t r, g, b; };

namespace Display {

void begin();
void setDigitCount(uint8_t n);      // 2..MAX_DIGITS
uint8_t digitCount();
uint16_t ledCount();

void clear();
void setNumeral(uint8_t panel, uint8_t numeral, Rgb c);
void show(uint8_t brightness_pct);

// Lays out hh:mm across the panels and renders it in one colour.
void renderClock(const LocalTime &lt, Rgb c, uint8_t brightness_pct);

// Which panels are lit, and with which numeral, for the current time. Phase 3's
// effects need this to colour each lit numeral by its position.
uint16_t ledIndex(uint8_t panel, uint8_t numeral);

}  // namespace Display
