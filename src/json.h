#pragma once
#include <stddef.h>
#include <stdint.h>

// Minimal read-only JSON scanner.
//
// Non-allocating by design: every function scans the caller's buffer in place
// and never touches the heap. ArduinoJson 7 is heap-only (v7 removed
// StaticJsonDocument), and this firmware is expected to run for months without
// a reboot, so fragmentation matters more here than convenience.
//
// Free of any Particle dependency, so it is tested on the host.
//
// Only what this firmware actually parses is supported: objects, arrays,
// strings, numbers, booleans and null, with one level of dotted path lookup.
// It is a scanner, not a validator -- malformed input yields "not found"
// rather than a diagnostic.

enum JsonType : char {
    JSON_NONE = 0,
    JSON_OBJECT = 'o',
    JSON_ARRAY = 'a',
    JSON_STRING = 's',
    JSON_NUMBER = 'n',
    JSON_BOOL = 'b',
    JSON_NULL = 'z',
};

struct JsonVal {
    const char *p = nullptr;   // for strings, points at the opening quote
    size_t len = 0;            // spans the whole value including quotes/brackets
    JsonType type = JSON_NONE;

    bool ok() const { return type != JSON_NONE; }
};

// Look up `key` in the object at [json, json+len). `key` may contain one or more
// '.' separators to descend into nested objects, e.g. "color.r".
bool jsonFind(const char *json, size_t len, const char *key, JsonVal &out);

// Convenience accessors. Each returns false if the key is missing or the value
// is not of a compatible type; `out` is then untouched.
bool jsonGetInt(const char *json, size_t len, const char *key, long &out);
bool jsonGetFloat(const char *json, size_t len, const char *key, float &out);
bool jsonGetBool(const char *json, size_t len, const char *key, bool &out);

// Copies at most n-1 bytes plus a terminator, resolving \" \\ \/ \n \r \t \b \f
// and \uXXXX (as '?' for anything outside ASCII). Returns false if not a string.
bool jsonGetStr(const char *json, size_t len, const char *key, char *out, size_t n);

// Array helpers. `arr` must point at a '[' -- typically from a JsonVal obtained
// via jsonFind.
int jsonArrayCount(const char *arr, size_t len);
bool jsonArrayAt(const char *arr, size_t len, int index, JsonVal &out);
