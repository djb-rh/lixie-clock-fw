#include "display.h"
#include "config.h"
#include "neopixel.h"

namespace {
const uint8_t PIXEL_PIN = D0;
Adafruit_NeoPixel strip(MAX_DIGITS * LEDS_PER_DIGIT, PIXEL_PIN, WS2812B);
uint8_t g_digits = 4;

// Working buffer at full brightness; the dimmer is applied on the way out.
Rgb8 g_buf[MAX_DIGITS * LEDS_PER_DIGIT];

const uint8_t BLANK = 0xFF;

uint32_t g_frames = 0;
}  // namespace

void Display::begin() {
    strip.begin();
    strip.setBrightness(255);      // we scale colours ourselves
    clear();
    show(0);
}

void Display::setDigitCount(uint8_t n) {
    if (n < 2) n = 2;
    if (n > MAX_DIGITS) n = MAX_DIGITS;
    g_digits = n;
}

uint8_t Display::digitCount() { return g_digits; }
uint16_t Display::ledCount() { return (uint16_t)g_digits * LEDS_PER_DIGIT; }

uint16_t Display::ledIndex(uint8_t panel, uint8_t numeral) {
    return (uint16_t)panel * LEDS_PER_DIGIT + (uint16_t)numeral * 2;
}

void Display::clear() {
    memset(g_buf, 0, sizeof(g_buf));
}

void Display::setNumeral(uint8_t panel, uint8_t numeral, Rgb8 c) {
    if (panel >= g_digits || numeral > 9) return;
    uint16_t i = ledIndex(panel, numeral);
    g_buf[i] = c;
    g_buf[i + 1] = c;
}

void Display::show(uint8_t brightness_pct) {
    if (brightness_pct > 100) brightness_pct = 100;
    uint16_t n = ledCount();
    for (uint16_t i = 0; i < n; i++) {
        // +50 rounds instead of truncating, so brightness 1% is not simply black.
        uint8_t r = (uint8_t)(((uint16_t)g_buf[i].r * brightness_pct + 50) / 100);
        uint8_t g = (uint8_t)(((uint16_t)g_buf[i].g * brightness_pct + 50) / 100);
        uint8_t b = (uint8_t)(((uint16_t)g_buf[i].b * brightness_pct + 50) / 100);
        strip.setPixelColor(i, r, g, b);
    }
    // Panels beyond the configured digit count stay dark even if wired up.
    for (uint16_t i = n; i < MAX_DIGITS * LEDS_PER_DIGIT; i++) {
        strip.setPixelColor(i, 0, 0, 0);
    }
    strip.show();
    g_frames++;
}

uint32_t Display::frameCount() { return g_frames; }

Rgb8 Display::lastLitColor() {
    for (uint16_t i = 0; i < ledCount(); i++)
        if (g_buf[i].r || g_buf[i].g || g_buf[i].b) return g_buf[i];
    return Rgb8{0, 0, 0};
}

void Display::renderClock(const LocalTime &lt, uint8_t effect, Rgb8 base,
                          uint8_t brightness_pct, uint32_t t_ms) {
    // Decide which numeral each panel shows, then colour them. Keeping layout
    // separate from colouring is what lets an effect know a panel's position on
    // the display without duplicating the time-formatting rules.
    uint8_t lit[MAX_DIGITS];
    for (uint8_t i = 0; i < MAX_DIGITS; i++) lit[i] = BLANK;

    int hour = lt.hour;
    if (cfg.hour_format == 12) {
        hour = hour % 12;
        if (hour == 0) hour = 12;
    }

    // Panels fill from the right, so a 4-digit clock shows hh:mm and a 6-digit
    // one shows hh:mm:ss without any per-size special casing.
    int8_t p = (int8_t)g_digits - 1;
    if (g_digits >= 6) {
        lit[p--] = (uint8_t)(lt.second % 10);
        lit[p--] = (uint8_t)((lt.second / 10) % 10);
    }
    if (g_digits >= 4) {
        lit[p--] = (uint8_t)(lt.minute % 10);
        lit[p--] = (uint8_t)((lt.minute / 10) % 10);
    }
    if (p >= 0) lit[p--] = (uint8_t)(hour % 10);
    if (p >= 0) {
        uint8_t tens = (uint8_t)((hour / 10) % 10);
        // Blank the leading zero in 12-hour mode: 9:05, not 09:05.
        if (!(tens == 0 && cfg.hour_format == 12 && cfg.blank_hour_zero)) {
            lit[p] = tens;
        }
    }

    clear();
    for (uint8_t i = 0; i < g_digits; i++) {
        if (lit[i] == BLANK) continue;
        EffectCtx ctx{i, g_digits, lit[i], t_ms, base};
        setNumeral(i, lit[i], effectColor(effect, ctx));
    }
    show(brightness_pct);
}
