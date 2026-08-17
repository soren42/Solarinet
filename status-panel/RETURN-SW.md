# RETURN-SW — Standalone WiFi program (task #8), phases P1a–P5

`2026-08-17 · branch feat/standalone-wifi · Lead: Fable 5 · to: operator`

## Verdict

The panel firmware, daemon, tooling and documentation for standalone WiFi
mode are **code-complete, cross-lab reviewed at every gate, and ready for
your review**. Nothing has been merged, pushed, deployed, flashed, or
issued a certificate — every live action in this program remains yours.

## What was built (12 commits, 53b9a82..a3ce9b5)

| Phase | Commits | Author → Reviewer | Outcome |
|---|---|---|---|
| P1a SW5 protocol + D17 mask | 53b9a82, 02c6fcb, 0d17441 | codex-authored parts + Lead → cross-lab | APPROVE after 3 MUST/3 SHOULD fixed |
| P1b daemon TLS listener | a9aa3b4 | codex → Claude | APPROVE; ERR_clear_error root cause found in review |
| P2 provisioning + cred store + panelProv | e4be4ab, c99ff00, 01a421e | Claude → codex | APPROVE after 2 rounds (10 MUST + partials) |
| P3 WiFi transport (FSM + device glue) | 787d33f, f1ec244 | codex → Claude | APPROVE; reviewer caught SNTP_COMP_ROUNDTRIP epoch bug |
| P4 power ride-through + alarm | 623c823, d3ab452 | Claude → codex | REVISE → APPROVE (same-tick ack, glyph-under-help, composition seam) |
| P5 docs + final gate | a3ce9b5 | Lead → codex (whole-branch) | see final-review section |

## Deliverables

- **Firmware**: WiFi transport (cyw43/lwIP/mbedTLS, mTLS both ways,
  SNTP-before-TLS, D14 arbitration, D13 per-transport seq), USB-only
  provisioning with A/B CRC'd flash credential store, VBUS power
  episodes (D15/D16/D18) with brightness cap + battery glyph + alarm
  composition.
- **Daemon**: raw-TLS listener mode beside serial (OpenSSL, TLS≥1.2
  AEAD-only, SAN+EKU+denylist verification, single-session
  newest-wins-post-auth, deadline enforcement).
- **Tooling**: `status-panel/tools/panelProv` lockstep provisioning
  tool with resume + `--wipe`.
- **Docs** (print-verified, all pages fit US Letter):
  - `docs/panel/SolariNet_Panel_Standalone_Runbook.html` — staged,
    reversible cutover (4 stages, rollback at each) + revocation,
    renewal, cert inventory, expiry probe. `[OPERATOR]` blanks mark
    every value only you can fill.
  - Manual 1.1.0 — new §21 Standalone WiFi + §22 Power & battery;
    pre-existing P04 page overflow (+60px, on main too) found by a
    headless render audit and fixed.
- **Server**: panel.php D17 episode-mask fix (`&0x3fffffff`) + CI
  golden test — this is live already via the repo-served API.

## Test evidence (final state, all run this session)

- Firmware host suite: all binaries green under ASAN/UBSAN, including
  panelPowerTest 49 checks, panelParityTest 107, netFsm 36, link 22.
- Daemon: codec tests + listener harness 10/10 (6 refusal cases).
- Tools smoke: pty round-trip incl. wipe.
- Device build on lithium (arm-none-eabi 14.2, SDK 2.1.1): clean; only
  the two known-benign warning classes (vendored lwIP mbedtls glue,
  newlib _link/_unlink).
- Two-build reproducibility pair at the final firmware state (a3ce9b5,
  firmware unchanged since d3ab452): two clean builds on lithium,
  BYTE-IDENTICAL, sha256 `6aae0b066ac464475a745585986fca885cdc7bb0
  cca23aa9fcdab8c832ddd96e` — the flash-gate evidence for this image.
- Final whole-branch cross-lab review: **[PENDING — see
  p5-final-review.md at repo root]**

## Decisions I made under the standing "best judgement" grant

1. **P4 tick reorder** — `runAlarm()` now runs before `scanButtons()` so
   a press on the very tick an episode arms is consumed as the ack.
   This also fixed a pre-existing one-tick window for server episodes.
2. **Brightness raw setter** — the 0.20 battery cap conflicts with the
   0.25 UX floor; a dedicated `panelHwSetBrightnessRaw` (floor 0.05) is
   reachable only from the cap path, and every user-intent path stays
   floored at 0.25. STATE reports the capped (true) hardware value.
3. **Glyph under help** — D16's "whenever on battery" wins over help;
   the power *inlay* stays help-suppressed, symmetric with the server
   inlay.
4. **Runbook host placement** — xenon config sets `serialDev=` (empty)
   explicitly; omitting the key would leave the default glob retrying a
   nonexistent device.
5. **Expiry probe as a file-reading cron** — a TLS probe against :7443
   would conflate reachability with validity (per D23); the runbook
   ships a `openssl -checkend` script + monthly cron instead.
6. **Manual version bump to 1.1.0** with the standalone/power sections;
   P04 overflow fixed while in the file (it was on your read-through
   risk list).

## Your gates (in dependency order)

1. **Battery pack record** — DESIGN-BRIEF-P4-POWER.md §1, BEFORE the
   pack is ever connected (polarity vs silkscreen; board has no charger).
2. **D19 measurement session** — draw + runtime at capped brightness
   with WiFi associated; you ratify a minimum; a repeat run + ≥3 VBUS
   cycles must meet it.
3. **Merge/push decision** on feat/standalone-wifi.
4. **Flash** (two-build gate evidence in this packet) and **cert
   issuance / DNS / daemon deploy / cutover** — runbook Stages 1–4.

## UNVERIFIED (nothing below was exercised; none of it is claimable)

- Any behavior on real hardware beyond compiling: WiFi association,
  mTLS against the real chlorine chain, SNTP, arbitration timing,
  VBUS edges, brightness cap/glyph appearance, tone, D19 numbers.
- The runbook's operational procedures end-to-end (issuance commands
  against chlorine's actual provisioner, DNS SoR step, systemd unit on
  xenon). Config keys/log texts were verified against source; the
  procedures themselves have not been executed anywhere.
- §10.3–10.7 acceptance and D20 soak (1 h reconnect, heap high-water):
  hardware-gated, not attempted.
- Print output verified by headless-Chrome box-fit audit, not by a
  physical printer.
- Dashboard panel-sim (JS renderer) does NOT draw the battery glyph —
  deviation, out of firmware scope; flagged for a follow-up if you want
  wall/page parity for the glyph.

## Secrets

No key material anywhere in the branch, docs, or this packet; secrets
are referenced by path only (run/panel.pass, /etc/solari-panel/*,
bench 0600 paths).
