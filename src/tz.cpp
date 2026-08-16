#include "tz.h"

#include <ctype.h>
#include <string.h>

// ------------------------------------------------------------- calendar ----
// Howard Hinnant's civil-calendar algorithms (public domain). Exact for any
// year, and far cheaper than pulling in a full time library.

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void civilFromDays(int32_t z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t yy = (int32_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yy + (m <= 2);
}

// 1970-01-01 was a Thursday.
int dowFromDays(int32_t days) { return (int)(((days % 7) + 11) % 7); }

int dayOfYear(int y, unsigned m, unsigned d) {
    return (int)(daysFromCivil(y, m, d) - daysFromCivil(y, 1, 1)) + 1;
}

// ------------------------------------------------------- POSIX TZ parsing --
// Grammar: std offset [dst [offset] [,start[/time],end[/time]]]
// Offsets are POSIX-signed: positive means WEST of UTC, so EST5EDT is +5.

static const char *parseAbbr(const char *s, char *out, size_t n) {
    size_t i = 0;
    if (*s == '<') {                       // <-03> style numeric zone names
        s++;
        while (*s && *s != '>' && i < n - 1) out[i++] = *s++;
        if (*s == '>') s++;
    } else {
        while (isalpha((unsigned char)*s) && i < n - 1) out[i++] = *s++;
    }
    out[i] = 0;
    return s;
}

static const char *parseOffset(const char *s, int32_t &out, bool &found) {
    found = false;
    int sign = 1;
    const char *p = s;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) return s;

    int32_t h = 0, m = 0, sec = 0;
    while (isdigit((unsigned char)*p)) h = h * 10 + (*p++ - '0');
    if (*p == ':') { p++; while (isdigit((unsigned char)*p)) m = m * 10 + (*p++ - '0'); }
    if (*p == ':') { p++; while (isdigit((unsigned char)*p)) sec = sec * 10 + (*p++ - '0'); }

    out = sign * (h * 3600 + m * 60 + sec);
    found = true;
    return p;
}

static const char *parseRule(const char *s, TzRule &r) {
    r = TzRule{};
    r.time_sec = 2 * 3600;                 // POSIX default transition time

    if (*s == 'M') {
        s++;
        r.type = 1;
        while (isdigit((unsigned char)*s)) r.month = r.month * 10 + (*s++ - '0');
        if (*s == '.') s++;
        while (isdigit((unsigned char)*s)) r.week = r.week * 10 + (*s++ - '0');
        if (*s == '.') s++;
        while (isdigit((unsigned char)*s)) r.dow = r.dow * 10 + (*s++ - '0');
        if (r.month < 1 || r.month > 12 || r.week < 1 || r.week > 5 || r.dow > 6)
            r.type = 0;
    } else if (*s == 'J') {
        s++;
        r.type = 2;
        while (isdigit((unsigned char)*s)) r.day = r.day * 10 + (*s++ - '0');
        if (r.day < 1 || r.day > 365) r.type = 0;
    } else if (isdigit((unsigned char)*s)) {
        r.type = 3;
        while (isdigit((unsigned char)*s)) r.day = r.day * 10 + (*s++ - '0');
        if (r.day > 365) r.type = 0;
    } else {
        return s;                          // leaves type 0 = invalid
    }

    if (*s == '/') {
        s++;
        int32_t t = 0; bool got = false;
        s = parseOffset(s, t, got);        // same hh[:mm[:ss]] shape, may be signed
        if (got) r.time_sec = t;
    }
    return s;
}

bool tzParse(const char *str, TzInfo &out) {
    out = TzInfo{};
    if (!str || !*str) return false;

    const char *s = str;
    s = parseAbbr(s, out.std_abbr, sizeof(out.std_abbr));
    if (!out.std_abbr[0]) return false;

    bool got = false;
    s = parseOffset(s, out.std_off, got);
    if (!got) return false;

    if (*s) {
        s = parseAbbr(s, out.dst_abbr, sizeof(out.dst_abbr));
        if (out.dst_abbr[0]) {
            out.has_dst = true;
            int32_t d = 0; bool gotd = false;
            s = parseOffset(s, d, gotd);
            // POSIX default: DST runs one hour ahead, i.e. one hour less west.
            out.dst_off = gotd ? d : out.std_off - 3600;
        }
    }

    if (out.has_dst && *s == ',') {
        s++;
        s = parseRule(s, out.start);
        if (*s == ',') { s++; s = parseRule(s, out.end); }
        // A DST name with no usable rules would strand the clock permanently on
        // one side of the transition; safer to treat the zone as standard-only.
        if (out.start.type == 0 || out.end.type == 0) out.has_dst = false;
    } else if (out.has_dst) {
        out.has_dst = false;               // DST named but no rules given
    }

    out.valid = true;
    return true;
}

// Local-midnight day number on which a rule fires, for a given year.
static int32_t ruleDay(const TzRule &r, int year) {
    if (r.type == 1) {
        int32_t first = daysFromCivil(year, r.month, 1);
        int firstDow = dowFromDays(first);
        int32_t day = first + ((int)r.dow - firstDow + 7) % 7 + ((int)r.week - 1) * 7;
        if (r.week == 5) {                 // "last" -- step back if we overshot
            int y2; unsigned m2, d2;
            civilFromDays(day, y2, m2, d2);
            while (m2 != r.month) { day -= 7; civilFromDays(day, y2, m2, d2); }
        }
        return day;
    }
    if (r.type == 2) {                     // Jn: 1..365, Feb 29 never counted
        int32_t day = daysFromCivil(year, 1, 1) + (int32_t)r.day - 1;
        if (isLeap(year) && r.day >= 60) day += 1;
        return day;
    }
    return daysFromCivil(year, 1, 1) + (int32_t)r.day;   // n: 0-based, counts Feb 29
}

// The transition instant is expressed in whichever offset is in effect just
// before it: standard time entering DST, DST time leaving it.
static int64_t ruleUtc(const TzRule &r, int year, int32_t offset_in_effect) {
    return (int64_t)ruleDay(r, year) * 86400 + r.time_sec + offset_in_effect;
}

// Floor division, so pre-1970 instants don't round toward zero and land a day off.
static int32_t floorDiv(int64_t a, int32_t b) {
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return (int32_t)q;
}

int32_t tzOffsetFor(const TzInfo &tz, int64_t utc, bool *is_dst) {
    if (is_dst) *is_dst = false;
    if (!tz.valid) return 0;
    if (!tz.has_dst) return tz.std_off;

    int y; unsigned m, d;
    civilFromDays(floorDiv(utc, 86400), y, m, d);

    int64_t start = ruleUtc(tz.start, y, tz.std_off);
    int64_t end   = ruleUtc(tz.end,   y, tz.dst_off);

    bool dst;
    if (start <= end) {
        dst = (utc >= start && utc < end);           // northern hemisphere
    } else {
        dst = (utc >= start || utc < end);           // southern: DST spans New Year
    }

    if (is_dst) *is_dst = dst;
    return dst ? tz.dst_off : tz.std_off;
}

LocalTime localFromUtc(const TzInfo &tz, int64_t utc) {
    bool dst = false;
    int32_t off = tzOffsetFor(tz, utc, &dst);
    int64_t local = utc - off;                       // POSIX offsets are west-positive

    int32_t days = floorDiv(local, 86400);
    int32_t rem = (int32_t)(local - (int64_t)days * 86400);

    LocalTime lt{};
    int y; unsigned m, d;
    civilFromDays(days, y, m, d);
    lt.year = y; lt.month = (uint8_t)m; lt.day = (uint8_t)d;
    lt.hour = (uint8_t)(rem / 3600);
    lt.minute = (uint8_t)((rem % 3600) / 60);
    lt.second = (uint8_t)(rem % 60);
    lt.dow = (uint8_t)dowFromDays(days);
    lt.doy = (uint16_t)dayOfYear(y, m, d);
    lt.is_dst = dst;
    lt.abbr = dst ? tz.dst_abbr : tz.std_abbr;
    return lt;
}
