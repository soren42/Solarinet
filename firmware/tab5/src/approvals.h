// approvals.h — push-approval (2FA / access-approval) for the Authenticator.
//
// The core of the device's mission. When someone authenticates to the SolariNet
// dashboard or Keycloak, the server-side broker (deploy/authbroker/) publishes a
// request to `auth/request/<id>`; this module surfaces an Approve/Deny prompt on
// the Tab5 with a short TTL, and publishes a SIGNED decision to
// `auth/response/<id>`. The broker verifies the signature with the device's
// enrolled PUBLIC key, so only this Tab5 (holding the private key in encrypted
// NVS) can approve. See DESIGN.md "Approval flow + signing scheme".
//
//   request  auth/request/<id>   {v,id,ts,ttl,action,subject,detail,source_ip,nonce}
//   response auth/response/<id>  {v,id,ts,decision,device_id,nonce,sig}
//   signed bytes: "<v>\n<id>\n<decision>\n<nonce>\n<device_id>"  (ECDSA-P256/SHA-256)
#pragma once

#include <Arduino.h>
#include <deque>
#include <functional>

namespace solari {

struct ApprovalRequest {
  String id;
  String action;     // "login" | "elevate" | "approve"
  String subject;    // who is authenticating, e.g. "jason"
  String detail;     // e.g. "SolariNet dashboard"
  String sourceIp;   // origin of the auth attempt
  String nonce;      // server random (base64); echoed + signed in the response
  time_t ts = 0;
  uint32_t ttl = 60;         // seconds the request is valid
  uint32_t receivedMs = 0;   // millis() at receipt, for the countdown
};

class Approvals {
 public:
  void begin();   // load/ensure device key, subscribe auth/request/#
  void loop();    // expire stale requests

  // The oldest still-pending request (what the UI shows), or nullptr.
  const ApprovalRequest* pending() const;
  size_t pendingCount() const { return queue_.size(); }
  uint32_t secondsLeft(const ApprovalRequest& r) const;

  // Operator actions from the UI. Sign + publish the decision, then drop it.
  void approve();
  void deny();

  // The device identity the broker enrolls (matches [device.<id>] in the
  // broker config). Defaults to "tab5-<efuse-mac>"; overridable in settings.
  String deviceId() const;

  // UI hook: called when a new request arrives (to wake the screen / toast).
  using AlertFn = std::function<void(const ApprovalRequest&)>;
  void onAlert(AlertFn fn) { alert_ = std::move(fn); }

  // First-boot/enrollment: generate a fresh P-256 keypair into NVS and return
  // the public key (uncompressed SEC1, hex) to paste into the broker config.
  // Returns "" on failure. Existing key is NOT overwritten unless force=true.
  String provisionDeviceKey(bool force = false);
  String publicKeyHex() const;   // enrolled public key, or "" if none

 private:
  void handleRequest_(const uint8_t* payload, unsigned len);
  bool respond_(const ApprovalRequest& r, const char* decision);
  bool signCanonical_(const String& canonical, String& sigB64Out) const;

  std::deque<ApprovalRequest> queue_;
  AlertFn alert_;
  static constexpr size_t kMaxQueue = 8;
};

Approvals& approvals();

}  // namespace solari
