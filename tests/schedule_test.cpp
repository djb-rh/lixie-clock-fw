// Host tests for schedule evaluation.
//
// Entries are transition points: at its moment an entry's settings take over and
// hold until the next one fires. The cases that matter are the ones that are
// easy to get subtly wrong -- boot-time catch-up, sparse day masks, sun anchors
// with offsets, and the two nights a year when local time is not a function.

#include "../src/schedule.h"
#include "../src/solar.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

// Durham, in US Eastern time.
static const double LAT = 35.9940, LON = -78.8986;
static const char *TZ = "EST5EDT,M3.2.0,M11.1.0";

static int64_t utcOf(int y, unsigned mo, unsigned d, int h, int mi) {
    return (int64_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60;
}

// Local wall-clock instant, resolved through the timezone.
static int64_t localOf(const TzInfo &tz, int y, unsigned mo, unsigned d, int h, int mi) {
    int64_t guess = utcOf(y, mo, d, h, mi);
    return guess + tzOffsetFor(tz, guess + 12 * 3600);
}

static ScheduleEntry clockEntry(uint8_t days, int minutes, uint8_t bright,
                                uint8_t r = 255, uint8_t g = 136, uint8_t b = 0) {
    ScheduleEntry e{};
    e.enabled = 1; e.days_mask = days; e.anchor = ANCHOR_CLOCK;
    e.minutes = (int16_t)minutes; e.mode = MODE_SOLID;
    e.r = r; e.g = g; e.b = b; e.brightness = bright;
    return e;
}

static ScheduleEntry sunEntry(uint8_t days, uint8_t anchor, int offset, uint8_t bright) {
    ScheduleEntry e{};
    e.enabled = 1; e.days_mask = days; e.anchor = anchor;
    e.minutes = (int16_t)offset; e.mode = MODE_SOLID;
    e.r = 255; e.g = 136; e.b = 0; e.brightness = bright;
    return e;
}

static const uint8_t ALL_DAYS = 0x7F;

int main() {
    char buf[220];
    TzInfo tz;
    tzParse(TZ, tz);

    printf("--- empty schedule ---\n");
    Resolved r = scheduleEvaluate(nullptr, 0, tz, LAT, LON, utcOf(2026, 8, 16, 18, 0));
    check(!r.active && r.entry == -1, "no entries means nothing applies");

    printf("\n--- basic transitions ---\n");
    // 07:00 bright, 21:30 dim, every day.
    ScheduleEntry daily[2] = {
        clockEntry(ALL_DAYS, 7 * 60, 90),
        clockEntry(ALL_DAYS, 21 * 60 + 30, 15),
    };

    r = scheduleEvaluate(daily, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 12, 0));
    snprintf(buf, sizeof(buf), "midday resolves to the 07:00 entry (brightness %u)", r.brightness);
    check(r.active && r.entry == 0 && r.brightness == 90, buf);

    r = scheduleEvaluate(daily, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 23, 0));
    snprintf(buf, sizeof(buf), "late evening resolves to the 21:30 entry (brightness %u)", r.brightness);
    check(r.active && r.entry == 1 && r.brightness == 15, buf);

    printf("\n--- boot-time catch-up ---\n");
    // The whole point: a clock rebooting at 03:00 must resume the overnight
    // setting, not sit on defaults until 07:00.
    r = scheduleEvaluate(daily, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 3, 0));
    snprintf(buf, sizeof(buf), "03:00 picks up YESTERDAY's 21:30 entry (brightness %u)", r.brightness);
    check(r.active && r.entry == 1 && r.brightness == 15, buf);

    int64_t since = r.since_utc;
    LocalTime lt = localFromUtc(tz, since);
    snprintf(buf, sizeof(buf), "…and reports it fired at %02d-%02d %02d:%02d local",
             lt.month, lt.day, lt.hour, lt.minute);
    check(lt.day == 15 && lt.hour == 21 && lt.minute == 30, buf);

    printf("\n--- sparse day masks ---\n");
    // Weekends only, 09:00. On a Wednesday it must reach back to Sunday.
    const uint8_t SUN = 1 << 0, SAT = 1 << 6;
    ScheduleEntry weekend[1] = { clockEntry((uint8_t)(SUN | SAT), 9 * 60, 70) };

    // 2026-08-19 is a Wednesday; the previous Sunday is the 16th.
    r = scheduleEvaluate(weekend, 1, tz, LAT, LON, localOf(tz, 2026, 8, 19, 12, 0));
    lt = localFromUtc(tz, r.since_utc);
    snprintf(buf, sizeof(buf), "Wednesday reaches back to Sunday the %d", lt.day);
    check(r.active && lt.day == 16 && lt.dow == 0, buf);

    // A Monday-only entry seen the following Sunday is six days old.
    ScheduleEntry monday[1] = { clockEntry(1 << 1, 8 * 60, 50) };
    r = scheduleEvaluate(monday, 1, tz, LAT, LON, localOf(tz, 2026, 8, 16, 12, 0));
    lt = localFromUtc(tz, r.since_utc);
    snprintf(buf, sizeof(buf), "Monday-only entry still resolves six days later (fired the %d)", lt.day);
    check(r.active && lt.dow == 1, buf);

    printf("\n--- sun anchors ---\n");
    // Sunset-anchored, 30 minutes before.
    ScheduleEntry sunset30[1] = { sunEntry(ALL_DAYS, ANCHOR_SUNSET, -30, 20) };

    SunTimes s = sunTimesForLocalDate(LAT, LON, 2026, 8, 16, 4 * 3600);
    int64_t want = s.set_utc - 30 * 60;

    // Just after it should fire: it applies.
    r = scheduleEvaluate(sunset30, 1, tz, LAT, LON, want + 60);
    snprintf(buf, sizeof(buf), "fires within a minute of sunset-30 (off by %llds)",
             (long long)(r.since_utc - want));
    check(r.active && llabs(r.since_utc - want) <= 60, buf);

    lt = localFromUtc(tz, r.since_utc);
    LocalTime ls = localFromUtc(tz, s.set_utc);
    snprintf(buf, sizeof(buf), "sunset %02d:%02d local, entry fires %02d:%02d local",
             ls.hour, ls.minute, lt.hour, lt.minute);
    check(lt.hour * 60 + lt.minute == ls.hour * 60 + ls.minute - 30, buf);

    // Just before it fires, the previous day's instance is what applies.
    r = scheduleEvaluate(sunset30, 1, tz, LAT, LON, want - 60);
    snprintf(buf, sizeof(buf), "one minute earlier, yesterday's instance applies (%llds back)",
             (long long)(want - 60 - r.since_utc));
    check(r.active && r.since_utc < want - 60 && (want - r.since_utc) < 90000, buf);

    printf("\n--- sunrise and sunset in one schedule ---\n");
    ScheduleEntry both[2] = {
        sunEntry(ALL_DAYS, ANCHOR_SUNRISE, 0, 80),
        sunEntry(ALL_DAYS, ANCHOR_SUNSET, 0, 10),
    };
    r = scheduleEvaluate(both, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 12, 0));
    check(r.active && r.entry == 0 && r.brightness == 80, "midday is after sunrise");
    r = scheduleEvaluate(both, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 23, 0));
    check(r.active && r.entry == 1 && r.brightness == 10, "late evening is after sunset");
    r = scheduleEvaluate(both, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 4, 0));
    check(r.active && r.entry == 1 && r.brightness == 10,
          "pre-dawn is still yesterday's sunset");

    printf("\n--- offsets that cross midnight ---\n");
    // Sunset + 6 h lands after midnight, so the entry "for" the 16th fires on
    // the 17th. Scanning only backwards by calendar day would miss it.
    ScheduleEntry late[1] = { sunEntry(ALL_DAYS, ANCHOR_SUNSET, 6 * 60, 5) };
    int64_t after = s.set_utc + 6 * 3600 + 60;
    r = scheduleEvaluate(late, 1, tz, LAT, LON, after);
    snprintf(buf, sizeof(buf), "sunset+6h resolves correctly across midnight (off by %llds)",
             (long long)(r.since_utc - (s.set_utc + 6 * 3600)));
    check(r.active && llabs(r.since_utc - (s.set_utc + 6 * 3600)) <= 60, buf);

    printf("\n--- DST boundaries ---\n");
    // Spring forward 2026-03-08: 02:00 does not exist. An 02:30 entry must still
    // resolve to something sane rather than vanishing or firing twice.
    ScheduleEntry spring[1] = { clockEntry(ALL_DAYS, 2 * 60 + 30, 40) };
    r = scheduleEvaluate(spring, 1, tz, LAT, LON, utcOf(2026, 3, 8, 12, 0));
    check(r.active, "an 02:30 entry still resolves on spring-forward day");
    lt = localFromUtc(tz, r.since_utc);
    snprintf(buf, sizeof(buf), "…resolving to %02d-%02d %02d:%02d local",
             lt.month, lt.day, lt.hour, lt.minute);
    check(lt.month == 3 && lt.day == 8, buf);

    // Fall back 2026-11-01: 01:30 happens twice. It must fire, once.
    ScheduleEntry fall[1] = { clockEntry(ALL_DAYS, 1 * 60 + 30, 40) };
    r = scheduleEvaluate(fall, 1, tz, LAT, LON, utcOf(2026, 11, 1, 12, 0));
    lt = localFromUtc(tz, r.since_utc);
    snprintf(buf, sizeof(buf), "a 01:30 entry on fall-back day resolves to %02d-%02d %02d:%02d",
             lt.month, lt.day, lt.hour, lt.minute);
    check(r.active && lt.month == 11 && lt.day == 1, buf);

    printf("\n--- polar latitudes ---\n");
    // Tromso in midwinter has no sunrise, so sun anchors cannot fire. A clock
    // anchor on the same schedule must still work.
    TzInfo ntz;
    tzParse("CET-1CEST,M3.5.0,M10.5.0/3", ntz);
    ScheduleEntry mixed[2] = {
        sunEntry(ALL_DAYS, ANCHOR_SUNRISE, 0, 80),
        clockEntry(ALL_DAYS, 16 * 60, 25),
    };
    r = scheduleEvaluate(mixed, 2, ntz, 69.6492, 18.9553, utcOf(2026, 12, 21, 18, 0));
    check(r.active && r.entry == 1 && r.brightness == 25,
          "polar night: sun anchor cannot fire, clock anchor still does");

    r = scheduleEvaluate(mixed, 1, ntz, 69.6492, 18.9553, utcOf(2026, 12, 21, 18, 0));
    check(!r.active, "polar night with only sun anchors resolves to nothing");

    printf("\n--- next fire ---\n");
    int64_t next = scheduleNextFire(daily, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 12, 0));
    lt = localFromUtc(tz, next);
    snprintf(buf, sizeof(buf), "next change after midday is %02d:%02d", lt.hour, lt.minute);
    check(next != 0 && lt.hour == 21 && lt.minute == 30, buf);

    next = scheduleNextFire(daily, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 23, 0));
    lt = localFromUtc(tz, next);
    snprintf(buf, sizeof(buf), "next change after 23:00 is tomorrow %02d:%02d on the %d",
             lt.hour, lt.minute, lt.day);
    check(next != 0 && lt.hour == 7 && lt.day == 17, buf);

    check(scheduleNextFire(nullptr, 0, tz, LAT, LON, utcOf(2026, 8, 16, 12, 0)) == 0,
          "no entries means no next fire");

    printf("\n--- disabled entries are ignored ---\n");
    ScheduleEntry off[2] = { clockEntry(ALL_DAYS, 7 * 60, 90), clockEntry(ALL_DAYS, 12 * 60, 33) };
    off[1].enabled = 0;
    r = scheduleEvaluate(off, 2, tz, LAT, LON, localOf(tz, 2026, 8, 16, 18, 0));
    check(r.active && r.entry == 0, "a disabled entry never applies");

    printf("\n--- a full schedule resolves ---\n");
    ScheduleEntry full[MAX_SCHEDULE];
    for (uint8_t i = 0; i < MAX_SCHEDULE; i++)
        full[i] = clockEntry(ALL_DAYS, i * 60, (uint8_t)(i + 1));
    r = scheduleEvaluate(full, MAX_SCHEDULE, tz, LAT, LON, localOf(tz, 2026, 8, 16, 13, 30));
    snprintf(buf, sizeof(buf), "24 hourly entries: 13:30 picks the 13:00 one (brightness %u)",
             r.brightness);
    check(r.active && r.brightness == 14, buf);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
