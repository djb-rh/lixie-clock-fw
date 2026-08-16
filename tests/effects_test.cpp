// Host tests for the effects engine.
//
// Effects are pure functions, so the whole set can be swept over time and
// position here rather than by staring at a clock in another room. The
// invariant that actually matters: whatever an effect does, the time has to
// stay readable. An effect that dips to black turns the clock off.

#include "../src/effects.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int luma(Rgb8 c) { return (c.r * 30 + c.g * 59 + c.b * 11) / 100; }

int main() {
    char buf[220];
    const Rgb8 BASE = {255, 136, 0};        // the default amber
    const uint32_t SPAN = 600000;           // 10 minutes of animation
    const uint32_t STEP = 17;               // deliberately not a frame multiple

    printf("--- names line up with the count ---\n");
    bool named = true;
    for (int i = 0; i < FX_COUNT; i++)
        if (!EFFECT_NAMES[i] || !EFFECT_NAMES[i][0]) named = false;
    check(named, "every effect id has a name");
    check(!strcmp(EFFECT_NAMES[FX_SOLID], "Solid"), "id 0 is Solid");

    printf("\n--- solid is exactly the configured colour ---\n");
    bool exact = true;
    for (uint32_t t = 0; t < SPAN; t += 9999) {
        EffectCtx c{2, 4, 7, t, BASE};
        Rgb8 o = effectColor(FX_SOLID, c);
        if (o.r != BASE.r || o.g != BASE.g || o.b != BASE.b) { exact = false; break; }
    }
    check(exact, "Solid never modulates");

    printf("\n--- no effect may ever black out the display ---\n");
    // The one invariant a clock cannot violate.
    for (int fx = 0; fx < FX_COUNT; fx++) {
        int worst = 255;
        uint32_t worstT = 0;
        int worstPanel = 0;
        for (uint8_t panels = 2; panels <= 6; panels += 2) {
            for (uint8_t p = 0; p < panels; p++) {
                for (uint32_t t = 0; t < SPAN; t += STEP) {
                    Rgb8 o = effectColor((uint8_t)fx, EffectCtx{p, panels, 8, t, BASE});
                    int l = luma(o);
                    if (l < worst) { worst = l; worstT = t; worstPanel = p; }
                }
            }
        }
        snprintf(buf, sizeof(buf),
                 "%-14s dimmest sample is luma %3d (panel %d at %lums)",
                 EFFECT_NAMES[fx], worst, worstPanel, (unsigned long)worstT);
        check(worst > 0, buf);
    }

    printf("\n--- at least one panel stays legible at every instant ---\n");
    // Comet deliberately drives individual panels to their floor; what matters
    // is that the display as a whole is never dark.
    for (int fx = 0; fx < FX_COUNT; fx++) {
        int worstFrame = 255;
        uint32_t worstT = 0;
        for (uint32_t t = 0; t < SPAN; t += STEP) {
            int best = 0;
            for (uint8_t p = 0; p < 4; p++) {
                int l = luma(effectColor((uint8_t)fx, EffectCtx{p, 4, 8, t, BASE}));
                if (l > best) best = l;
            }
            if (best < worstFrame) { worstFrame = best; worstT = t; }
        }
        snprintf(buf, sizeof(buf), "%-14s brightest panel never drops below luma %d (at %lums)",
                 EFFECT_NAMES[fx], worstFrame, (unsigned long)worstT);
        check(worstFrame >= 20, buf);
    }

    printf("\n--- effects are deterministic ---\n");
    bool stable = true;
    for (int fx = 0; fx < FX_COUNT && stable; fx++) {
        for (uint32_t t = 0; t < 50000; t += 313) {
            EffectCtx c{1, 4, 3, t, BASE};
            Rgb8 a = effectColor((uint8_t)fx, c);
            Rgb8 b = effectColor((uint8_t)fx, c);
            if (memcmp(&a, &b, sizeof(a))) { stable = false; break; }
        }
    }
    check(stable, "same inputs give the same colour (no hidden rand())");

    printf("\n--- animated effects actually animate ---\n");
    for (int fx = 1; fx < FX_COUNT; fx++) {
        int lo = 1 << 20, hi = -1;
        for (uint32_t t = 0; t < SPAN; t += STEP) {
            int l = luma(effectColor((uint8_t)fx, EffectCtx{1, 4, 8, t, BASE}));
            if (l < lo) lo = l;
            if (l > hi) hi = l;
        }
        // Rainbow effects hold luma roughly constant while sweeping hue, so
        // check chroma movement too rather than demanding brightness change.
        int hueSpread = 0;
        Rgb8 first = effectColor((uint8_t)fx, EffectCtx{1, 4, 8, 0, BASE});
        for (uint32_t t = 0; t < SPAN; t += STEP) {
            Rgb8 o = effectColor((uint8_t)fx, EffectCtx{1, 4, 8, t, BASE});
            int d = abs((int)o.r - first.r) + abs((int)o.g - first.g) + abs((int)o.b - first.b);
            if (d > hueSpread) hueSpread = d;
        }
        snprintf(buf, sizeof(buf), "%-14s luma range %d..%d, max colour excursion %d",
                 EFFECT_NAMES[fx], lo, hi, hueSpread);
        check(hueSpread > 30, buf);
    }

    printf("\n--- position matters where it should ---\n");
    // Flow and comet must differ across panels at a given instant; cycle must not.
    auto spreadAt = [&](int fx, uint32_t t) {
        int lo = 1 << 20, hi = -1;
        for (uint8_t p = 0; p < 4; p++) {
            Rgb8 o = effectColor((uint8_t)fx, EffectCtx{p, 4, 8, t, BASE});
            int v = o.r + o.g + o.b;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        return hi - lo;
    };
    int flowSpread = 0, cometSpread = 0, cycleSpread = 0;
    for (uint32_t t = 0; t < SPAN; t += STEP) {
        if (spreadAt(FX_RAINBOW_FLOW, t) > flowSpread) flowSpread = spreadAt(FX_RAINBOW_FLOW, t);
        if (spreadAt(FX_COMET, t) > cometSpread) cometSpread = spreadAt(FX_COMET, t);
        if (spreadAt(FX_RAINBOW_CYCLE, t) > cycleSpread) cycleSpread = spreadAt(FX_RAINBOW_CYCLE, t);
    }
    snprintf(buf, sizeof(buf), "Rainbow Flow varies across panels (spread %d)", flowSpread);
    check(flowSpread > 60, buf);
    snprintf(buf, sizeof(buf), "Comet varies across panels (spread %d)", cometSpread);
    check(cometSpread > 60, buf);
    snprintf(buf, sizeof(buf), "Rainbow Cycle is uniform across panels (spread %d)", cycleSpread);
    check(cycleSpread == 0, buf);

    printf("\n--- degenerate inputs ---\n");
    Rgb8 one = effectColor(FX_RAINBOW_FLOW, EffectCtx{0, 1, 0, 1234, BASE});
    check(luma(one) > 0, "single-panel display does not divide by zero");
    Rgb8 unknown = effectColor(200, EffectCtx{0, 4, 0, 999, BASE});
    check(unknown.r == BASE.r && unknown.g == BASE.g && unknown.b == BASE.b,
          "unknown effect id falls back to the solid colour");
    Rgb8 black = effectColor(FX_BREATHE, EffectCtx{0, 4, 0, 5000, Rgb8{0, 0, 0}});
    check(black.r == 0 && black.g == 0 && black.b == 0,
          "a black base stays black (the floor is relative, not additive)");

    printf("\n--- helpers ---\n");
    check(fxSin8(0) == 128 && fxSin8(64) == 255 && fxSin8(192) == 1,
          "fxSin8 hits its midpoint, peak and trough");
    Rgb8 red = hsv2rgb(0, 255, 255);
    check(red.r == 255 && red.g == 0 && red.b == 0, "hsv2rgb(0,255,255) is red");
    Rgb8 white = hsv2rgb(100, 0, 255);
    check(white.r == 255 && white.g == 255 && white.b == 255, "zero saturation is white");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
