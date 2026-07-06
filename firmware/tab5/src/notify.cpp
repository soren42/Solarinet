#include "notify.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "config.h"

namespace solari {

static Notify g_notify;
Notify& notify() { return g_notify; }

// TLS-capable transport for MQTT. For a self-hosted broker with a private CA,
// load the CA cert (setCACert) — TODO: embed/provision the broker CA. For dev
// only, setInsecure() skips verification (do NOT ship that way).
static WiFiClientSecure s_tls;
static PubSubClient s_mqtt(s_tls);

static Severity parseSeverity(const char* s) {
  if (!s) return Severity::Info;
  if (!strcasecmp(s, "crit") || !strcasecmp(s, "critical")) return Severity::Crit;
  if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) return Severity::Warn;
  return Severity::Info;
}

void Notify::begin() {
  const auto& s = config().settings();
  s_tls.setTimeout(8);
  // TODO(tls): s_tls.setCACert(BROKER_CA_PEM);  // provisioned broker CA
  // Dev fallback (insecure) — leave disabled for production:
  // s_tls.setInsecure();
  s_mqtt.setServer(s.mqttHost.c_str(), s.mqttPort);
  s_mqtt.setBufferSize(2048);  // notify payloads can be a few hundred bytes
  s_mqtt.setCallback([](char* t, uint8_t* p, unsigned l) {
    g_notify.handleMessage_(t, p, l);
  });
}

bool Notify::connected() const { return s_mqtt.connected(); }

void Notify::loop() {
  if (s_mqtt.connected()) {
    s_mqtt.loop();
    return;
  }
  // Reconnect with simple backoff so we don't hammer the broker offline.
  uint32_t now = millis();
  if (now < nextReconnectMs_) return;
  nextReconnectMs_ = now + 5000;

  const auto& s = config().settings();
  String clientId = "tab5-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = s_mqtt.connect(clientId.c_str(),
                           s.mqttUser.c_str(), s.mqttPass.c_str());
  if (ok) {
    s_mqtt.subscribe(s.mqttTopic.c_str(), /*qos=*/1);
  }
}

void Notify::handleMessage_(char* topic, uint8_t* payload, unsigned len) {
  JsonDocument doc;  // ArduinoJson 7 elastic doc
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) return;  // ignore malformed

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
