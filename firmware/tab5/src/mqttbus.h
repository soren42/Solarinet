// mqttbus.h — shared MQTT transport for the SolariNet Authenticator.
//
// One TLS MQTT connection is shared by every feature that needs the broker:
//   - approvals.*  (subscribes auth/request/#, publishes auth/response/<id>)
//   - notify.*     (subscribes notify/# for background crit/warn toasts)
// Features register a topic filter + handler; the bus (re)subscribes them on
// every (re)connect and dispatches inbound messages by MQTT topic match. This
// avoids two separate WiFiClientSecure/PubSubClient stacks on the device.
#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

namespace solari {

class MqttBus {
 public:
  using Handler =
      std::function<void(const char* topic, const uint8_t* payload, unsigned len)>;

  void begin();          // configure server + TLS from Config (call once)
  void loop();           // pump: connect/reconnect + inbound dispatch
  bool connected() const;

  // Register a subscription. Safe to call before begin(); the filter is
  // (re)subscribed automatically on each broker (re)connect.
  void subscribe(const String& topicFilter, Handler handler);

  // Publish (QoS1 by default). Returns false if not currently connected.
  bool publish(const String& topic, const String& payload, uint8_t qos = 1,
               bool retain = false);

 private:
  struct Sub { String filter; Handler handler; };

  void onMessage_(char* topic, uint8_t* payload, unsigned len);
  void resubscribeAll_();

  std::vector<Sub> subs_;
  uint32_t nextReconnectMs_ = 0;
};

MqttBus& mqttBus();

// MQTT topic filter match supporting '+' (single level) and '#' (multi level).
bool mqttTopicMatch(const String& filter, const String& topic);

}  // namespace solari
