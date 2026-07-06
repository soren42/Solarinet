// notify.h — SolariNet notification surface for the Tab5.
//
// Transport: MQTT over TLS. The device subscribes to `notify/#` on a Mosquitto
// broker. A tiny server-side shim (deploy/notify/senders/mqtt.py — a new
// notifyd sender) republishes each notify.events message to MQTT, so the Tab5
// never has to speak AMQP. See DESIGN.md "Notification transport" and the
// README for the shim. The payload is the same JSON contract notifyd already
// uses: {ts, severity, source, title, body, ...}.
#pragma once

#include <Arduino.h>
#include <functional>
#include <deque>

namespace solari {

enum class Severity { Info, Warn, Crit };

struct Notification {
  time_t ts = 0;
  Severity severity = Severity::Info;
  String source;
  String title;
  String body;
  String routingKey;  // original notify.events routing key, if present
};

class Notify {
 public:
  // Called on the main loop task. Maintains the MQTT connection (connect,
  // subscribe, reconnect with backoff) and drains inbound messages.
  void begin();
  void loop();

  bool connected() const;

  // Newest-first feed for the UI (bounded ring; older entries dropped).
  const std::deque<Notification>& feed() const { return feed_; }
  size_t unreadCount() const { return unread_; }
  void markAllRead() { unread_ = 0; }

  // UI registers this to get a toast for crit/warn as they arrive.
  using ToastFn = std::function<void(const Notification&)>;
  void onToast(ToastFn fn) { toast_ = std::move(fn); }

 private:
  void handleMessage_(char* topic, uint8_t* payload, unsigned len);

  std::deque<Notification> feed_;
  size_t unread_ = 0;
  ToastFn toast_;
  uint32_t nextReconnectMs_ = 0;
  static constexpr size_t kMaxFeed = 200;
};

Notify& notify();

}  // namespace solari
