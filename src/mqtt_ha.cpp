#include "mqtt_ha.h"

#include "MQTT.h"

#include "config.h"
#include "control.h"
#include "display.h"
#include "effects.h"
#include "json.h"
#include "netwatch.h"
#include "timekeep.h"

namespace {

// The library defaults to a 255-byte buffer, which is far too small for a
// discovery payload. 1536 leaves comfortable room for the light config with its
// full effect list and device block.
const uint16_t MQTT_BUFFER = 1536;

const uint32_t RECONNECT_MIN_MS = 5000;
const uint32_t RECONNECT_MAX_MS = 60000;
const uint32_t HEARTBEAT_MS = 30000;

char g_id[16];              // short device id, e.g. "a1b2c3"
char g_base[40];            // lixie/<id>
char g_name[32];            // friendly name

char g_buf[1400];           // payload scratch
char g_topic[96];

uint32_t g_lastAttempt = 0;
uint32_t g_backoff = RECONNECT_MIN_MS;
uint32_t g_lastHeartbeat = 0;
uint32_t g_connects = 0;
uint32_t g_commands = 0;
bool g_discovered = false;
bool g_dirty = true;
char g_err[48] = "";

void onMessage(char *topic, byte *payload, unsigned int length);
MQTT client("0.0.0.0", 1883, MQTT_BUFFER, onMessage);

// --- payload building -------------------------------------------------------

struct Out { char *p; char *end; bool overflow; };

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

// Full HA key names rather than the abbreviated forms. There is plenty of flash
// and buffer for it, and a half-remembered abbreviation fails as a silently
// ignored key -- an entity that shows up missing a feature, with nothing in any
// log to say why.
bool publishDiscovery() {
    Out o{g_buf, g_buf + sizeof(g_buf), false};

    // --- the light ---
    app(o, "{\"~\":\"%s\","
           "\"name\":null,"                    // null = use the device name
           "\"unique_id\":\"lixie_%s_light\","
           "\"object_id\":\"%s\","
           "\"schema\":\"json\","
           "\"state_topic\":\"~/state\","
           "\"command_topic\":\"~/set\","
           "\"availability_topic\":\"~/status\","
           "\"brightness\":true,"
           "\"supported_color_modes\":[\"rgb\"],"
           "\"effect\":true,"
           "\"effect_list\":[",
        g_base, g_id, g_name);
    for (uint8_t i = 0; i < FX_COUNT; i++)
        app(o, "%s\"%s\"", i ? "," : "", EFFECT_NAMES[i]);
    app(o, "],\"device\":{\"identifiers\":[\"lixie_%s\"],\"name\":\"%s\","
           "\"manufacturer\":\"djb\",\"model\":\"Lixie Clock\","
           "\"sw_version\":\"" __DATE__ "\"}}",
        g_id, g_name);

    if (o.overflow) {
        snprintf(g_err, sizeof(g_err), "discovery payload too large");
        return false;
    }
    snprintf(g_topic, sizeof(g_topic), "homeassistant/light/lixie_%s/config", g_id);
    if (!client.publish(g_topic, g_buf, true)) {
        snprintf(g_err, sizeof(g_err), "light discovery publish failed");
        return false;
    }

    // --- return-to-schedule button ---
    o = Out{g_buf, g_buf + sizeof(g_buf), false};
    app(o, "{\"~\":\"%s\",\"name\":\"Return to schedule\","
           "\"unique_id\":\"lixie_%s_release\","
           "\"command_topic\":\"~/release\","
           "\"availability_topic\":\"~/status\","
           "\"icon\":\"mdi:calendar-clock\","
           "\"device\":{\"identifiers\":[\"lixie_%s\"]}}",
        g_base, g_id, g_id);
    snprintf(g_topic, sizeof(g_topic), "homeassistant/button/lixie_%s_release/config", g_id);
    client.publish(g_topic, g_buf, true);

    // --- diagnostic sensors ---
    struct Sensor {
        const char *key, *name, *unit, *devclass, *icon;
    };
    static const Sensor SENSORS[] = {
        {"rssi",     "Wi-Fi signal", "dBm", "signal_strength", nullptr},
        {"uptime",   "Uptime",       "s",   "duration",        nullptr},
        {"freemem",  "Free memory",  "B",   "data_size",       nullptr},
        {"ntp_age",  "Last NTP sync","s",   "duration",        "mdi:clock-check"},
        {"ip",       "IP address",   nullptr, nullptr,         "mdi:ip-network"},
        {"source",   "Controlled by",nullptr, nullptr,         "mdi:cog-outline"},
    };

    for (const Sensor &s : SENSORS) {
        o = Out{g_buf, g_buf + sizeof(g_buf), false};
        app(o, "{\"~\":\"%s\",\"name\":\"%s\","
               "\"unique_id\":\"lixie_%s_%s\","
               "\"state_topic\":\"~/diag\","
               "\"availability_topic\":\"~/status\","
               "\"value_template\":\"{{ value_json.%s }}\","
               "\"entity_category\":\"diagnostic\"",
            g_base, s.name, g_id, s.key, s.key);
        if (s.unit) app(o, ",\"unit_of_measurement\":\"%s\"", s.unit);
        if (s.devclass) app(o, ",\"device_class\":\"%s\",\"state_class\":\"measurement\"",
                            s.devclass);
        if (s.icon) app(o, ",\"icon\":\"%s\"", s.icon);
        app(o, ",\"device\":{\"identifiers\":[\"lixie_%s\"]}}", g_id);

        snprintf(g_topic, sizeof(g_topic), "homeassistant/sensor/lixie_%s_%s/config",
                 g_id, s.key);
        client.publish(g_topic, g_buf, true);
    }

    return true;
}

void publishState() {
    Control::Settings a = Control::active();

    Out o{g_buf, g_buf + sizeof(g_buf), false};
    app(o, "{\"state\":\"%s\"", a.on ? "ON" : "OFF");
    if (a.on) {
        // HA brightness is 0-255; ours is a percentage.
        uint8_t bri = (uint8_t)((a.brightness * 255 + 50) / 100);
        if (bri == 0) bri = 1;
        app(o, ",\"brightness\":%u,\"color_mode\":\"rgb\","
               "\"color\":{\"r\":%u,\"g\":%u,\"b\":%u},\"effect\":\"%s\"",
            bri, a.r, a.g, a.b,
            EFFECT_NAMES[(a.mode == MODE_EFFECT && a.effect < FX_COUNT) ? a.effect : 0]);
    }
    app(o, "}");
    if (o.overflow) return;

    snprintf(g_topic, sizeof(g_topic), "%s/state", g_base);
    client.publish(g_topic, g_buf, true);
}

void publishDiag() {
    Out o{g_buf, g_buf + sizeof(g_buf), false};
    app(o, "{\"rssi\":%d,\"uptime\":%lu,\"freemem\":%lu,\"ntp_age\":%lu,"
           "\"ip\":\"%s\",\"source\":\"%s\",\"boots\":%lu,\"recoveries\":%lu}",
        NetWatch::rssi(), (unsigned long)(millis() / 1000),
        (unsigned long)System.freeMemory(),
        (unsigned long)Timekeep::secondsSinceSync(),
        WiFi.localIP().toString().c_str(), Control::sourceName(),
        (unsigned long)NetWatch::bootCount(),
        (unsigned long)NetWatch::wifiRecoveries());
    if (o.overflow) return;

    snprintf(g_topic, sizeof(g_topic), "%s/diag", g_base);
    client.publish(g_topic, g_buf, true);
}

// --- command handling -------------------------------------------------------

int effectIdByName(const char *name) {
    for (uint8_t i = 0; i < FX_COUNT; i++)
        if (!strcasecmp(name, EFFECT_NAMES[i])) return i;
    return -1;
}

void handleSet(const char *body, size_t len) {
    // Start from what is showing, so a command that only changes brightness
    // does not silently reset colour and effect to defaults.
    Control::Settings s = Control::active();

    char sbuf[32];
    long v;

    if (jsonGetStr(body, len, "state", sbuf, sizeof(sbuf)))
        s.on = (!strcasecmp(sbuf, "ON"));

    if (jsonGetInt(body, len, "brightness", v)) {
        // HA 0-255 -> percent, never rounding a non-zero level down to 0.
        long pct = (v * 100 + 127) / 255;
        if (pct < 1) pct = 1;
        if (pct > 100) pct = 100;
        s.brightness = (uint8_t)pct;
    }

    bool haveColour = false;
    long r, g, b;
    if (jsonGetInt(body, len, "color.r", r) &&
        jsonGetInt(body, len, "color.g", g) &&
        jsonGetInt(body, len, "color.b", b)) {
        s.r = (uint8_t)constrain(r, 0, 255);
        s.g = (uint8_t)constrain(g, 0, 255);
        s.b = (uint8_t)constrain(b, 0, 255);
        haveColour = true;
    }

    if (jsonGetStr(body, len, "effect", sbuf, sizeof(sbuf))) {
        int fx = effectIdByName(sbuf);
        if (fx >= 0) {
            s.effect = (uint8_t)fx;
            s.mode = (fx == FX_SOLID) ? MODE_SOLID : MODE_EFFECT;
        }
    } else if (haveColour) {
        // Setting a colour in HA means "show me this colour", so drop out of a
        // hue-driven effect that would ignore it.
        if (s.mode == MODE_EFFECT &&
            (s.effect == FX_RAINBOW_FLOW || s.effect == FX_RAINBOW_CYCLE ||
             s.effect == FX_WARM_GLOW)) {
            s.mode = MODE_SOLID;
            s.effect = FX_SOLID;
        }
    }

    g_commands++;
    Control::setOverride(s);
    g_dirty = true;
}

void onMessage(char *topic, byte *payload, unsigned int length) {
    if (length >= sizeof(g_buf)) return;

    // The library reuses its own buffer for the payload, so copy before doing
    // anything that might publish.
    static char body[512];
    size_t n = length < sizeof(body) - 1 ? length : sizeof(body) - 1;
    memcpy(body, payload, n);
    body[n] = 0;

    if (strstr(topic, "/release")) {
        Control::releaseOverride();
        g_commands++;
        g_dirty = true;
        return;
    }
    if (strstr(topic, "/set")) handleSet(body, n);
}

bool connectNow() {
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "lixie-%s", g_id);

    char willTopic[96];
    snprintf(willTopic, sizeof(willTopic), "%s/status", g_base);

    client.setBroker(cfg.mqtt_host, cfg.mqtt_port);

    // Last will marks the clock unavailable in HA if it drops off without
    // saying goodbye, which is the normal case for a power cut.
    bool ok = client.connect(
        clientId,
        cfg.mqtt_user[0] ? cfg.mqtt_user : nullptr,
        cfg.mqtt_pass[0] ? cfg.mqtt_pass : nullptr,
        willTopic, MQTT::QOS0, true, "offline", true);

    if (!ok) {
        snprintf(g_err, sizeof(g_err), "connect refused (check host/credentials)");
        return false;
    }

    g_connects++;
    g_err[0] = 0;
    NetWatch::noteAlive();

    client.publish(willTopic, "online", true);

    char sub[96];
    snprintf(sub, sizeof(sub), "%s/set", g_base);
    client.subscribe(sub);
    snprintf(sub, sizeof(sub), "%s/release", g_base);
    client.subscribe(sub);

    // Republish discovery on every connect, retained, so a Home Assistant
    // restart re-learns the entities without the clock needing one.
    g_discovered = publishDiscovery();
    publishState();
    publishDiag();
    return true;
}

}  // namespace

void MqttHa::begin() {
    String did = System.deviceID();
    const char *s = did.c_str();
    size_t n = strlen(s);
    snprintf(g_id, sizeof(g_id), "%s", n > 6 ? s + n - 6 : s);

    snprintf(g_base, sizeof(g_base), "lixie/%s", g_id);
    snprintf(g_name, sizeof(g_name), "Lixie Clock %s", g_id);
}

void MqttHa::tick() {
    if (!configured() || !WiFi.ready()) return;

    if (client.isConnected()) {
        client.loop();
        NetWatch::noteAlive();   // the broker link is live

        uint32_t now = millis();
        if (g_dirty || (now - g_lastHeartbeat) >= HEARTBEAT_MS) {
            g_dirty = false;
            g_lastHeartbeat = now;
            publishState();
            publishDiag();
        }
        return;
    }

    uint32_t now = millis();
    if (now - g_lastAttempt < g_backoff) return;
    g_lastAttempt = now;

    if (connectNow()) {
        g_backoff = RECONNECT_MIN_MS;
        g_lastHeartbeat = now;
    } else {
        g_backoff = min(g_backoff * 2, RECONNECT_MAX_MS);
    }
}

bool MqttHa::connected() { return client.isConnected(); }
bool MqttHa::configured() { return cfg.mqtt_host[0] != 0; }
uint32_t MqttHa::connectCount() { return g_connects; }
uint32_t MqttHa::commandCount() { return g_commands; }
const char *MqttHa::lastError() { return g_err; }
void MqttHa::notifyChanged() { g_dirty = true; }
