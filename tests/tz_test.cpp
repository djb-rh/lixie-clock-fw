// Host tests for the calendar and POSIX-timezone math.
//
//   cd tests && make && ./tz_test
//
// This code runs on the device but has no Particle dependency, so the DST
// transitions can be checked here in a second instead of by flashing a clock
// and waiting for March.

#include "../src/tz.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int64_t utcOf(int y, unsigned mo, unsigned d, int h, int mi, int s) {
    return (int64_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

// Assert the offset immediately before and after an instant, which is what a
// DST transition actually is.
static void checkTransition(const char *zone, const char *label,
                            int64_t at, int32_t before, int32_t after) {
    TzInfo tz;
    char buf[160];
    if (!tzParse(zone, tz)) {
        snprintf(buf, sizeof(buf), "%s: parse", zone);
        check(false, buf);
        return;
    }
    int32_t got_before = tzOffsetFor(tz, at - 1);
    int32_t got_after = tzOffsetFor(tz, at);
    snprintf(buf, sizeof(buf), "%s %s: %+d -> %+d (got %+d -> %+d)",
             zone, label, -before / 3600, -after / 3600,
             -got_before / 3600, -got_after / 3600);
    check(got_before == before && got_after == after, buf);
}

int main() {
    char buf[160];

    printf("--- calendar ---\n");
    check(daysFromCivil(1970, 1, 1) == 0, "epoch is day 0");
    check(dowFromDays(0) == 4, "1970-01-01 is a Thursday");
    check(dowFromDays(daysFromCivil(2026, 3, 8)) == 0, "2026-03-08 is a Sunday");
    check(dayOfYear(2026, 8, 16) == 228, "2026-08-16 is day 228");
    check(dayOfYear(2024, 3, 1) == 61, "leap year: 2024-03-01 is day 61");
    check(dayOfYear(2026, 3, 1) == 60, "common year: 2026-03-01 is day 60");
    check(isLeap(2024) && !isLeap(2026) && !isLeap(1900) && isLeap(2000), "leap rules");

    // Round-trip a few thousand days through both conversions.
    bool roundtrip = true;
    for (int32_t z = -20000; z < 40000; z += 7) {
        int y; unsigned m, d;
        civilFromDays(z, y, m, d);
        if (daysFromCivil(y, m, d) != z) { roundtrip = false; break; }
    }
    check(roundtrip, "civil<->days round-trip over 1915-2079");

    printf("\n--- parsing ---\n");
    TzInfo tz;
    check(tzParse("EST5EDT,M3.2.0,M11.1.0/2", tz) && tz.has_dst &&
          tz.std_off == 18000 && tz.dst_off == 14400 &&
          !strcmp(tz.std_abbr, "EST") && !strcmp(tz.dst_abbr, "EDT"),
          "EST5EDT,M3.2.0,M11.1.0/2");
    check(tzParse("MST7", tz) && !tz.has_dst && tz.std_off == 25200, "MST7 (no DST)");
    check(tzParse("IST-5:30", tz) && tz.std_off == -19800, "IST-5:30 (half-hour, east)");
    check(tzParse("<-03>3", tz) && tz.std_off == 10800 && !strcmp(tz.std_abbr, "-03"),
          "<-03>3 (numeric zone name)");
    check(tzParse("AEST-10AEDT,M10.1.0,M4.1.0/3", tz) && tz.std_off == -36000 &&
          tz.dst_off == -39600, "AEST-10AEDT (southern hemisphere)");
    check(tzParse("CET-1CEST,M3.5.0,M10.5.0/3", tz) && tz.dst_off == -7200,
          "CET-1CEST (implicit 1 h DST offset)");
    check(!tzParse("", tz), "empty string rejected");
    check(!tzParse("GARBAGE", tz), "offset-less string rejected");
    check(tzParse("EST5EDT", tz) && !tz.has_dst, "DST named but ruleless -> standard only");

    printf("\n--- DST transitions (2026) ---\n");
    // America/New_York: 2nd Sunday March 02:00 EST, 1st Sunday November 02:00 EDT
    checkTransition("EST5EDT,M3.2.0,M11.1.0/2", "spring forward",
                    utcOf(2026, 3, 8, 7, 0, 0), 18000, 14400);
    checkTransition("EST5EDT,M3.2.0,M11.1.0/2", "fall back",
                    utcOf(2026, 11, 1, 6, 0, 0), 14400, 18000);

    // Europe/London: last Sunday March 01:00 UTC, last Sunday October 01:00 UTC
    checkTransition("GMT0BST,M3.5.0/1,M10.5.0/2", "spring forward",
                    utcOf(2026, 3, 29, 1, 0, 0), 0, -3600);
    checkTransition("GMT0BST,M3.5.0/1,M10.5.0/2", "fall back",
                    utcOf(2026, 10, 25, 1, 0, 0), -3600, 0);

    // Australia/Sydney: DST starts 1st Sunday October, ends 1st Sunday April.
    checkTransition("AEST-10AEDT,M10.1.0,M4.1.0/3", "DST starts",
                    utcOf(2026, 10, 3, 16, 0, 0), -36000, -39600);
    checkTransition("AEST-10AEDT,M10.1.0,M4.1.0/3", "DST ends",
                    utcOf(2026, 4, 4, 16, 0, 0), -39600, -36000);

    printf("\n--- southern hemisphere spans New Year ---\n");
    tzParse("AEST-10AEDT,M10.1.0,M4.1.0/3", tz);
    bool jan_dst = false, jul_dst = false;
    tzOffsetFor(tz, utcOf(2026, 1, 15, 0, 0, 0), &jan_dst);
    tzOffsetFor(tz, utcOf(2026, 7, 15, 0, 0, 0), &jul_dst);
    check(jan_dst && !jul_dst, "Sydney: DST in January, standard in July");

    tzParse("EST5EDT,M3.2.0,M11.1.0/2", tz);
    bool ny_jan = false, ny_jul = false;
    tzOffsetFor(tz, utcOf(2026, 1, 15, 0, 0, 0), &ny_jan);
    tzOffsetFor(tz, utcOf(2026, 7, 15, 0, 0, 0), &ny_jul);
    check(!ny_jan && ny_jul, "New York: standard in January, DST in July");

    printf("\n--- local time rendering ---\n");
    tzParse("EST5EDT,M3.2.0,M11.1.0/2", tz);

    // 2026-08-16 18:25 UTC is 14:25 EDT.
    LocalTime lt = localFromUtc(tz, utcOf(2026, 8, 16, 18, 25, 30));
    snprintf(buf, sizeof(buf), "18:25:30 UTC -> %04d-%02d-%02d %02d:%02d:%02d %s",
             lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second, lt.abbr);
    check(lt.year == 2026 && lt.month == 8 && lt.day == 16 && lt.hour == 14 &&
          lt.minute == 25 && lt.second == 30 && lt.is_dst && !strcmp(lt.abbr, "EDT"), buf);

    // Crossing back over midnight: 03:30 UTC is 23:30 the previous evening.
    lt = localFromUtc(tz, utcOf(2026, 8, 16, 3, 30, 0));
    snprintf(buf, sizeof(buf), "03:30 UTC -> %04d-%02d-%02d %02d:%02d (day rolls back)",
             lt.year, lt.month, lt.day, lt.hour, lt.minute);
    check(lt.day == 15 && lt.hour == 23 && lt.minute == 30, buf);

    // The repeated hour in the fall: 05:30 UTC is still EDT, 06:30 is EST --
    // both render as 01:30 local, which is correct and unavoidable.
    LocalTime a = localFromUtc(tz, utcOf(2026, 11, 1, 5, 30, 0));
    LocalTime b = localFromUtc(tz, utcOf(2026, 11, 1, 6, 30, 0));
    snprintf(buf, sizeof(buf), "fall-back repeats 01:30 (%02d:%02d %s, then %02d:%02d %s)",
             a.hour, a.minute, a.abbr, b.hour, b.minute, b.abbr);
    check(a.hour == 1 && a.minute == 30 && a.is_dst &&
          b.hour == 1 && b.minute == 30 && !b.is_dst, buf);

    // The skipped hour in the spring: 02:xx local never occurs.
    a = localFromUtc(tz, utcOf(2026, 3, 8, 6, 59, 0));
    b = localFromUtc(tz, utcOf(2026, 3, 8, 7, 0, 0));
    snprintf(buf, sizeof(buf), "spring-forward skips 02:00 (%02d:%02d -> %02d:%02d)",
             a.hour, a.minute, b.hour, b.minute);
    check(a.hour == 1 && a.minute == 59 && b.hour == 3 && b.minute == 0, buf);

    printf("\n--- day-of-week across a year ---\n");
    tzParse("MST7", tz);
    lt = localFromUtc(tz, utcOf(2026, 8, 16, 12, 0, 0));
    check(lt.dow == 0, "2026-08-16 is a Sunday");
    check(lt.doy == 228, "2026-08-16 is day-of-year 228");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
