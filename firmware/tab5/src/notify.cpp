#include "notify.h"

#include <ArduinoJson.h>

#include "config.h"
#include "mqttbus.h"

namespace solari {

static Notify g_notify;
Notify& notify() { return g_notify; }

static Severity parseSeverity(const char* s) {
  if (!s) return Severity::Info;
  if (!strcasecmp(s, "crit") || !strcasecmp(s, "critical")) return Severity::Crit;
  if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) return Severity::Warn;
  return Severity::Info;
}

void Notify::begin() {
  // Background notification surface: reuse the shared MQTT bus (mqttbus.*)
  // rather than a second connection. On the Authenticator this is secondary to
  // push-approval — it just raises crit/warn toasts; there is no Alerts tile.
  const auto& s = config().settings();
  mqttBus().subscribe(s.notifyTopic,
                      [this](const char* t, const uint8_t* p, unsigned l) {
                        handleMessage_((char*)t, (uint8_t*)p, l);
                      });
}

bool Notify::connected() const { return mqttBus().connected(); }

void Notify::loop() { /* the shared MqttBus pumps the connection; nothing here */ }

void Notify::handleMessage_(char* topic, uint8_t* payload, unsigned len) {
  JsonDocument doc;  // ArduinoJson 7 elastic doc
  if (deserializeJson(doc, payload, len)) return;  // ignore malformed

  Notification n;
  n.routingKey = topic ? String(topic) : String();
  n.ts = doc["ts"] | (long)time(nullptr);
  n.severity = parseSeverity(doc["severity"] | "info");
  n.source = String((const char*)(doc["source"] | ""));
  n.title = String((const char*)(doc["title"] | ""));
  n.body = String((const char*)(doc["body"] | ""));

  feed_.push_front(n);
  if (feed_.size() > kMaxFeed) feed_.pop_back();
  ++unread_;

  // Toast crit/warn immediately; info just accumulates in the feed.
  if (toast_ && n.severity != Severity::Info) toast_(n);
}

}  // namespace solari
