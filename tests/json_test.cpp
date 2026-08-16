// Host tests for the non-allocating JSON scanner.
//
// This parser handles config POSTs from the web UI and command payloads from
// Home Assistant, so it sees untrusted input from the network. The malformed
// cases below matter as much as the well-formed ones: the requirement is that
// bad input returns "not found", never a crash or a read past the buffer.

#include "../src/json.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static long I(const char *j, const char *k, long def = -12345) {
    long v = def;
    jsonGetInt(j, strlen(j), k, v);
    return v;
}

static const char *S(const char *j, const char *k) {
    static char buf[64];
    buf[0] = 0;
    jsonGetStr(j, strlen(j), k, buf, sizeof(buf));
    return buf;
}

int main() {
    char buf[256];

    printf("--- flat objects ---\n");
    const char *flat =
        "{\"brightness\":42,\"tz\":\"EST5EDT,M3.2.0,M11.1.0/2\",\"on\":true,"
        "\"off\":false,\"lat\":35.994,\"lon\":-78.899,\"nothing\":null}";

    check(I(flat, "brightness") == 42, "int");
    check(!strcmp(S(flat, "tz"), "EST5EDT,M3.2.0,M11.1.0/2"), "string with commas and dots");

    bool b = false;
    check(jsonGetBool(flat, strlen(flat), "on", b) && b, "bool true");
    check(jsonGetBool(flat, strlen(flat), "off", b) && !b, "bool false");

    float f = 0;
    check(jsonGetFloat(flat, strlen(flat), "lat", f) && f > 35.99f && f < 36.0f,
          "float");
    check(jsonGetFloat(flat, strlen(flat), "lon", f) && f < -78.89f && f > -78.91f,
          "negative float");

    check(I(flat, "missing") == -12345, "missing key returns false");
    long scratch = 0;
    check(!jsonGetInt(flat, strlen(flat), "tz", scratch) && scratch == 0,
          "type mismatch rejected and out left untouched");

    JsonVal v;
    check(jsonFind(flat, strlen(flat), "nothing", v) && v.type == JSON_NULL, "null");

    printf("\n--- nested objects (Home Assistant command shape) ---\n");
    const char *ha =
        "{\"state\":\"ON\",\"brightness\":128,"
        "\"color\":{\"r\":255,\"g\":136,\"b\":0},\"effect\":\"Rainbow Flow\"}";

    check(!strcmp(S(ha, "state"), "ON"), "state");
    check(I(ha, "brightness") == 128, "brightness");
    check(I(ha, "color.r") == 255, "dotted path color.r");
    check(I(ha, "color.g") == 136, "dotted path color.g");
    check(I(ha, "color.b") == 0, "dotted path color.b");
    check(!strcmp(S(ha, "effect"), "Rainbow Flow"), "string with a space");
    check(I(ha, "color.missing") == -12345, "missing nested key");
    check(I(ha, "state.r") == -12345, "dotted path into a non-object");

    printf("\n--- keys that are prefixes of each other ---\n");
    const char *pre = "{\"br\":1,\"brightness\":2,\"b\":3}";
    check(I(pre, "b") == 3 && I(pre, "br") == 1 && I(pre, "brightness") == 2,
          "b / br / brightness resolve independently");

    printf("\n--- values containing structural characters ---\n");
    const char *tricky =
        "{\"a\":\"}{,:[]\",\"b\":42,\"c\":\"quote\\\" and backslash\\\\\",\"d\":7}";
    check(I(tricky, "b") == 42, "key after a string full of braces and commas");
    check(I(tricky, "d") == 7, "key after a string with escaped quote");
    check(!strcmp(S(tricky, "a"), "}{,:[]"), "structural chars survive extraction");
    check(!strcmp(S(tricky, "c"), "quote\" and backslash\\"), "escapes resolved");

    printf("\n--- escapes ---\n");
    const char *esc = "{\"s\":\"tab\\there\\nnewline\",\"u\":\"caf\\u0041\"}";
    check(!strcmp(S(esc, "s"), "tab\there\nnewline"), "\\t and \\n");
    check(!strcmp(S(esc, "u"), "cafA"), "\\u ASCII");

    printf("\n--- truncation is bounded ---\n");
    const char *longstr = "{\"s\":\"0123456789abcdefghij\"}";
    char small[8];
    jsonGetStr(longstr, strlen(longstr), "s", small, sizeof(small));
    snprintf(buf, sizeof(buf), "long string truncated to %zu bytes: \"%s\"",
             strlen(small), small);
    check(strlen(small) == 7 && !strcmp(small, "0123456"), buf);

    printf("\n--- arrays (schedule shape) ---\n");
    const char *sched =
        "{\"schedule\":["
        "{\"enabled\":1,\"anchor\":0,\"minutes\":1290,\"brightness\":80},"
        "{\"enabled\":1,\"anchor\":2,\"minutes\":-30,\"brightness\":20},"
        "{\"enabled\":0,\"anchor\":1,\"minutes\":15,\"brightness\":60}]}";

    JsonVal arr;
    check(jsonFind(sched, strlen(sched), "schedule", arr) && arr.type == JSON_ARRAY,
          "find array");
    check(jsonArrayCount(arr.p, arr.len) == 3, "array length 3");

    JsonVal e;
    check(jsonArrayAt(arr.p, arr.len, 1, e) && e.type == JSON_OBJECT, "element 1");
    long m = 0;
    check(jsonGetInt(e.p, e.len, "minutes", m) && m == -30,
          "negative int inside an array element");
    long a = 0;
    check(jsonGetInt(e.p, e.len, "anchor", a) && a == 2, "anchor inside element 1");

    check(jsonArrayAt(arr.p, arr.len, 2, e) && jsonGetInt(e.p, e.len, "enabled", a) &&
          a == 0, "last element reachable");
    check(!jsonArrayAt(arr.p, arr.len, 3, e), "index past the end rejected");
    check(!jsonArrayAt(arr.p, arr.len, -1, e), "negative index rejected");

    const char *empty = "{\"schedule\":[]}";
    check(jsonFind(empty, strlen(empty), "schedule", arr) &&
          jsonArrayCount(arr.p, arr.len) == 0, "empty array");

    printf("\n--- malformed input must not crash or over-read ---\n");
    // Inputs where `a` has no recoverable value. Any success here is fabrication.
    const char *bad[] = {
        "", "{", "}", "[", "{\"a\"", "{\"a\":", "{\"a\":}", "{\"a\":\"unterminated",
        "{\"a\":{\"b\":", "{{{{{{{{", "]]]]]]", "not json at all",
        "{\"a\":\"\\", "{\"a\":[1,2,", "\"bare string\"", "{\"\":1}",
    };
    // The contract: return cleanly, never crash, never read past len, and never
    // report success on garbage. Run these under a sanitizer build (make asan)
    // so an over-read is a hard failure rather than a silent pass.
    const size_t nbad = sizeof(bad) / sizeof(bad[0]);
    bool anyFalsePositive = false;
    for (size_t i = 0; i < nbad; i++) {
        size_t blen = strlen(bad[i]);
        long out = 0;
        char sbuf[16];
        JsonVal jv;
        if (jsonGetInt(bad[i], blen, "a", out)) anyFalsePositive = true;
        if (jsonGetStr(bad[i], blen, "a", sbuf, sizeof(sbuf))) anyFalsePositive = true;
        if (jsonFind(bad[i], blen, "a.b", jv)) anyFalsePositive = true;
        jsonArrayCount(bad[i], blen);
        jsonArrayAt(bad[i], blen, 0, jv);
    }
    snprintf(buf, sizeof(buf),
             "%zu malformed inputs: no crash, no fabricated values", nbad);
    check(!anyFalsePositive, buf);

    // Documented leniency: this is a scanner, not a validator. A well-formed
    // pair followed by garbage still resolves, because the value really is
    // there. Callers range-check every field regardless, so this is safe --
    // but it is a deliberate choice, not an oversight, so it is pinned here.
    const char *lenient = "{\"a\":1,,,}";
    long lv = 0;
    check(jsonGetInt(lenient, strlen(lenient), "a", lv) && lv == 1,
          "trailing garbage after a valid pair is tolerated, by design");

    // Non-terminated buffer: the parser must respect len, not look for a NUL.
    char raw[8] = {'{', '"', 'a', '"', ':', '4', '2', '}'};
    long got = 0;
    check(jsonGetInt(raw, sizeof(raw), "a", got) && got == 42,
          "respects len on a buffer with no NUL terminator");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
