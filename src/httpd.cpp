#include "httpd.h"

#include <stdarg.h>

#include "config.h"
#include "control.h"
#include "display.h"
#include "eventlog.h"
#include "json.h"
#include "mqtt_ha.h"
#include "netwatch.h"
#include "timekeep.h"
#include "web_assets.h"

// Defined in main.cpp, which owns the backstop deadline.
void armOutageTest();

namespace {

TCPServer server(80);

const size_t REQ_MAX = 1024;      // request line + headers
const uint32_t READ_TIMEOUT_MS = 2000;

// Sized from the largest real request rather than a round number: a full
// schedule is MAX_SCHEDULE entries of roughly
//   {"days":127,"anchor":0,"minutes":1290,"mode":0,"effect":0,"r":255,
//    "g":136,"b":0,"brightness":60}
// which is a little under 100 bytes each. At 2048 the UI could not save a full
// schedule at all -- the clock answered 413 to its own config page.
const size_t SCHED_ENTRY_JSON_MAX = 128;
const size_t BODY_MAX = MAX_SCHEDULE * SCHED_ENTRY_JSON_MAX + 256;

char g_req[REQ_MAX];
char g_body[BODY_MAX];

// Same sizing for the response side. Undersizing this does not fail loudly --
// it emits JSON that stops mid-token, and the browser reports a parse error far
// from the cause.
char g_out[MAX_SCHEDULE * SCHED_ENTRY_JSON_MAX + 512];

// Bounded appender.
//
// The obvious `p += snprintf(p, end - p, ...)` is wrong: snprintf returns the
// length it WOULD have written, so on truncation p advances past the buffer and
// every later call gets a negative size. This tracks overflow explicitly and
// stops writing, and callers turn that into a 500 rather than shipping a
// half-formed body.
struct Out {
    char *p;
    char *end;
    bool overflow;
};

void app(Out &o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void app(Out &o, const char *fmt, ...) {
    if (o.overflow) return;
    size_t space = (size_t)(o.end - o.p);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o.p, space, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= space) { o.overflow = true; return; }
    o.p += n;
}

uint32_t g_requests = 0;
uint32_t g_rejected = 0;

// Tracks the Wi-Fi edge so the listening socket can be recreated.
//
// WiFi.off() tears down the interface and takes the TCPServer's listening
// socket with it. Re-associating does NOT bring it back: the radio comes up,
// WiFi.ready() reports true, the recovery ladder is satisfied -- and the clock
// silently stops answering on port 80 until something reboots it. Found by
// deliberately dropping the radio and watching the clock come back only when an
// unrelated backstop reset fired.
bool g_wifiWasUp = false;
uint32_t g_rebinds = 0;

// --- response helpers -------------------------------------------------------

void sendHeaders(TCPClient &c, const char *status, const char *type,
                 size_t len, const char *extra = nullptr) {
    char h[256];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.0 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: X-Auth, Content-Type\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s\r\n",
                     status, type, (unsigned)len, extra ? extra : "");
    c.write((const uint8_t *)h, n);
}

void sendText(TCPClient &c, const char *status, const char *body) {
    size_t n = strlen(body);
    sendHeaders(c, status, "application/json", n);
    c.write((const uint8_t *)body, n);
}

void sendOk(TCPClient &c) { sendText(c, "200 OK", "{\"ok\":true}"); }
void sendErr(TCPClient &c, const char *status, const char *msg) {
    char b[128];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", msg);
    sendText(c, status, b);
}

void serveIndex(TCPClient &c) {
    sendHeaders(c, "200 OK", "text/html", WEB_INDEX_GZ_LEN,
                "Content-Encoding: gzip\r\n");
    for (uint32_t i = 0; i < WEB_INDEX_GZ_LEN; i += 512) {
        uint32_t n = min((uint32_t)512, WEB_INDEX_GZ_LEN - i);
        c.write(WEB_INDEX_GZ + i, n);
    }
}

// --- auth -------------------------------------------------------------------

// Length-independent, value-independent comparison. Over plain HTTP this stops
// accidents rather than attackers -- the README says so plainly -- but leaking
// the password one byte at a time via response timing would be gratuitous.
bool constantTimeEquals(const char *a, const char *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

bool authorized(const char *req) {
    if (!cfg.web_pass[0]) return true;   // no password set: writes are open

    const char *h = strstr(req, "X-Auth:");
    if (!h) h = strstr(req, "x-auth:");
    if (!h) return false;
    h += 7;
    while (*h == ' ') h++;

    char given[sizeof(cfg.web_pass)];
    size_t i = 0;
    while (h[i] && h[i] != '\r' && h[i] != '\n' && i < sizeof(given) - 1) {
        given[i] = h[i];
        i++;
    }
    given[i] = 0;

    if (i != strlen(cfg.web_pass)) return false;
    return constantTimeEquals(given, cfg.web_pass, i);
}

// --- GET /api/state ---------------------------------------------------------

const char *resetReasonName(int r) {
    switch (r) {
        case 20:  return "pin";
        case 30:  return "power management";
        case 40:  return "power down";
        case 50:  return "brownout";
        case 60:  return "watchdog";
        case 70:  return "firmware update";
        case 80:  return "update failed";
        case 90:  return "factory reset";
        case 100: return "safe mode";
        case 110: return "dfu mode";
        case 120: return "panic";
        case 140: return "user";
        case 0:   return "none";
        default:  return "unknown";
    }
}

void serveState(TCPClient &c) {
    LocalTime lt = Timekeep::now();
    // From the RESOLVED layer, not from cfg. Reading the base config here made
    // the status report "Solid" while a schedule entry was visibly running
    // Breathe -- a diagnostic that lies is worse than no diagnostic.
    const Control::Settings actNow = Control::active();
    const uint8_t fxNow = (actNow.mode == MODE_EFFECT && actNow.effect < FX_COUNT)
                          ? actNow.effect : (uint8_t)FX_SOLID;
    const TzInfo &tz = Timekeep::tz();

    int n = snprintf(g_out, sizeof(g_out),
        "{"
        "\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d\",\"abbr\":\"%s\",\"dst\":%s,"
        "\"synced\":%s,\"ntp_age\":%lu,\"ntp_stale\":%s,\"ntp_fails\":%lu,"
        "\"tz_ok\":%s,\"std_off\":%ld,\"dst_off\":%ld,\"observe_dst\":%s,"
        "\"wifi\":%s,\"cloud\":%s,\"rssi\":%d,\"ip\":\"%s\",\"ssid\":\"%s\","
        "\"uptime\":%lu,\"freemem\":%lu,\"boots\":%lu,\"recoveries\":%lu,"
        "\"reset_reason\":\"%s\",\"requests\":%lu,\"rebinds\":%lu,"
        "\"quiet\":%lu,"
        // A per-compile stamp, not a phase name. Polling an endpoint until it
        // merely *answers* is useless after an OTA -- the outgoing firmware
        // answers too, so you read stale values and conclude your fix did not
        // land. Wait for this string to change instead.
        "\"auth\":%s,\"digits\":%u,\"leds\":%u,\"frames\":%lu,"
        "\"fx\":%u,\"fx_name\":\"%s\",\"lit\":[%u,%u,%u],"
        "\"brightness\":%u,\"source\":\"%s\",\"entry\":%d,"
        "\"ha_override\":%s,\"ha_since\":%lu,\"mqtt\":%s,"
        "\"mqtt_configured\":%s,\"mqtt_connects\":%lu,\"mqtt_cmds\":%lu,"
        "\"mqtt_error\":\"%s\",\"since\":%lu,\"next\":%lu,"
        "\"sunrise\":%lu,\"sunset\":%lu,\"sun_valid\":%s,"
        "\"build\":\"" __DATE__ " " __TIME__ "\""
        "}",
        lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second,
        lt.abbr[0] ? lt.abbr : "UTC", lt.is_dst ? "true" : "false",
        Timekeep::everSynced() ? "true" : "false",
        (unsigned long)Timekeep::secondsSinceSync(),
        Timekeep::isStale() ? "true" : "false",
        (unsigned long)Timekeep::failedSyncs(),
        Timekeep::tzValid() ? "true" : "false",
        (long)tz.std_off, (long)(tz.has_dst ? tz.dst_off : tz.std_off),
        cfg.observe_dst ? "true" : "false",
        NetWatch::wifiUp() ? "true" : "false",
        NetWatch::cloudUp() ? "true" : "false",
        NetWatch::rssi(), WiFi.localIP().toString().c_str(), WiFi.SSID(),
        (unsigned long)(millis() / 1000), (unsigned long)System.freeMemory(),
        (unsigned long)NetWatch::bootCount(),
        (unsigned long)NetWatch::wifiRecoveries(),
        resetReasonName(NetWatch::lastResetReason()),
        (unsigned long)g_requests, (unsigned long)g_rebinds,
        (unsigned long)NetWatch::secondsSinceTraffic(),
        cfg.web_pass[0] ? "true" : "false",
        cfg.digits, Display::ledCount(), (unsigned long)Display::frameCount(),
        fxNow, EFFECT_NAMES[fxNow],
        Display::lastLitColor().r, Display::lastLitColor().g,
        Display::lastLitColor().b,
        // NOT %lld: newlib-nano's printf has no long-long support, and passing
        // one silently emits the literal "ld" and then desynchronises every
        // argument after it -- which is how this endpoint started returning
        // corrupt JSON with trailing garbage. Epochs are positive and fit an
        // unsigned 32-bit value until 2106, which outlives the hardware.
        actNow.brightness,
        Control::sourceName(), (int)Control::scheduleEntry(),
        Control::overridden() ? "true" : "false",
        (unsigned long)Control::overrideSince(),
        MqttHa::connected() ? "true" : "false",
        MqttHa::configured() ? "true" : "false",
        (unsigned long)MqttHa::connectCount(),
        (unsigned long)MqttHa::commandCount(),
        MqttHa::lastError(),
        (unsigned long)Control::since(), (unsigned long)Control::nextChange(),
        (unsigned long)Control::sunToday().rise_utc,
        (unsigned long)Control::sunToday().set_utc,
        Control::sunToday().valid ? "true" : "false");

    if (n < 0 || (size_t)n >= sizeof(g_out)) {
        sendErr(c, "500 Internal Server Error", "state truncated");
        return;
    }
    sendHeaders(c, "200 OK", "application/json", (size_t)n);
    c.write((const uint8_t *)g_out, n);
}

// --- GET /api/config --------------------------------------------------------

void serveConfig(TCPClient &c) {
    Out o{g_out, g_out + sizeof(g_out), false};

    // The effect list is served from EFFECT_NAMES rather than duplicated in the
    // page, so the UI, the firmware and the Home Assistant effect_list cannot
    // drift apart -- a mismatch would show the wrong effect name for an id.
    app(o, "{\"effects\":[");
    for (uint8_t i = 0; i < FX_COUNT; i++)
        app(o, "%s\"%s\"", i ? "," : "", EFFECT_NAMES[i]);
    app(o, "],");

    app(o, "\"tz\":\"%s\",\"ntp\":\"%s\",\"lat\":%.4f,\"lon\":%.4f,"
        "\"digits\":%u,\"hour_format\":%u,\"blank_hour_zero\":%u,"
        "\"observe_dst\":%u,"
        "\"mode\":%u,\"effect\":%u,\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u,"
        "\"mqtt_host\":\"%s\",\"mqtt_port\":%u,\"mqtt_user\":\"%s\","
        "\"has_mqtt_pass\":%s,\"has_web_pass\":%s}",
        cfg.tz, cfg.ntp_server, (double)cfg.lat, (double)cfg.lon,
        cfg.digits, cfg.hour_format, cfg.blank_hour_zero, cfg.observe_dst,
        cfg.mode, cfg.effect, cfg.r, cfg.g, cfg.b, cfg.brightness,
        cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_user,
        cfg.mqtt_pass[0] ? "true" : "false",
        cfg.web_pass[0] ? "true" : "false");

    if (o.overflow) {
        sendErr(c, "500 Internal Server Error", "config truncated");
        return;
    }
    size_t n = (size_t)(o.p - g_out);
    sendHeaders(c, "200 OK", "application/json", n);
    c.write((const uint8_t *)g_out, n);
}

// --- POST /api/config -------------------------------------------------------

// Every field is optional; only what the client sends is changed. Ranges are
// clamped rather than rejected, except the timezone, which must parse or the
// whole request fails -- a bad TZ would silently put the clock hours off.
bool applyConfig(const char *body, size_t len, char *err, size_t errn) {
    char sbuf[64];
    long v;
    float f;

    // Counted so a body that yields nothing -- truncated, malformed, or simply
    // misspelled -- fails loudly instead of returning 200 and quietly saving
    // nothing. Silent success is the worst answer a config endpoint can give.
    int applied = 0;

    if (jsonGetStr(body, len, "tz", sbuf, sizeof(sbuf))) {
        if (!Timekeep::setTz(sbuf)) {
            snprintf(err, errn, "unparseable timezone rule");
            return false;
        }
        applied++;
    }
    if (jsonGetStr(body, len, "ntp", sbuf, sizeof(sbuf))) {
        strncpy(cfg.ntp_server, sbuf, sizeof(cfg.ntp_server) - 1);
        cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = 0;
        applied++;
    }

    if (jsonGetFloat(body, len, "lat", f)) {
        if (f < -90.0f || f > 90.0f) {
            snprintf(err, errn, "latitude out of range");
            return false;
        }
        cfg.lat = f;
        applied++;
    }
    if (jsonGetFloat(body, len, "lon", f)) {
        if (f < -180.0f || f > 180.0f) {
            snprintf(err, errn, "longitude out of range");
            return false;
        }
        cfg.lon = f;
        applied++;
    }

    if (jsonGetInt(body, len, "digits", v)) {
        cfg.digits = (uint8_t)constrain(v, 2, (long)MAX_DIGITS);
        Display::setDigitCount(cfg.digits);
        applied++;
    }
    if (jsonGetInt(body, len, "hour_format", v)) {
        cfg.hour_format = (v == 24) ? 24 : 12;
        applied++;
    }
    if (jsonGetInt(body, len, "blank_hour_zero", v)) {
        cfg.blank_hour_zero = v ? 1 : 0;
        applied++;
    }
    if (jsonGetInt(body, len, "observe_dst", v)) {
        cfg.observe_dst = v ? 1 : 0;
        // Re-parse so the change takes effect now rather than at next boot.
        Timekeep::setTz(cfg.tz);
        applied++;
    }
    if (jsonGetInt(body, len, "mode", v)) {
        cfg.mode = (uint8_t)constrain(v, 0, 1);
        applied++;
    }
    if (jsonGetInt(body, len, "effect", v)) {
        cfg.effect = (uint8_t)constrain(v, 0, FX_COUNT - 1);
        applied++;
    }
    if (jsonGetInt(body, len, "r", v)) {
        cfg.r = (uint8_t)constrain(v, 0, 255);
        applied++;
    }
    if (jsonGetInt(body, len, "g", v)) {
        cfg.g = (uint8_t)constrain(v, 0, 255);
        applied++;
    }
    if (jsonGetInt(body, len, "b", v)) {
        cfg.b = (uint8_t)constrain(v, 0, 255);
        applied++;
    }
    if (jsonGetInt(body, len, "brightness", v)) {
        cfg.brightness = (uint8_t)constrain(v, 1, 100);
        applied++;
    }

    if (jsonGetStr(body, len, "mqtt_host", sbuf, sizeof(sbuf))) {
        strncpy(cfg.mqtt_host, sbuf, sizeof(cfg.mqtt_host) - 1);
        cfg.mqtt_host[sizeof(cfg.mqtt_host) - 1] = 0;
        applied++;
    }
    if (jsonGetInt(body, len, "mqtt_port", v)) {
        cfg.mqtt_port = (uint16_t)constrain(v, 1, 65535);
        applied++;
    }
    if (jsonGetStr(body, len, "mqtt_user", sbuf, sizeof(sbuf))) {
        strncpy(cfg.mqtt_user, sbuf, sizeof(cfg.mqtt_user) - 1);
        cfg.mqtt_user[sizeof(cfg.mqtt_user) - 1] = 0;
        applied++;
    }
    // Secrets are only overwritten when a non-empty value is supplied, so the
    // page can render "already set" without ever reading them back.
    if (jsonGetStr(body, len, "mqtt_pass", sbuf, sizeof(sbuf)) && sbuf[0]) {
        strncpy(cfg.mqtt_pass, sbuf, sizeof(cfg.mqtt_pass) - 1);
        cfg.mqtt_pass[sizeof(cfg.mqtt_pass) - 1] = 0;
        applied++;
    }
    if (jsonGetStr(body, len, "web_pass", sbuf, sizeof(sbuf))) {
        strncpy(cfg.web_pass, sbuf, sizeof(cfg.web_pass) - 1);
        cfg.web_pass[sizeof(cfg.web_pass) - 1] = 0;
        applied++;
    }

    if (applied == 0) {
        snprintf(err, errn, "no recognized settings in request body");
        return false;
    }
    return true;
}

// --- schedule ---------------------------------------------------------------

void serveSchedule(TCPClient &c) {
    Out o{g_out, g_out + sizeof(g_out), false};
    app(o, "{\"schedule\":[");

    bool first = true;
    for (uint8_t i = 0; i < MAX_SCHEDULE; i++) {
        const ScheduleEntry &e = cfg.schedule[i];
        if (!e.enabled) continue;
        app(o, "%s{\"days\":%u,\"anchor\":%u,\"minutes\":%d,\"mode\":%u,"
               "\"effect\":%u,\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u}",
            first ? "" : ",", e.days_mask, e.anchor, e.minutes, e.mode,
            e.effect, e.r, e.g, e.b, e.brightness);
        first = false;
    }
    app(o, "]}");

    if (o.overflow) {
        sendErr(c, "500 Internal Server Error", "schedule too large to serialize");
        return;
    }
    size_t n = (size_t)(o.p - g_out);
    sendHeaders(c, "200 OK", "application/json", n);
    c.write((const uint8_t *)g_out, n);
}

// Replaces the whole schedule; partial updates would need stable entry ids and
// this list is short enough that resending it is simpler and less error-prone.
bool applySchedule(const char *body, size_t len, char *err, size_t errn) {
    JsonVal arr;
    if (!jsonFind(body, len, "schedule", arr) || arr.type != JSON_ARRAY) {
        snprintf(err, errn, "expected a schedule array");
        return false;
    }

    int count = jsonArrayCount(arr.p, arr.len);
    if (count < 0) { snprintf(err, errn, "malformed schedule array"); return false; }
    if (count > MAX_SCHEDULE) {
        snprintf(err, errn, "too many entries (max %u)", MAX_SCHEDULE);
        return false;
    }

    // Build into a scratch copy so a bad entry halfway through cannot leave the
    // clock running a half-applied schedule.
    ScheduleEntry next[MAX_SCHEDULE];
    memset(next, 0, sizeof(next));

    for (int i = 0; i < count; i++) {
        JsonVal e;
        if (!jsonArrayAt(arr.p, arr.len, i, e) || e.type != JSON_OBJECT) {
            snprintf(err, errn, "entry %d is not an object", i);
            return false;
        }
        long v;
        ScheduleEntry &s = next[i];
        s.enabled = 1;
        s.days_mask = jsonGetInt(e.p, e.len, "days", v) ? (uint8_t)(v & 0x7F) : 0x7F;
        s.anchor = jsonGetInt(e.p, e.len, "anchor", v)
                   ? (uint8_t)constrain(v, 0, 2) : ANCHOR_CLOCK;

        if (!jsonGetInt(e.p, e.len, "minutes", v)) {
            snprintf(err, errn, "entry %d has no time", i);
            return false;
        }
        // Clock anchors are a time of day; sun anchors are a signed offset.
        long lo = (s.anchor == ANCHOR_CLOCK) ? 0 : -720;
        long hi = (s.anchor == ANCHOR_CLOCK) ? 1439 : 720;
        if (v < lo || v > hi) {
            snprintf(err, errn, "entry %d time out of range", i);
            return false;
        }
        s.minutes = (int16_t)v;

        s.mode = jsonGetInt(e.p, e.len, "mode", v) ? (uint8_t)constrain(v, 0, 1) : 0;
        s.effect = jsonGetInt(e.p, e.len, "effect", v)
                   ? (uint8_t)constrain(v, 0, FX_COUNT - 1) : 0;
        s.r = jsonGetInt(e.p, e.len, "r", v) ? (uint8_t)constrain(v, 0, 255) : 255;
        s.g = jsonGetInt(e.p, e.len, "g", v) ? (uint8_t)constrain(v, 0, 255) : 136;
        s.b = jsonGetInt(e.p, e.len, "b", v) ? (uint8_t)constrain(v, 0, 255) : 0;
        s.brightness = jsonGetInt(e.p, e.len, "brightness", v)
                       ? (uint8_t)constrain(v, 1, 100) : 60;
    }

    memcpy(cfg.schedule, next, sizeof(cfg.schedule));
    return true;
}

// Oldest-first, so the tail of the list is what happened just before the last
// failure -- which is the only part anyone reads.
void serveEvents(TCPClient &c) {
    const EvLog &L = eventLogData();
    Out o{g_out, g_out + sizeof(g_out), false};
    const EvAlive &A = eventLogAlive();
    app(o, "{\"boot_id\":%u,\"count\":%u,"
           "\"last_alive\":{\"boot\":%u,\"uptime\":%lu},\"events\":[",
        L.boot_id, L.count, A.boot_id, (unsigned long)A.uptime);

    for (uint8_t i = 0; i < L.count; i++) {
        uint8_t idx = (uint8_t)((L.next + EV_MAX - L.count + i) % EV_MAX);
        const EvEntry &e = L.e[idx];
        app(o, "%s{\"boot\":%u,\"t\":%lu,\"ev\":\"%s\",\"arg\":%u}",
            i ? "," : "", e.boot_id, (unsigned long)e.uptime, eventName(e.code), e.arg);
    }
    app(o, "]}");

    if (o.overflow) { sendErr(c, "500 Internal Server Error", "event log too large"); return; }
    size_t n = (size_t)(o.p - g_out);
    sendHeaders(c, "200 OK", "application/json", n);
    c.write((const uint8_t *)g_out, n);
}

// --- routing ----------------------------------------------------------------

void handle(TCPClient &c, const char *req, const char *body, size_t bodyLen) {
    bool isGet = !strncmp(req, "GET ", 4);
    bool isPost = !strncmp(req, "POST ", 5);

    if (!strncmp(req, "OPTIONS ", 8)) {         // CORS preflight
        sendHeaders(c, "204 No Content", "text/plain", 0);
        return;
    }

    const char *path = strchr(req, ' ');
    if (!path) { sendErr(c, "400 Bad Request", "malformed request"); return; }
    path++;

    if (isPost && !authorized(req)) {
        g_rejected++;
        sendErr(c, "401 Unauthorized", "bad or missing X-Auth");
        return;
    }

    if (isGet && (!strncmp(path, "/ ", 2) || !strncmp(path, "/index.html", 11))) {
        serveIndex(c);
    } else if (isGet && !strncmp(path, "/api/state", 10)) {
        serveState(c);
    } else if (isGet && !strncmp(path, "/api/config", 11)) {
        serveConfig(c);
    } else if (isGet && !strncmp(path, "/api/schedule", 13)) {
        serveSchedule(c);
    } else if (isGet && !strncmp(path, "/api/events", 11)) {
        serveEvents(c);
    } else if (isPost && !strncmp(path, "/api/config", 11)) {
        char err[96] = "";
        if (!applyConfig(body, bodyLen, err, sizeof(err))) {
            configLoad();                       // discard the partial application
            Display::setDigitCount(cfg.digits);
            Timekeep::begin();
            sendErr(c, "400 Bad Request", err);
            return;
        }
        configSave();
        Control::invalidate();
        MqttHa::notifyChanged();
        sendOk(c);
    } else if (isPost && !strncmp(path, "/api/schedule", 13)) {
        char err[96] = "";
        if (!applySchedule(body, bodyLen, err, sizeof(err))) {
            sendErr(c, "400 Bad Request", err);
            return;
        }
        configSave();
        Control::invalidate();
        MqttHa::notifyChanged();
        sendOk(c);
    } else if (isPost && !strncmp(path, "/api/action", 11)) {
        char what[24] = "";
        jsonGetStr(body, bodyLen, "action", what, sizeof(what));

        if (!strcmp(what, "resync")) {
            sendText(c, "200 OK", Timekeep::syncNow()
                     ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"NTP failed\"}");
        } else if (!strcmp(what, "release")) {
            Control::releaseOverride();
            MqttHa::notifyChanged();
            sendOk(c);
        } else if (!strcmp(what, "wifitest")) {
            // Answer BEFORE dropping the radio, or the caller never hears back.
            sendOk(c);
            c.flush(); delay(200); c.stop();
            armOutageTest();
            return;
        } else if (!strcmp(what, "reboot")) {
            sendOk(c);
            c.flush(); delay(200); c.stop();
            System.reset();
        } else if (!strcmp(what, "factory")) {
            configDefaults();
            configSave();
            sendOk(c);
            c.flush(); delay(200); c.stop();
            System.reset();
        } else {
            sendErr(c, "400 Bad Request", "unknown action");
        }
    } else {
        sendErr(c, "404 Not Found", "no such endpoint");
    }
}

}  // namespace

void Httpd::begin() {
    server.begin();
    g_wifiWasUp = WiFi.ready();
}

uint32_t Httpd::requestCount() { return g_requests; }
uint32_t Httpd::rejectedAuth() { return g_rejected; }
uint32_t Httpd::rebinds() { return g_rebinds; }

void Httpd::tick() {
    bool up = WiFi.ready();
    if (up && !g_wifiWasUp) {
        server.begin();          // re-listen after the interface came back
        g_rebinds++;
    }
    g_wifiWasUp = up;
    if (!up) return;

    TCPClient c = server.available();
    if (!c) return;

    g_requests++;
    NetWatch::noteAlive();

    // Read headers up to the blank line.
    size_t len = 0;
    bool haveHeaders = false;
    uint32_t deadline = millis() + READ_TIMEOUT_MS;
    while (c.connected() && millis() < deadline && len < REQ_MAX - 1) {
        int ch = c.read();
        if (ch < 0) continue;
        g_req[len++] = (char)ch;
        if (len >= 4 && !memcmp(g_req + len - 4, "\r\n\r\n", 4)) {
            haveHeaders = true;
            break;
        }
    }
    g_req[len] = 0;

    if (!haveHeaders) {
        sendErr(c, "400 Bad Request", "headers too large or timed out");
        c.flush(); delay(5); c.stop();
        return;
    }

    // Read the body, if the client declared one.
    size_t bodyLen = 0;
    const char *cl = strstr(g_req, "Content-Length:");
    if (!cl) cl = strstr(g_req, "content-length:");
    if (cl) {
        long want = strtol(cl + 15, nullptr, 10);
        if (want < 0) want = 0;
        if (want > (long)BODY_MAX - 1) {
            sendErr(c, "413 Payload Too Large", "body too large");
            c.flush(); delay(5); c.stop();
            return;
        }
        deadline = millis() + READ_TIMEOUT_MS;
        while ((long)bodyLen < want && c.connected() && millis() < deadline) {
            int ch = c.read();
            if (ch < 0) continue;
            g_body[bodyLen++] = (char)ch;
        }
    }
    g_body[bodyLen] = 0;

    handle(c, g_req, g_body, bodyLen);

    c.flush();
    delay(5);
    c.stop();
}
