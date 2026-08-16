#include "effects.h"

const char *const EFFECT_NAMES[FX_COUNT] = {
    "Solid", "Rainbow Flow", "Rainbow Cycle", "Breathe",
    "Pulse", "Comet", "Twinkle", "Warm Glow",
};

namespace {

// Quarter-wave sine table, 0..255 over 0..90 degrees. Integer throughout: the
// Photon 1 is a Cortex-M3 with no FPU, and while a handful of floats per frame
// would be affordable, there is no reason to spend them.
const uint8_t SIN_Q[65] = {
      0,   6,  12,  18,  25,  31,  37,  43,  49,  56,  62,  68,  74,  80,  86,  92,
     97, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152, 157, 162, 167, 171, 176,
    180, 185, 189, 193, 197, 201, 205, 208, 212, 215, 219, 222, 225, 228, 231, 233,
    236, 238, 240, 242, 244, 246, 247, 249, 250, 251, 252, 253, 254, 254, 255, 255,
    255,
};

uint8_t scale8(uint8_t v, uint8_t s) { return (uint8_t)(((uint16_t)v * s + 255) >> 8); }

// Deterministic per-panel noise. Twinkle must not use rand(): the same inputs
// have to give the same output so the effect can be swept in a host test.
uint8_t hash8(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (uint8_t)x;
}

}  // namespace

// Full-wave sine, 0..255 in and out, centred on 128.
uint8_t fxSin8(uint8_t theta) {
    uint8_t quadrant = (uint8_t)(theta >> 6);
    uint8_t idx = (uint8_t)(theta & 63);
    uint16_t v;
    switch (quadrant) {
        case 0: v = 128 + (SIN_Q[idx] >> 1); break;
        case 1: v = 128 + (SIN_Q[64 - idx] >> 1); break;
        case 2: v = 128 - (SIN_Q[idx] >> 1); break;
        default: v = 128 - (SIN_Q[64 - idx] >> 1); break;
    }
    return (uint8_t)v;
}

Rgb8 hsv2rgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return Rgb8{v, v, v};

    uint8_t region = (uint8_t)(h / 43);
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = scale8(v, (uint8_t)(255 - s));
    uint8_t q = scale8(v, (uint8_t)(255 - scale8(s, rem)));
    uint8_t t = scale8(v, (uint8_t)(255 - scale8(s, (uint8_t)(255 - rem))));

    switch (region) {
        case 0:  return Rgb8{v, t, p};
        case 1:  return Rgb8{q, v, p};
        case 2:  return Rgb8{p, v, t};
        case 3:  return Rgb8{p, q, v};
        case 4:  return Rgb8{t, p, v};
        default: return Rgb8{v, p, q};
    }
}

namespace {

// Scale a colour by a level, never letting it fall below FX_MIN_LEVEL of its
// own value, so the time stays readable at the bottom of every dip.
Rgb8 dim(Rgb8 c, uint8_t level) {
    if (level < FX_MIN_LEVEL) level = FX_MIN_LEVEL;
    return Rgb8{scale8(c.r, level), scale8(c.g, level), scale8(c.b, level)};
}

// Panel position as 0..255 across the display. A single-panel display sits in
// the middle rather than dividing by zero.
uint8_t panelPos(const EffectCtx &c) {
    if (c.panels <= 1) return 128;
    return (uint8_t)((uint16_t)c.panel * 255 / (c.panels - 1));
}

}  // namespace

Rgb8 effectColor(uint8_t effect, const EffectCtx &c) {
    const uint32_t t = c.t_ms;
    const uint8_t x = panelPos(c);

    switch (effect) {
        case FX_RAINBOW_FLOW: {
            // Hue sweeps across the display and drifts with time: a moving
            // rainbow rather than the whole clock changing colour together.
            uint8_t hue = (uint8_t)((t / 24) + (x * 2 / 3));
            return hsv2rgb(hue, 255, 255);
        }

        case FX_RAINBOW_CYCLE: {
            uint8_t hue = (uint8_t)(t / 24);
            return hsv2rgb(hue, 255, 255);
        }

        case FX_BREATHE: {
            // ~6 s period, deep and slow.
            uint8_t level = fxSin8((uint8_t)(t / 24));
            return dim(c.base, level);
        }

        case FX_PULSE: {
            // ~1.3 s period, and squared so it snaps rather than swells.
            uint8_t s = fxSin8((uint8_t)(t / 5));
            return dim(c.base, scale8(s, s));
        }

        case FX_COMET: {
            // A bright head sweeps left to right; panels trail off behind it.
            uint8_t head = (uint8_t)(t / 12);
            uint8_t d = (uint8_t)(head - x);          // wraps, so the tail wraps too
            uint8_t level = (d < 96) ? (uint8_t)(255 - (d * 255 / 96)) : 0;
            return dim(c.base, level);
        }

        case FX_TWINKLE: {
            // Each panel picks a new target every ~700 ms and eases toward it,
            // so it shimmers instead of strobing.
            uint32_t bucket = t / 700;
            uint8_t a = hash8(bucket * 31 + c.panel);
            uint8_t b = hash8((bucket + 1) * 31 + c.panel);
            uint8_t frac = (uint8_t)(((t % 700) * 255) / 700);
            uint8_t blend = (uint8_t)(((uint16_t)a * (255 - frac) + (uint16_t)b * frac) >> 8);
            return dim(c.base, (uint8_t)(140 + (blend >> 1)));
        }

        case FX_WARM_GLOW: {
            // Drifts slowly through amber and orange, ignoring the base colour --
            // the point of this one is the warmth.
            uint8_t hue = (uint8_t)(10 + (fxSin8((uint8_t)(t / 60)) >> 4));
            uint8_t val = (uint8_t)(200 + (fxSin8((uint8_t)(t / 43 + x)) >> 3));
            return hsv2rgb(hue, 240, val);
        }

        case FX_SOLID:
        default:
            return c.base;
    }
}
