#include "solar.h"

#include <math.h>

#include "tz.h"

// NOAA solar-position algorithm.
//
// Written in double precision on purpose. The series carries terms like
// 36000.76983 * T; by 2026 that product is around 9600 degrees, and float's
// ~7 significant digits leave roughly 0.001 degrees of slop there -- about a
// quarter of a minute of time before anything else goes wrong. This runs once
// or twice a day, so the softfloat doubles cost nothing that matters.

namespace {

const double DEG = M_PI / 180.0;

// Official sunrise/sunset: geometric zenith 90 degrees plus 50 arc-minutes for
// refraction and the solar semi-diameter.
const double ZENITH = 90.833;

double norm360(double d) {
    d = fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}

struct SolarDay {
    double eqTime;      // minutes
    double declDeg;
};

SolarDay solarDay(double jd) {
    const double T = (jd - 2451545.0) / 36525.0;

    const double L0 = norm360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);

    const double C = sin(M * DEG) * (1.914602 - T * (0.004817 + 0.000014 * T))
                   + sin(2 * M * DEG) * (0.019993 - 0.000101 * T)
                   + sin(3 * M * DEG) * 0.000289;

    const double trueLong = L0 + C;
    const double omega = 125.04 - 1934.136 * T;
    const double appLong = trueLong - 0.00569 - 0.00478 * sin(omega * DEG);

    const double meanObliq = 23.0 + (26.0 + ((21.448 - T * (46.815 + T * (0.00059
                             - T * 0.001813)))) / 60.0) / 60.0;
    const double obliqCorr = meanObliq + 0.00256 * cos(omega * DEG);

    SolarDay out;
    out.declDeg = asin(sin(obliqCorr * DEG) * sin(appLong * DEG)) / DEG;

    const double y = tan(obliqCorr / 2 * DEG) * tan(obliqCorr / 2 * DEG);
    out.eqTime = 4.0 / DEG * (y * sin(2 * L0 * DEG)
                            - 2 * e * sin(M * DEG)
                            + 4 * e * y * sin(M * DEG) * cos(2 * L0 * DEG)
                            - 0.5 * y * y * sin(4 * L0 * DEG)
                            - 1.25 * e * e * sin(2 * M * DEG));
    return out;
}

enum EventKind { RISE, SET };

// Solve for one event, refining the sun's position at the event time.
//
// A single evaluation at noon is not good enough. Declination and the equation
// of time both move over a day, and sunrise can be eight hours from noon at high
// latitude, so noon's values are simply the wrong ones to use. Measured against
// an independent reference, one-shot-at-noon was 1250 s out at Tromso and 447 s
// at Reykjavik; iterating collapses that to seconds. Two passes converge --
// three is belt and braces and still costs nothing at once-a-day.
double solveEvent(double lat, double lon, int32_t days, EventKind kind,
                  bool &ok, bool &alwaysUp) {
    ok = false;
    alwaysUp = false;

    const double latR = lat * DEG;
    double minutesPastMidnight = 720.0;      // start the search at noon UT

    for (int iter = 0; iter < 3; iter++) {
        const double jd = 2440587.5 + (double)days + minutesPastMidnight / 1440.0;
        const SolarDay sd = solarDay(jd);
        const double declR = sd.declDeg * DEG;

        const double cosH = (cos(ZENITH * DEG) - sin(latR) * sin(declR))
                          / (cos(latR) * cos(declR));
        if (cosH > 1.0)  { alwaysUp = false; return 0.0; }   // polar night
        if (cosH < -1.0) { alwaysUp = true;  return 0.0; }   // midnight sun

        const double H = acos(cosH) / DEG;
        const double noonMin = 720.0 - 4.0 * lon - sd.eqTime;
        minutesPastMidnight = (kind == RISE) ? noonMin - 4.0 * H : noonMin + 4.0 * H;
    }

    ok = true;
    return minutesPastMidnight;
}

}  // namespace

SunTimes sunTimesUtcDate(double lat, double lon, int y, unsigned m, unsigned d) {
    SunTimes out{0, 0, false, false};

    const int32_t days = daysFromCivil(y, m, d);
    const int64_t midnightUtc = (int64_t)days * 86400;

    bool riseOk, setOk, upR, upS;
    const double rise = solveEvent(lat, lon, days, RISE, riseOk, upR);
    const double set = solveEvent(lat, lon, days, SET, setOk, upS);

    if (!riseOk || !setOk) {
        out.valid = false;
        out.always_up = upR || upS;
        return out;
    }

    out.rise_utc = midnightUtc + (int64_t)llround(rise * 60.0);
    out.set_utc = midnightUtc + (int64_t)llround(set * 60.0);
    out.valid = true;
    return out;
}

SunTimes sunTimesForLocalDate(double lat, double lon, int y, unsigned m, unsigned d,
                              int32_t utc_offset_west) {
    // The solar day to use is whichever UTC date contains this local day's noon.
    const int64_t localNoonUtc =
        (int64_t)daysFromCivil(y, m, d) * 86400 + 12 * 3600 + utc_offset_west;

    int32_t udays = (int32_t)(localNoonUtc / 86400);
    if (localNoonUtc < 0 && localNoonUtc % 86400 != 0) udays--;

    int uy; unsigned um, ud;
    civilFromDays(udays, uy, um, ud);
    return sunTimesUtcDate(lat, lon, uy, um, ud);
}
