#pragma once
#include <stdint.h>
#include <stddef.h>

// Calendar and POSIX-timezone math. Deliberately free of any Particle
// dependency so it can be compiled and tested on the host -- see tests/.
//
// There is no timezone database on the device. The config page derives a POSIX
// TZ rule in the browser (from Intl, which already ships the world's zone data)
// and the clock stores those ~48 bytes and interprets them here.

struct TzRule {
    uint8_t type;      // 0 unused, 1 = Mm.w.d, 2 = Jn (skips Feb 29), 3 = n (counts it)
    uint8_t month;     // 1..12          (type 1)
    uint8_t week;      // 1..5, 5 = last (type 1)
    uint8_t dow;       // 0 = Sunday     (type 1)
    uint16_t day;      // type 2 and 3
    int32_t time_sec;  // seconds after local midnight, default 02:00
};

struct TzInfo {
    char std_abbr[8];
    char dst_abbr[8];
    int32_t std_off;   // seconds WEST of UTC, per POSIX (EST5EDT -> +18000)
    int32_t dst_off;
    bool has_dst;
    TzRule start, end;
    bool valid;
};

struct LocalTime {
    int year;
    uint8_t month, day, hour, minute, second;
    uint8_t dow;       // 0 = Sunday
    uint16_t doy;      // 1-based day of year
    bool is_dst;
    const char *abbr;
};

bool isLeap(int y);
int32_t daysFromCivil(int y, unsigned m, unsigned d);
void civilFromDays(int32_t z, int &y, unsigned &m, unsigned &d);
int dowFromDays(int32_t days);
int dayOfYear(int y, unsigned m, unsigned d);

bool tzParse(const char *s, TzInfo &out);
int32_t tzOffsetFor(const TzInfo &tz, int64_t utc, bool *is_dst = nullptr);
LocalTime localFromUtc(const TzInfo &tz, int64_t utc);
