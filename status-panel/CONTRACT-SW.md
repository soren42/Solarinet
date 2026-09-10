# Status Panel — Standalone WiFi Mode: Build Contract

`v1.0 · 2026-08-11 · Lead: Fable 5 · task #8 · extends CONTRACT.md,
CONTRACT-CP.md (both remain binding); architecture decided by the operator
2026-08-11 — the four §1 decisions are settled and not up for re-litigation`

> v1.0 incorporates the P0 cross-lab consult (gpt-5.6 codex, 22 MUST +
> 2 SHOULD, verdict REVISE). §13 dispositions are binding and WIN over
> §2–§11 where they conflict. Headline changes: PROVISION gains offsets +
> generations (retry-safe), full mTLS identity spec (SAN/EKU/chain/time/
> revocation), per-transport seq state, per-edge power episodes, corrected
> episode-namespace masking, and resequenced phases (SW5 lands first).

Operator ask (from the task #8 roadmap + the 2026-08-11 architecture call):
the panel runs untethered from lithium — WiFi to the network, battery as
UPS ride-through — with USB serial retained as the fallback transport.

## 1. Settled architecture decisions (operator, 2026-08-11)

1. **Thin client.** The firmware speaks the EXISTING binary protocol
   (protocol.{c,h}, version 0x01) over a TLS TCP connection to a
   `solariPanel` daemon listener. The daemon remains the sole JSON/auth
   brain; the firmware gains transport code only. NOT chosen: fw-side
   /api/panel JSON fetch (fat client), MQTT push.
2. **mTLS with per-device client certificates** issued by the chlorine
   step-ca (10.7.0.10). CA pinned in both directions. NOT chosen: TLS-PSK,
   bearer token.
3. **Provisioning over USB** via new protocol frames + a host tool writing
   into on-flash credential storage. No secrets in the repo, the UF2, or
   the build tree. NOT chosen: compile-time creds, first-boot AP mode.
4. **Battery = ride-through + alarm.** USB is normal power. On VBUS loss:
   hard brightness cap, on-panel power-loss indicator, AND the alarm path
   arms (panel-site power loss is itself alarm-worthy).

## 2. Architecture

```
xenon API                      xenon daemon (moves from lithium)         RP2350 fw
┌────────────────────┐ poll 5s ┌──────────────────────────────┐  mTLS   ┌─────────────────┐
│ /api/panel (+cmds) │◄────────│ solariPanel                  │◄════════│ WiFi (CYW43 +   │
│ /api/panel/state   │  HTTPS  │  serial mode (unchanged)     │ binary  │ lwIP + mbedTLS) │
└────────────────────┘ session │  + NEW listen mode :7443     │ protocol│ USB serial =    │
                               └──────────────────────────────┘  frames │ fallback, wins  │
                                        ▲ USB serial (bench/fallback)   │ when active     │
                                        └───────────────────────────────┴─────────────────┘
```

- One daemon binary, two transports, selected by config: `serial=` (today's
  mode, unchanged) and/or `listen=` (new). The deployed instance moves to
  xenon running listen mode; lithium is freed from the chain. Serial mode
  survives untouched for bench work and as the fallback path.
- The PANEL initiates the TCP connection (it sits on DHCP WiFi; the daemon
  has the stable address). Service name per [[service-url-convention]]:
  `panelgw.akoria.net`, dedicated TLS port 7443 (raw TLS, not HTTP — the
  subdomain convention's :443 SNI vhosts do not apply; document the port).
- Same frames, same CRC, same version 0x01 both transports. On each TCP
  connect the daemon sends HELLOREQ exactly as it does at serial link-up
  (CONTRACT-CP §10 capability gate applies per-connection); wire seq resets
  with the connection, so the serial-restart seq-adoption escape hatch
  (CONTRACT.md) is not needed on TCP — a new connection IS the reset signal.

## 3. Transport arbitration (normative)

- USB wins. If valid frames arrived on USB serial within the last 15 s, the
  firmware treats USB as the active transport and closes/suspends the WiFi
  connection. WiFi (re)connects only when USB has been silent ≥15 s.
- Double-drive is tolerable, not catastrophic, by inherited design: commands
  are idempotent, firmware dedupes on cmdId (CONTRACT-CP §10), and both
  daemons converge on the same API. The arbitration rule exists to make the
  steady state single-transport, not to paper over an unsafe protocol.
- STATE frames go to whichever transport is active. The daemon's
  per-connection capability gate keeps command forwarding honest.
- Firmware exposes link status: PANEL_EV_LINKLOST/LINKBACK fire per active
  transport as today; a new STATE reserved-byte bit or CONFIG flag is NOT
  added — transport identity is visible in the daemon journal, which is
  sufficient (revisit only if ops need it on the dashboard).

## 4. Protocol amendment (SW5 — additive, no version bump)

Additive frame types are safe under version 0x01 (both sides ignore unknown
types — the CONTRACT-CP precedent). protocol.h gains:

- `0x05 PROVISION` (host→panel, USB TRANSPORT ONLY — firmware MUST reject it
  on WiFi; provisioning over the network is forbidden): TLV payload,
  u8 itemId + u16 len + bytes. Items: 1 ssid, 2 psk, 3 serverHost,
  4 serverPort (u16), 5 caCertDER, 6 clientCertDER, 7 clientKeyDER,
  8 commit (empty; atomically writes the staged set), 9 wipe (empty;
  erases credential storage). Certificates/keys arrive in ≤192-byte chunks
  (protocol.h's MAX_PAYLOAD is 2048 — the 192-byte chunk is a deliberate
  conservative choice for USB CDC lockstep latency, NOT a frame limit;
  protocol.h is the single normative size authority).
- `0x86 PROVACK` (panel→host): u8 itemId, u8 status (0 ok, 1 crc/format,
  2 storage-full, 3 rejected-on-wifi, 4 commit-invalid: staged set
  incomplete). The host tool is lockstep: one TLV, await PROVACK.
  [§13 D1/D2/D4 amend this frame pair — offsets, generations, corrected
  size rationale, and an expanded status set are normative there.]
- Flash layout: a NEW reserved region ahead of the existing 4 KB config
  sector — 12 KB (3 sectors) for the credential store, CRC'd record, same
  journaling discipline as the AW config sector (PICO_FLASH_ASSUME_CORE1_SAFE
  lesson applies). Plain-flash storage is accepted: the threat model is a
  device on the operator's own wall; physical possession already defeats it.
- Episode-ID namespace partition (extends CONTRACT-LC §3.2 usage): server
  auto-increment IDs live below 0x80000000; server CRC episodes occupy
  0x80000000–0xBFFFFFFF; 0xC0000000–0xFFFFFFFF is RESERVED FOR
  FIRMWARE-LOCAL episodes. NOTE: the CURRENT panel.php masking
  `0x80000000|(crc32&0x7fffffff)` spills into the 0xC range and MUST
  change to `0x80000000|(crc32&0x3fffffff)` — §13 D17. The power-loss
  alarm (§7) is the first firmware-local tenant.

## 5. Daemon listener mode (SW1)

- Config additions (solari-panel.conf): `listen=7443`, `tlsCert=`, `tlsKey=`
  (server identity, issued by chlorine), `clientCa=` (chlorine root; the
  ONLY accepted issuer), `allowedCn=` (exact CN match, default `panel-01`).
  `serial=` and `listen=` may coexist; each connection/port runs the same
  session state machine.
- TLS server-side via OpenSSL (already in the daemon's link closure through
  libcurl; no new dependency class). mbedTLS stays firmware-side only.
- Verification: chain to `clientCa` AND CN == `allowedCn`. Anything else:
  TCP close, one journal line with peer IP + presented CN. No fallback to
  plaintext, no cert-less mode, not even for testing — the bench path is
  serial, which needs no TLS.
- One panel: the listener accepts ONE concurrent session; a second connect
  supersedes the first (newest wins — a rebooting panel must not be locked
  out by its own half-dead old connection).
- Hardware-free test harness: the host suite drives the listener over a
  loopback socket with `openssl s_client`-style scripted peers — wrong CA,
  wrong CN, expired cert, mid-frame disconnect, supersession — plus a
  pipe-fed fake panel proving HELLOREQ-at-connect and the capability gate.

## 6. Firmware WiFi transport (SW2)

- CYW43 + lwIP + mbedTLS via pico-sdk (`pico_lwip_mbedtls`, altcp_tls
  client). Build prerequisite on lithium: `git submodule update --init` in
  ~/pico/pico-sdk (cyw43-driver, lwip currently uninitialized).
- Connection policy: join SSID from the credential store; connect to
  serverHost:serverPort with the stored client cert; verify server chain
  against stored CA. Reconnect with exponential backoff 1 s→60 s cap,
  jittered. AP loss / DHCP renew / server restart are all the same case:
  drop, backoff, reconnect (the UDR7 radio cutover to APs makes AP restarts
  a live scenario, not a corner).
- The existing staleness machinery is transport-agnostic and unchanged:
  no snapshot ≥ CONTRACT.md staleness bound → LINKLOST behavior, regardless
  of which transport is nominally up.
- Unprovisioned device (empty/invalid credential store): WiFi stays off,
  USB serial works exactly as today. Standalone mode is strictly additive.
- RAM/flash budget gate: the two-build reproducibility pair must remain
  byte-identical (SOURCE_DATE_EPOCH pinning extends over lwIP/mbedTLS), and
  the host suite must still link — renderer/state code stays hardware-free;
  all lwIP/mbedTLS usage lives behind the transport seam like panelLink.c.

## 7. Power: ride-through + alarm (SW4)

- VBUS sensing per Pico 2 W reference (WL_GPIO2 via CYW43 — note: readable
  only once cyw43 is initialized; on USB-only bench builds the feature
  self-disables).
- On VBUS loss: (a) brightness hard-caps at PANEL_BATT_BRIGHT_MAX (build
  default 20 %, field-calibratable like PANEL_LUX_FULL); (b) a power-loss
  glyph renders in the ticker region (design per DESIGN-BRIEF addendum,
  before implementation — styleguide-first rule); (c) the alarm arms with a
  firmware-local episode `0xC0000000 | bootSalt`, ackable exactly like any
  episode (any button; CONTROL ackAlarm). VBUS restore clears the episode
  and lifts the cap.
- Alarm tone under battery follows the existing night-profile rules
  (CONTRACT.md) — power loss does not override quiet-hours handling.
- Power budget is MEASURED, not asserted: acceptance records current draw at
  battery-cap brightness with WiFi associated, and the resulting runtime on
  the JST pack. No runtime number appears in any doc until measured.

## 8. Provisioning & PKI ops (SW3 + runbook)

- Host tool `status-panel/tools/panelProv` (C, per project language policy):
  reads a conf file (ssid/psk paths, cert/key/ca paths, host/port), speaks
  §4 over the serial device, lockstep, prints per-item PROVACK. A `--wipe`
  flag sends item 9. Tool refuses to run against a WiFi-connected panel
  (it can't — PROVISION is USB-only by firmware rule; the tool just gets
  PROVACK status 3 and reports it plainly).
- Cert issuance (ops runbook, not code): step-ca on chlorine issues
  `panel-01` client cert + the daemon's `panelgw.akoria.net` server cert.
  Lifetimes per chlorine policy; renewal = re-run panelProv with fresh
  cert/key (annual bench visit — accepted for a one-device fleet; on-device
  ACME renewal is explicitly out of scope). Revocation = step-ca revoke +
  daemon restart (the listener loads CRL/denylist at start; a live-reload
  path is out of scope).
- Secrets discipline (red line): key material lives only on chlorine, in
  the panel's flash, and transiently on the provisioning bench under
  run/-style 0600 paths. Never in git, never in packets, never in logs.

## 9. Components & ownership (cross-lab per review doctrine)

| # | Component | Author | Reviewer |
|---|-----------|--------|----------|
| SW1 | Daemon listen mode + TLS + harness (C) | gpt-5.5 codex | fable-5/opus-4.8 |
| SW2 | Firmware WiFi transport + arbitration (C) | Claude | gpt-5.5 codex |
| SW3 | PROVISION/PROVACK + cred store + panelProv (C) | Claude | gpt-5.5 codex |
| SW4 | VBUS/battery/power-loss alarm (C) | Claude | gpt-5.5 codex |
| SW5 | protocol.h amendment §4 | Lead | both consume |
| SW6 | Docs: manual section, ops runbook, DESIGN-BRIEF addendum | Lead | operator |

Branch `feat/standalone-wifi` off main. No merges, no flash, no daemon
deploy without the operator (red line 1). Contract consult (cross-lab, on
this document) BEFORE SW2 starts; SW1 may start immediately — it is pure
server-side and hardware-free.

## 10. Acceptance (observable; UNVERIFIED items named at return)

1. Listener harness: all §5 rejection cases + supersession + capability
   gate proven in the daemon host suite (no hardware).
2. Provisioning round trip: panelProv → PROVACKs → power-cycle → credential
   store survives (CRC-valid) → wipe → store empty; PROVISION over WiFi
   rejected with status 3.
3. WiFi E2E: provisioned panel, NO USB data connection, renders live fleet
   state within one poll cadence; command round trip (page → queue → WiFi
   forward → STATE lastCmdId → applied) inside the CONTRACT-CP ≤12 s bound.
4. mTLS negative cases live: wrong-CA cert and revoked cert both refused
   (daemon journal shows the refusal); wrong server cert refused by fw.
5. Arbitration: with WiFi live, plug USB into a daemon-running host →
   firmware switches to USB ≤15 s (WiFi session drops on the xenon
   journal); unplug → WiFi resumes via backoff. No duplicate command
   application throughout (journal + lastCmdId evidence).
6. AP-bounce resilience: restart the serving AP; panel reconnects
   unattended; LINKLOST/LINKBACK behavior per staleness bounds.
7. Power: pull wall power under WiFi — panel stays up on battery,
   brightness caps, power-loss glyph shows, alarm arms with a 0xC-range
   episode, button ack works; restore clears it. Measured draw + runtime
   recorded in the return packet.
8. Regression: full existing gate — fw host suite + parity fixture green,
   two-build byte-identical UF2 pair, daemon make test, serial-mode bench
   round trip unchanged, JS/PHP suites green.
9. Docs shipped (SW6): manual gains a standalone-mode + power-loss section
   (print-checked), ops runbook covers issuance/renewal/revocation/
   provisioning, DESIGN-BRIEF addendum for the power-loss glyph.

## 11. Implementation plan (phases; each ends at a review gate)

- **P0 — contract consult.** Cross-lab review of this document (codex),
  dispositions folded in as §12 (the CONTRACT-CP v1.1 pattern). Exit: the
  operator approves the contract.
- **P1 — daemon listener (SW1) + protocol amendment (SW5).** Hardware-free;
  parallel with nothing blocking it. Exit: §10.1 green, cross-lab review
  passed. Deliverable runs on xenon in listen mode against zero panels.
- **P2 — provisioning (SW3).** Firmware cred store + PROVISION handling +
  panelProv, all provable over the socat pty harness before touching
  hardware. Exit: §10.2 on the bench panel (USB only, one flash).
- **P3 — firmware WiFi transport (SW2).** Builds on P1 (a live listener to
  test against) and P2 (creds to connect with). Bench-first: xenon listener
  + bench AP. Exit: §10.3–10.6, cross-lab review, flash gate.
- **P4 — power (SW4).** DESIGN-BRIEF addendum first, then implementation.
  Exit: §10.7 with measured numbers.
- **P5 — integration + deploy + docs (SW6).** Daemon deploy to xenon
  (listen+serial), panelgw DNS + certs issued, panel reprovisioned for
  production, lithium daemon retired FROM THE CHAIN but binary left
  installed (bench fallback), full §10.8–10.9, publish/merge on the
  operator's word.

Sequencing: P1 ∥ P2 after P0; P3 needs both; P4 ∥ P3 is allowed (different
subsystems) but flashes serialize through the flash gate; P5 last. Each
phase returns a RETURN-SW<n>.md packet with its UNVERIFIED section.

## 12. Out of scope

MQTT push (separate roadmap item), multi-panel support, on-device cert
renewal (ACME), provisioning over WiFi or any network path, first-boot AP
mode, listener live-reload of certs/CRL, dashboard surfacing of the active
transport, battery telemetry in STATE frames (revisit under "full portable
mode" if that mode is ever wanted — it was explicitly not chosen).

## 13. v1.0 dispositions (P0 consult, binding; supersede §2–§11 on conflict)

All 22 MUSTs and both SHOULDs accepted (D2 and D19 accepted-amended).
Numbering follows the consult findings.

**Provisioning wire (D1, D2, D3, D4 — replace §4's PROVISION/PROVACK).**
- PROVISION TLV becomes: u8 itemId, u8 generation, u16 offset, u16 len,
  bytes. `generation` is a transfer epoch chosen by the host at session
  start; a TLV whose generation differs from the staging area's discards
  the staged set and starts fresh (that IS the abort/replace mechanism).
  PROVACK becomes: u8 itemId, u8 status, u8 generation, u16 nextOffset.
  Retransmission after a lost PROVACK is safe: an offset ≤ already-accepted
  re-acks with the current nextOffset and writes nothing (idempotent).
- Host timeout 2 s per TLV, 3 retries, then abort with a plain report.
  Staged (uncommitted) data lives in RAM only — a reset mid-transfer
  costs nothing and the ACTIVE record is untouched until commit succeeds.
- D2: 192-byte chunks stand as a latency choice; protocol.h MAX_PAYLOAD
  (2048) is the only normative frame bound (§4 corrected inline).
- Item formats (D3): all strings UTF-8, NOT NUL-terminated, length from
  the TLV. Bounds: ssid ≤32, psk 8–63 (WPA2-PSK passphrase rules),
  serverHost ≤253 (RFC 1035), serverPort u16 LE ≠ 0, caCertDER ≤2048,
  clientCertDER ≤4096 (leaf-then-intermediates concatenated DER, D8),
  clientKeyDER ≤2048. Re-sending an itemId at offset 0 in the same
  generation replaces that item's staging. Commit validates: every
  required item present, all DER parseable by mbedTLS, client key matches
  client leaf public key, CA cert is a CA (basicConstraints), serverHost
  syntactically valid. Any failure → status 4, staged set kept for
  correction, ACTIVE record untouched. Wipe (item 9) clears BOTH staging
  and the active record.
- PROVACK status set (D4): 0 ok, 1 format-invalid (well-framed TLV failed
  item validation — frame-CRC failures never reach the handler; host
  timeout/retry covers them), 2 storage-full, 3 rejected-on-wifi,
  4 commit-invalid, 5 offset-mismatch (nextOffset tells the host where to
  resume), 6 flash-io-error, 7 crypto-invalid, 8 busy.

**Credential flash store (D5 — replaces §4's flash paragraph).** Reserved
map: the LAST 16 KiB of flash = 3× 4 KiB credential sectors (A record,
B record, spare) + the existing 4 KiB AW config sector, exact offsets in a
shared header with a CMake/link-time assert that the image never reaches
them (AW precedent, made explicit). Record: magic, version, generation
(u32 monotonic), per-item lengths, payload, CRC32 over all of it. Commit =
write the FULL record to the inactive slot (page-padded writes, dual-core
safe via flash_safe_execute + PICO_FLASH_ASSUME_CORE1_SAFE), verify by
read-back, THEN bump generation — the old record stays valid at every
interrupted erase/program point. Boot selects the highest-generation
CRC-valid record; none valid → unprovisioned behavior (§6). Power-loss
recovery is an explicit host-suite case (corrupt A, corrupt B, corrupt
both, torn write simulated by truncated record).

**mTLS identity, both directions (D6, D8, D9 — amend §5/§6).**
- Firmware verifies the daemon: chain to stored CA AND SAN DNS
  `panelgw.akoria.net` (mbedTLS hostname verification set; SNI sent) AND
  serverAuth EKU. CN-only or wrong-host certs are refused.
- Daemon verifies the panel: chain to `clientCa` AND clientAuth EKU AND
  SAN URI/DNS `panel-01.panel.akoria.net` (config `allowedSan=`,
  replacing §5's `allowedCn=`; CN remains display-only). The step-ca
  provisioner template for panel certs pins EKU + SAN shape; duplicate
  identity issuance is prevented by template policy, recorded as an
  operational invariant in the runbook.
- clientCertDER carries leaf then intermediates (concatenated DER);
  the daemon presents its full chain; both sides anchor on roots only.
  Acceptance exercises the REAL chlorine chain, not a bench self-sign.

**Certificate time (D7 — new normative design).** RP2350 has no trusted
boot clock. Before the FIRST TLS attempt of a boot, firmware obtains time
via SNTP against the gateway NTP (address provisioned as a new TLV item
10 ntpHost, ≤253, optional — absent means use serverHost's address);
retry/backoff on failure and TLS is NOT attempted until time is plausible
(> the firmware build timestamp, compiled in). Wall time is kept by the
RP2350 AON timer while powered; it does not survive power loss and is
re-fetched. No persisted-time write path (flash wear for no gain). There
is NO validity-bypass mode. Acceptance includes expired server cert,
not-yet-valid server cert, and expired client cert — all must refuse.

**Revocation (D10 — replaces §8's sentence).** Serial denylist, not CRL
(one device, one issuer): config `denylist=` names a file of one hex
serial per line; the daemon loads it at start and refuses any presented
cert whose serial matches (checked after chain verification). Runbook:
`step ca revoke <serial>` on chlorine, append the serial to the denylist
on xenon (path documented there), `systemctl restart solari-panel`.
Missing file = empty denylist (logged once); unreadable file = fatal at
start (fail closed, loudly). Freshness is operational (the runbook step),
not protocol — accepted for this fleet size.

**Listener TLS/socket profile (D11 — amends §5).** OpenSSL becomes an
explicit build dependency (`-lssl -lcrypto`, headers checked at
configure). TLS 1.2 minimum, 1.3 preferred; AEAD suites only. Bind
address config `listenAddr=` (default the LAN interface address, never
0.0.0.0 unconsidered). Deadlines: TLS handshake 10 s, read idle 90 s
(3× the 30 s STATE heartbeat), write 5 s — expiry closes the socket.
One handshake in flight at a time (a second SYN queues in the backlog,
cap 4); SIGPIPE ignored process-wide; partial frame buffers freed on
close. Key/cert files must be 0600/0644 root-or-daemon-owned or the
listener refuses to start.

**Session supersession (D12 — amends §5).** "Newest wins" applies ONLY
after a connection COMPLETES mTLS + SAN authorization. Replacement is an
atomic generation swap: old socket closed, its buffered frames and parser
state discarded, its capability gate reset, and any in-flight state POST
from it dropped (session-generation tag checked before posting). An
unauthenticated or failed-handshake connection can never evict the active
session. API polling is SINGLETON per daemon process regardless of how
many transports are configured — one poller, one cookie jar, sessions
consume from the same queue snapshot.

**Sequence state across transports (D13 — amends §2/§3).** Wire seq is
PER-TRANSPORT state on both ends, not global. Firmware keeps independent
lastAppliedSeq per transport; on active-transport switch it ADOPTS the
first CRC-valid snapshot on the newly active transport (explicit adopt,
not RFC1982 catch-up — the CONTRACT.md serial escape hatch stays for
serial only). The daemon resets its TX seq per TCP connection. Host-suite
cases: USB→WiFi→USB with lower/equal/wrap-boundary seq values, plus a
buffered stale frame from the losing transport arriving post-switch
(must be discarded with the transport, per D12).

**Arbitration activity definition (D14 — amends §3).** "USB activity" =
any CRC-valid frame of a KNOWN host→panel type (SNAPSHOT, PING, HELLOREQ,
CONTROL, PROVISION). CRC-valid unknown types do NOT qualify (version-skew
safety). The first qualifying USB frame suspends WiFi immediately (close,
not linger). WiFi resume waits 15 s of USB silence, EXCEPT detected CDC
disconnect (USB unplug/host gone) which bypasses the delay. PING alone
keeping USB active is intentional and now stated. LINKLOST/LINKBACK
remain bound to SNAPSHOT staleness only — a transport switch with fresh
snapshots must not blip them.

**Power episodes (D15, D16 — amend §7).**
- Every VBUS-loss EDGE allocates a fresh episode:
  `0xC0000000 | (edgeCounter & 0x3FFFFFFF)` where edgeCounter starts from
  a per-boot salt and increments per edge — two outages in one boot are
  two episodes; ack of the first cannot mute the second.
- Composition with server alarms: alarm sources are independent episodes
  ORed into "tone active if ANY unacked". Ack (button or CONTROL) applies
  to ALL currently-unacked episodes (matches the existing any-button-acks
  UX — an operator at the panel is acknowledging "the noise", and the
  journal records which episodes that covered). A server SNAPSHOT never
  clears the local power episode; VBUS restore clears ONLY the power
  episode and never suppresses a live server episode. Display: the alarm
  inlay shows the server topAlert when one exists, else the power-loss
  text; the battery glyph shows whenever on battery, alarm or not.
  Acceptance covers power loss during acked AND unacked server alarms.

**Namespace enforcement (D17 — amends §4; touches panel.php).** The
server's episode masking changes to `0x80000000 | (crc32 & 0x3fffffff)`
(both pool and vital namespaces — the current `& 0x7fffffff` spills into
the firmware-local range). Auto-increment headroom: alertEvent.eventId
approaching 0x80000000 is absurd at this fleet's rates but gets a
comment at the query site naming the bound. Firmware-local generation
masks into 0xC0000000+ by construction (D15). This panel.php change
ships in P1 with SW5 (it is a server-side two-line diff + test).

**VBUS/battery hardware contract (D18 — amends §7).** cyw43 init is
UNCONDITIONAL in production firmware (VBUS sensing must work provisioned
or not; the §7 "self-disables" clause is struck — it applied only to the
host-suite build, which stubs the PAL anyway). VBUS debounce 100 ms both
edges; boot-on-battery is a VBUS-loss state from t=0 (cap + glyph + one
episode). Approved pack: a protected single-cell LiPo with JST-PH on the
Galactic Unicorn battery input per Pimoroni's spec — the board has NO
charger and NO power-path mixing: battery and USB must not be assumed to
combine, and the exact pack (capacity, protection cutoff, polarity
CONFIRMED against the silkscreen) is recorded in the P4 design addendum
BEFORE first connection. Brownout behavior at protection cutoff = clean
death, acceptable; no low-battery UX in this contract (out of scope with
battery telemetry).

**Power acceptance gate (D19 — amends §10.7/§11 P4, accepted-amended).**
P4 becomes measure-then-ratify: (a) characterize draw + runtime at the
capped brightness with WiFi associated, fixed test conditions recorded
(pack, starting charge, ambient, workload = live snapshots + one alarm
cycle); (b) the operator ratifies a minimum ride-through from the
measurement; (c) acceptance then passes only if a repeat run meets the
ratified minimum, including ≥3 VBUS loss/restore cycles. No number is
invented ahead of (a) — but P4 cannot exit on a measurement alone.

**Firmware resource + fault gates (D20 — amends §6/§10).** Added gates:
link-time image cap (must clear the 16 KiB reserved region, D5 assert);
heap/stack high-water logged over ≥1 h reconnect soak (forced AP bounce
every 5 min) with zero growth trend; fault matrix exercised on bench —
wrong PSK, DHCP timeout, DNS failure, TLS handshake failure, AP loss,
server refusal, malformed credential store — each recovers or degrades
per contract with bounded jittered backoff and NO secret material in any
log line (grep-audited).

**Acceptance additions (D21 — extend §10).** New observable cases: lost
PROVACK retry + out-of-order offset (status 5 path); reset mid-commit
(old record survives); corrupt active/staged records; real intermediate
chain; wrong-SAN and wrong-EKU certs (both directions); expired +
not-yet-valid certs; client key/cert mismatch (status 7); supersession
attempt by an unauthenticated peer (refused, active session undisturbed);
slow-loris TLS peer (deadline closes it); reconnect bound (panel back
within backoff cap + one poll after server restart); seq cases per D13;
two power-loss cycles in one boot (two episodes); simultaneous local +
server alarms per D16. "Wrong server cert refused" now means: presented
by a bench daemon with a chlorine-issued cert lacking the panelgw SAN.

**Phase resequencing (D22 — replaces §11's ordering).** SW5 (protocol.h
+ shared codec changes + the D17 panel.php mask fix) is its own gate
P1a, cross-lab reviewed FIRST. Then P1b (listener) ∥ P2 (provisioning),
both consuming the frozen amendment. P3 unchanged (needs P1b + P2). P4
design/host-suite work may parallel P3, but its hardware exit needs P3's
cyw43 bring-up. P1b's exit is LOOPBACK/STAGING ONLY — nothing binds on
xenon until the P5 operator-approved deploy.

**PKI ops hardening (D23, SHOULD — extends §8 runbook).** The runbook
adds: cert inventory (serial + notAfter for panel + panelgw certs) kept
in the runbook itself; an expiry probe on the existing monitoring stack
(HTTPS probe against the listener port would conflate reachability —
instead a monthly `step certificate inspect` cron on xenon alerting at
<30 days); renewal window = the alert lead time; reprovision keeps the
old credential set staged-aside on the bench until the new set proves a
live connection (rollback = re-run panelProv with the retained set);
daemon cert replacement = install new chain + restart, old panel trust
unaffected (same root).

**Language/config reconciliation (D24, SHOULD — global).** The daemon
config key is `serialDev=` (existing normative name; §2/§5's `serial=`
is superseded). Journal lines sanitize peer cert subjects to
[A-Za-z0-9 .:=/-] before logging. Secrets rule restated precisely: key
material never in git, never in RETURN/handoff packets, never in logs;
it DOES traverse the PROVISION frames over USB by design. Bench copies
live under 0600 paths and are shredded (`shred -u`) after the live
connection is proven, per the D23 rollback window.
