#pragma once
#include <stdint.h>

// Lighting effects.
//
// An effect is a pure function of (position, time, base colour) -- no state, no
// allocation, no Particle dependency -- so the whole set can be swept on the
// host. See tests/effects_test.cpp.
//
// Position is the panel's horizontal place across the display, normalised to
// 0..1. That is the coordinate that reads visually: a Lixie's ten numeral panels
// are stacked front-to-back within one digit, so depth is not a useful axis, but
// left-to-right across the digits is.
//
// Global brightness is NOT applied here. Display::show() scales afterwards, so
// the Home Assistant dimmer behaves identically in every mode.

struct Rgb8 { uint8_t r, g, b; };

enum : uint8_t {
    FX_SOLID = 0,
    FX_RAINBOW_FLOW,
    FX_RAINBOW_CYCLE,
    FX_BREATHE,
    FX_PULSE,
    FX_COMET,
    FX_TWINKLE,
    FX_WARM_GLOW,
    FX_COUNT,
};

// Must stay in step with the EFFECTS array in web/index.html and with the
// effect_list published to Home Assistant in Phase 5.
extern const char *const EFFECT_NAMES[FX_COUNT];

struct EffectCtx {
    uint8_t panel;      // 0-based, left to right
    uint8_t panels;     // total lit panels, >= 1
    uint8_t numeral;    // 0..9, the digit being shown
    uint32_t t_ms;      // animation clock
    Rgb8 base;          // the configured colour
};

// Never returns full black: a clock you cannot read is not a clock, so every
// effect floors its output. See FX_MIN_LEVEL.
Rgb8 effectColor(uint8_t effect, const EffectCtx &c);

// Floor applied to brightness-modulating effects, out of 255. Breathe and pulse
// dip to this rather than to zero.
const uint8_t FX_MIN_LEVEL = 64;

// Exposed for testing.
Rgb8 hsv2rgb(uint8_t h, uint8_t s, uint8_t v);
uint8_t fxSin8(uint8_t theta);       // 0..255 sine, 0..255 input
