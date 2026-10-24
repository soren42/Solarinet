# F-06 Tab5 approval binding contract

**Status (2026-10-23):** v2 broker and firmware implementation is in the local
candidate. Twenty broker unit tests and the host C++ canonical-vector test
pass; disposable-MQTT end-to-end, pinned-core firmware build, and hardware
verification remain open. The dashboard integration must remain disabled until
every release gate below passes.

## Observed state

- The earlier source revision published unsigned request JSON and used
  plaintext MQTT. The current local candidate adds broker-signed requests and
  CA-verified TLS; it has not yet been firmware-compiled or hardware-tested.
- The current local candidate signs responses with the canonical request digest,
  binding the decision to the displayed fields; deployment behavior is not yet
  verified.
- The dashboard approval integration is disabled by default and denies login
  if an enabled broker call fails.
- Trust material is imported through USB serial commands, not the setup wizard.

These facts mean the finding remains open when approvals are enabled. The
existing response signature does not authenticate the request display.

## Selected architecture

Use an out-of-band, one-time USB serial provisioning ceremony for broker trust
material. Do not compile installation-specific trust pins into general firmware
or learn them over MQTT. The device stores the imported broker signing public
key and MQTT CA bundle in its encrypted `secrets` NVS partition. The operator
compares the device-displayed fingerprint with the broker fingerprint obtained
over an independently authenticated administration channel. Rotation imports a
new key under a distinct key ID alongside the current key, verifies both, then
removes the old key after the overlap window.

The broker stores a dedicated P-256 request-signing private key in a 0600 file
owned by the broker service account and outside the repository. It is distinct
from every device response key. The broker refuses to start without the signing key.
The device trusts only imported broker public keys.

### Request envelope, protocol version 2

The broker signs every request before publish. The signature covers all
security-relevant values in this fixed order:

1. Protocol version
2. Broker signing key ID
3. Request ID
4. Issued-at Unix time
5. TTL seconds
6. Action
7. Subject
8. Detail
9. Source IP
10. Nonce
11. Target device ID (empty only when any enrolled device may approve)

Canonical bytes are a domain separator (`SOLARI-AUTH-REQUEST`, NUL, version
byte `0x02`), followed by each UTF-8 field encoded as a 4-byte unsigned
big-endian length and its exact bytes. No delimiter escaping or JSON
re-serialization is used. ECDSA P-256 / SHA-256 signs these bytes; the DER
signature is base64 encoded in `sig`.

The Tab5 verifies the signature and target before displaying the request. It
rejects malformed or overlong fields, unsupported versions, missing trust
material, expired requests, timestamps beyond the allowed clock skew, and
requests received without a valid local clock. It checks `issued_at + ttl`
against wall time; the on-screen countdown cannot restart a delayed request.

### Response binding

The device computes SHA-256 over the canonical request bytes it verified and
includes that digest as `req_digest` in its signed response. The response
signature covers protocol version, request ID, decision, nonce, device ID, and
`req_digest`. The broker retains its canonical request digest in pending state
and rejects any response whose digest differs. This prevents a request from
being substituted between display and broker authorization.

### Channel and broker policy

- MQTT uses TLS on port 8883 with CA and hostname verification on the Tab5;
  there is no plaintext fallback on firmware.
- The broker requires TLS for non-loopback MQTT. Plaintext is allowed only for
  explicitly named disposable loopback tests.
- Mosquitto ACLs restrict the broker identity to request publish and response
  subscribe, and each Tab5 identity to request subscribe and its own response
  topic publish.
- The broker rejects unsigned responses, unsigned requests at the device,
  unknown signing key IDs, wrong-device replies, expired replies, and replays.
- Dashboard wiring remains `enabled: false` until the firmware, broker, TLS,
  and acceptance gates pass together.

## Acceptance tests

- Shared Python/C++ golden vectors cover empty fields, Unicode, embedded
  newlines, maximum-length fields, and exact canonical bytes.
- Change each displayed field while retaining the original ID and nonce; the
  device rejects every modified request before rendering it.
- Reject missing/bad signatures, unknown key IDs, wrong target device, expired
  requests, future-issued requests outside skew, and requests when local time is
  invalid.
- Verify the response's `req_digest`; a modified pending request with the same
  ID/nonce is rejected by the broker.
- Reject response replay after resolution and de-duplicate QoS redelivery by
  request ID.
- TLS tests reject an unknown CA, hostname mismatch, and port 1883 on firmware.
  A third broker credential cannot publish on `auth/request/#` under the ACL.
- Rotation tests accept current and next keys during overlap, then reject the
  retired key.
- Run the existing disposable-broker approve and deny round trip with protocol
  version 2; run the firmware trust-import and verification path on a physical
  Tab5 before enabling dashboard integration.

## Release gates

The feature is not production-ready until the broker, firmware, and trust
provisioning implement this contract; all software and hardware acceptance tests
pass; certificate hostname verification is confirmed on the pinned P4 core; and
flash encryption plus secure boot state are recorded for the physical device.
Until then, the dashboard integration stays disabled and broker failures remain
fail-closed.

## UNVERIFIED

- Firmware behavior on the physical Tab5 and pinned P4 core.
- USB serial provisioning and trust-fingerprint comparison UX.
- Mosquitto ACL and TLS state on the live broker host.
- Flash-encryption and secure-boot state of the device.
