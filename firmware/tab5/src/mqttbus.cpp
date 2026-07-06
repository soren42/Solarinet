#include "mqttbus.h"

#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "config.h"

namespace solari {

static MqttBus g_bus;
MqttBus& mqttBus() { return g_bus; }

// One TLS transport + one MQTT client shared by all features. For a self-hosted
// broker with a private CA, provision the CA (setCACert) — TODO(tls). Never
// ship setInsecure().
static WiFiClientSecure s_tls;
static PubSubClient s_mqtt(s_tls);

bool mqttTopicMatch(const String& filter, const String& topic) {
  int fi = 0, ti = 0;
  const int fn = filter.length(), tn = topic.length();
  while (fi < fn) {
    if (filter[fi] == '#') return true;  // matches the rest, any depth
    // Extract this filter level and this topic level.
    int fEnd = fi;
    while (fEnd < fn && filter[fEnd] != '/') ++fEnd;
    int tEnd = ti;
    while (tEnd < tn && topic[tEnd] != '/') ++tEnd;
    if (ti >= tn && fi < fn) return false;  // topic ran out early
    bool plus = (fEnd - fi == 1 && filter[fi] == '+');
    if (!plus) {
      if (fEnd - fi != tEnd - ti) return false;
      for (int k = 0; k < fEnd - fi; ++k)
        if (filter[fi + k] != topic[ti + k]) return false;
    }
    fi = fEnd; ti = tEnd;
    if (fi < fn && filter[fi] == '/') ++fi;
    if (ti < tn && topic[ti] == '/') ++ti;
  }
  return ti >= tn;  // both consumed exactly
}

void MqttBus::begin() {
  const auto& s = config().settings();
  s_tls.setTimeout(8);
  // TODO(tls): s_tls.setCACert(BROKER_CA_PEM); provision the broker CA and, for
  // :8883, verify it. Dev-only plaintext uses port 1883 (no TLS handshake).
  s_mqtt.setServer(s.mqttHost.c_str(), s.mqttPort);
  s_mqtt.setBufferSize(2048);  // approval + notify payloads are a few hundred B
  s_mqtt.setCallback([](char* t, uint8_t* p, unsigned l) {
    g_bus.onMessage_(t, p, l);
  });
}

bool MqttBus::connected() const { return s_mqtt.connected(); }

void MqttBus::subscribe(const String& topicFilter, Handler handler) {
  subs_.push_back({topicFilter, std::move(handler)});
  if (s_mqtt.connected()) s_mqtt.subscribe(topicFilter.c_str(), 1);
}

void MqttBus::resubscribeAll_() {
  for (const auto& sub : subs_) s_mqtt.subscribe(sub.filter.c_str(), 1);
}

void MqttBus::loop() {
  if (s_mqtt.connected()) {
    s_mqtt.loop();
    return;
  }
  uint32_t now = millis();
  if (now < nextReconnectMs_) return;
  nextReconnectMs_ = now + 5000;  // backoff so we don't hammer an offline broker

  const auto& s = config().settings();
  String clientId = "tab5-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = s_mqtt.connect(clientId.c_str(), s.mqttUser.c_str(),
                           s.mqttPass.c_str());
  if (ok) resubscribeAll_();
}

bool MqttBus::publish(const String& topic, const String& payload, uint8_t qos,
                      bool retain) {
  (void)qos;  // PubSubClient publish() is QoS0 on the wire; the broker + our
              // TTL/nonce contract make an occasional resend harmless.
  if (!s_mqtt.connected()) return false;
  return s_mqtt.publish(topic.c_str(), (const uint8_t*)payload.c_str(),
                        payload.length(), retain);
}

void MqttBus::onMessage_(char* topic, uint8_t* payload, unsigned len) {
  String t(topic ? topic : "");
  for (const auto& sub : subs_) {
    if (mqttTopicMatch(sub.filter, t)) sub.handler(topic, payload, len);
  }
}

}  // namespace solari
