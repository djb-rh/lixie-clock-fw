// Validates the NOAA solar implementation against an independent reference.
//
// Vectors come from api.sunrise-sunset.org via tools/gen_solar_vectors.sh --
// a separate implementation by separate people. Checking our maths against our
// own maths would prove nothing.
//
// The Phase 0 spike carried a low-precision approximation, and the note in
// docs/phase0-results.md put it at "3-4 minutes off" based on almanac figures I
// recalled rather than a real source. This test measures both, so the claim is
// backed by numbers instead of memory.

#include "../src/solar.h"
#include "../src/tz.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

// The Phase 0 approximation, kept here purely for comparison.
static void legacyApprox(double lat, double lon, int doy,
                         double &riseMin, double &setMin, bool &ok) {
    const double RAD = M_PI / 180.0;
    double g = 357.529 + 0.98560028 * (doy - 1);
    double lambda = 280.459 + 0.98564736 * (doy - 1) + 1.915 * sin(g * RAD);
    double decl = asin(sin(23.44 * RAD) * sin(lambda * RAD)) / RAD;
    double B = (doy - 81) * 2.0 * M_PI / 364.0;
    double eot = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);
    double cosH = cos(90.833 * RAD) / (cos(lat * RAD) * cos(decl * RAD))
                - tan(lat * RAD) * tan(decl * RAD);
    if (cosH > 1.0 || cosH < -1.0) { ok = false; return; }
    double H = acos(cosH) / RAD;
    double noon = 720.0 - 4.0 * lon - eot;
    riseMin = noon - 4.0 * H;
    setMin = noon + 4.0 * H;
    ok = true;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "solar_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("SKIP no %s -- run: ../tools/gen_solar_vectors.sh > %s\n", path, path);
        return 0;
    }

    char line[512], buf[220];
    int n = 0;
    int worstNoaa = 0, worstLegacy = 0;
    char worstNoaaWhere[128] = "", worstLegacyWhere[128] = "";
    long sumNoaa = 0, sumLegacy = 0;
    int over60 = 0, over120 = 0;

    printf("--- per-location worst error against the reference ---\n");
    char curName[64] = "";
    int locWorst = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char *name = strtok(line, "\t");
        double lat = atof(strtok(nullptr, "\t"));
        double lon = atof(strtok(nullptr, "\t"));
        char *date = strtok(nullptr, "\t");
        long wantRise = atol(strtok(nullptr, "\t"));
        long wantSet = atol(strtok(nullptr, "\t\n"));
        if (!name || !date) continue;

        int y; unsigned mo, d;
        sscanf(date, "%d-%u-%u", &y, &mo, &d);

        if (strcmp(curName, name)) {
            if (curName[0]) {
                snprintf(buf, sizeof(buf), "%-11s worst error %3d s", curName, locWorst);
                check(locWorst <= 120, buf);
            }
            snprintf(curName, sizeof(curName), "%s", name);
            locWorst = 0;
        }

        SunTimes s = sunTimesUtcDate(lat, lon, y, mo, d);
        if (!s.valid) {
            snprintf(buf, sizeof(buf), "%s %s: reported no sunrise but reference has one",
                     name, date);
            check(false, buf);
            continue;
        }

        int dr = (int)labs(s.rise_utc - wantRise);
        int ds = (int)labs(s.set_utc - wantSet);
        int worst = dr > ds ? dr : ds;
        if (worst > locWorst) locWorst = worst;
        sumNoaa += dr + ds;
        if (worst > 60) over60++;
        if (worst > 120) over120++;
        if (worst > worstNoaa) {
            worstNoaa = worst;
            snprintf(worstNoaaWhere, sizeof(worstNoaaWhere), "%s %s", name, date);
        }

        // Same comparison for the old approximation.
        double lr, ls; bool lok = false;
        legacyApprox(lat, lon, dayOfYear(y, mo, d), lr, ls, lok);
        if (lok) {
            int64_t mid = (int64_t)daysFromCivil(y, mo, d) * 86400;
            int ldr = (int)labs((long)(mid + (int64_t)(lr * 60)) - wantRise);
            int lds = (int)labs((long)(mid + (int64_t)(ls * 60)) - wantSet);
            int lworst = ldr > lds ? ldr : lds;
            sumLegacy += ldr + lds;
            if (lworst > worstLegacy) {
                worstLegacy = lworst;
                snprintf(worstLegacyWhere, sizeof(worstLegacyWhere), "%s %s", name, date);
            }
        }
        n++;
    }
    fclose(f);

    if (curName[0]) {
        snprintf(buf, sizeof(buf), "%-11s worst error %3d s", curName, locWorst);
        check(locWorst <= 120, buf);
    }

    printf("\n--- overall ---\n");
    printf("     %d sample days, %d rise/set instants\n", n, n * 2);
    printf("     NOAA   : worst %3d s (%s), mean %.1f s\n",
           worstNoaa, worstNoaaWhere, n ? (double)sumNoaa / (n * 2) : 0.0);
    printf("     legacy : worst %3d s (%s), mean %.1f s\n",
           worstLegacy, worstLegacyWhere, n ? (double)sumLegacy / (n * 2) : 0.0);

    snprintf(buf, sizeof(buf), "every instant within 2 minutes (%d of %d exceeded 60 s, "
             "%d exceeded 120 s)", over60, n * 2, over120);
    check(over120 == 0, buf);

    snprintf(buf, sizeof(buf), "worst case %d s is inside the 2-minute bar", worstNoaa);
    check(worstNoaa <= 120, buf);

    printf("\n--- polar cases ---\n");
    // Tromso, 69.65 N: midnight sun in June, polar night in December.
    SunTimes june = sunTimesUtcDate(69.6492, 18.9553, 2026, 6, 21);
    check(!june.valid && june.always_up, "Tromso midsummer: sun never sets");
    SunTimes dec = sunTimesUtcDate(69.6492, 18.9553, 2026, 12, 21);
    check(!dec.valid && !dec.always_up, "Tromso midwinter: sun never rises");

    printf("\n--- local-date mapping ---\n");
    // Durham's mid-August sunset lands after midnight UTC, which is exactly the
    // rollover the epoch-based API exists to keep callers from fumbling.
    SunTimes aug = sunTimesForLocalDate(35.9940, -78.8986, 2026, 8, 16, 4 * 3600);
    int64_t setDay = aug.set_utc / 86400;
    int64_t riseDay = aug.rise_utc / 86400;
    snprintf(buf, sizeof(buf), "sunset falls on the next UTC day (rise day %lld, set day %lld)",
             (long long)riseDay, (long long)setDay);
    check(aug.valid && setDay == riseDay + 1, buf);

    TzInfo tz;
    tzParse("EST5EDT,M3.2.0,M11.1.0", tz);
    LocalTime ls = localFromUtc(tz, aug.set_utc);
    snprintf(buf, sizeof(buf), "…but reads as %02d:%02d local on the 16th",
             ls.hour, ls.minute);
    check(ls.day == 16 && ls.hour == 20, buf);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
