#include "json.h"

#include <stdlib.h>
#include <string.h>

namespace {

const char *skipWs(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// Returns the position just past the closing quote, or end on malformed input.
const char *skipString(const char *p, const char *end) {
    if (p >= end || *p != '"') return end;
    p++;
    while (p < end) {
        if (*p == '\\') { p += 2; continue; }   // skip the escaped char wholesale
        if (*p == '"') return p + 1;
        p++;
    }
    return end;
}

// Returns the position just past the value starting at p.
const char *skipValue(const char *p, const char *end) {
    p = skipWs(p, end);
    if (p >= end) return end;

    if (*p == '"') return skipString(p, end);

    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (p < end) {
            if (*p == '"') { p = skipString(p, end); continue; }
            if (*p == open) depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return end;
    }

    // Number, true, false or null: run to the next structural character.
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

JsonType classify(const char *p, const char *end) {
    if (p >= end) return JSON_NONE;
    switch (*p) {
        case '{': return JSON_OBJECT;
        case '[': return JSON_ARRAY;
        case '"': return JSON_STRING;
        case 't': case 'f': return JSON_BOOL;
        case 'n': return JSON_NULL;
        default:
            return (*p == '-' || (*p >= '0' && *p <= '9')) ? JSON_NUMBER : JSON_NONE;
    }
}

// Classify, then reject anything the scan ran off the end of.
//
// skipString/skipValue return `end` on unterminated input, which without this
// check reads as a perfectly good value spanning the rest of the buffer -- so
// {"a":"unterminated  would hand back the string "unterminated" as valid. A
// truncated object or array has the same problem.
JsonType classifyChecked(const char *start, size_t len, const char *end) {
    JsonType t = classify(start, end);
    if (len == 0) return JSON_NONE;
    char last = start[len - 1];
    switch (t) {
        case JSON_STRING: return (len >= 2 && last == '"') ? t : JSON_NONE;
        case JSON_OBJECT: return (last == '}') ? t : JSON_NONE;
        case JSON_ARRAY:  return (last == ']') ? t : JSON_NONE;
        default: return t;
    }
}

// Compares a JSON string token (starting at the opening quote) against a plain
// key. Escapes are not resolved -- config keys never contain them.
bool keyEquals(const char *tok, const char *end, const char *key, size_t keyLen) {
    if (tok >= end || *tok != '"') return false;
    tok++;
    size_t i = 0;
    while (tok < end && *tok != '"') {
        if (i >= keyLen || *tok != key[i]) return false;
        tok++; i++;
    }
    return i == keyLen && tok < end && *tok == '"';
}

// Finds a single (non-dotted) key within one object.
bool findFlat(const char *json, size_t len, const char *key, size_t keyLen,
              JsonVal &out) {
    const char *p = json, *end = json + len;
    p = skipWs(p, end);
    if (p >= end || *p != '{') return false;
    p++;

    while (p < end) {
        p = skipWs(p, end);
        if (p >= end || *p == '}') return false;

        const char *keyTok = p;
        const char *afterKey = skipString(p, end);
        if (afterKey >= end) return false;

        p = skipWs(afterKey, end);
        if (p >= end || *p != ':') return false;
        p++;
        p = skipWs(p, end);

        const char *valStart = p;
        const char *valEnd = skipValue(p, end);

        if (keyEquals(keyTok, end, key, keyLen)) {
            out.p = valStart;
            out.len = (size_t)(valEnd - valStart);
            out.type = classifyChecked(valStart, out.len, end);
            return out.type != JSON_NONE;
        }

        p = skipWs(valEnd, end);
        if (p < end && *p == ',') p++;
    }
    return false;
}

}  // namespace

bool jsonFind(const char *json, size_t len, const char *key, JsonVal &out) {
    out = JsonVal{};
    if (!json || !key || !*key) return false;

    const char *cur = json;
    size_t curLen = len;

    // Walk one dotted segment at a time, descending into nested objects.
    while (true) {
        const char *dot = strchr(key, '.');
        size_t seg = dot ? (size_t)(dot - key) : strlen(key);

        JsonVal v;
        if (!findFlat(cur, curLen, key, seg, v)) return false;

        if (!dot) { out = v; return true; }
        if (v.type != JSON_OBJECT) return false;

        cur = v.p;
        curLen = v.len;
        key = dot + 1;
    }
}

bool jsonGetInt(const char *json, size_t len, const char *key, long &out) {
    JsonVal v;
    if (!jsonFind(json, len, key, v) || v.type != JSON_NUMBER) return false;
    char buf[24];
    size_t n = v.len < sizeof(buf) - 1 ? v.len : sizeof(buf) - 1;
    memcpy(buf, v.p, n);
    buf[n] = 0;
    char *endp = nullptr;
    long r = strtol(buf, &endp, 10);
    if (endp == buf) return false;
    out = r;
    return true;
}

bool jsonGetFloat(const char *json, size_t len, const char *key, float &out) {
    JsonVal v;
    if (!jsonFind(json, len, key, v) || v.type != JSON_NUMBER) return false;
    char buf[32];
    size_t n = v.len < sizeof(buf) - 1 ? v.len : sizeof(buf) - 1;
    memcpy(buf, v.p, n);
    buf[n] = 0;
    char *endp = nullptr;
    float r = strtof(buf, &endp);
    if (endp == buf) return false;
    out = r;
    return true;
}

bool jsonGetBool(const char *json, size_t len, const char *key, bool &out) {
    JsonVal v;
    if (!jsonFind(json, len, key, v) || v.type != JSON_BOOL) return false;
    out = (v.len > 0 && v.p[0] == 't');
    return true;
}

bool jsonGetStr(const char *json, size_t len, const char *key, char *out, size_t n) {
    if (!out || n == 0) return false;
    JsonVal v;
    if (!jsonFind(json, len, key, v) || v.type != JSON_STRING) return false;

    const char *p = v.p + 1;                 // past the opening quote
    const char *end = v.p + v.len - 1;       // at the closing quote
    size_t i = 0;

    while (p < end && i < n - 1) {
        if (*p != '\\') { out[i++] = *p++; continue; }
        p++;
        if (p >= end) break;
        switch (*p) {
            case 'n': out[i++] = '\n'; p++; break;
            case 't': out[i++] = '\t'; p++; break;
            case 'r': out[i++] = '\r'; p++; break;
            case 'b': out[i++] = '\b'; p++; break;
            case 'f': out[i++] = '\f'; p++; break;
            case '"': out[i++] = '"';  p++; break;
            case '\\': out[i++] = '\\'; p++; break;
            case '/': out[i++] = '/';  p++; break;
            case 'u': {
                // The config surface is ASCII; anything else becomes '?' rather
                // than silently truncating the string.
                p++;
                unsigned code = 0;
                int digits = 0;
                while (p < end && digits < 4) {
                    char c = *p;
                    unsigned d;
                    if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
                    else break;
                    code = code * 16 + d;
                    p++; digits++;
                }
                out[i++] = (code && code < 128) ? (char)code : '?';
                break;
            }
            default: out[i++] = *p++; break;
        }
    }
    out[i] = 0;
    return true;
}

int jsonArrayCount(const char *arr, size_t len) {
    const char *p = arr, *end = arr + len;
    p = skipWs(p, end);
    if (p >= end || *p != '[') return -1;
    p++;
    p = skipWs(p, end);
    if (p < end && *p == ']') return 0;

    int count = 0;
    while (p < end) {
        p = skipValue(p, end);
        count++;
        p = skipWs(p, end);
        if (p >= end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        break;
    }
    return count;
}

bool jsonArrayAt(const char *arr, size_t len, int index, JsonVal &out) {
    out = JsonVal{};
    if (index < 0) return false;

    const char *p = arr, *end = arr + len;
    p = skipWs(p, end);
    if (p >= end || *p != '[') return false;
    p++;

    for (int i = 0; p < end; i++) {
        p = skipWs(p, end);
        if (p >= end || *p == ']') return false;

        const char *valStart = p;
        const char *valEnd = skipValue(p, end);

        if (i == index) {
            out.p = valStart;
            out.len = (size_t)(valEnd - valStart);
            out.type = classifyChecked(valStart, out.len, end);
            return out.type != JSON_NONE;
        }

        p = skipWs(valEnd, end);
        if (p < end && *p == ',') p++;
    }
    return false;
}
