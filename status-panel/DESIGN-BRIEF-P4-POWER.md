# DESIGN-BRIEF addendum — P4 power ride-through + alarm (SW4)

`v1.0 · 2026-08-17 · governs CONTRACT-SW.md §7 as amended by D15/D16/D18/D19`
`authored: Claude (P4 lane) · review: codex (cross-lab) · ratification: operator`

This addendum satisfies two contract preconditions: the styleguide-first rule
(§7b — glyph design before implementation) and D18's requirement that the
exact battery pack be recorded here BEFORE first connection. Sections marked
**[OPERATOR]** are blanks only Jason can fill; the code ships without them,
the hardware steps do not.

---

## 1. Battery pack record (D18 — complete BEFORE first connection)

Contract requirements (fixed): protected single-cell LiPo, JST-PH plug, on
the Galactic Unicorn battery input per Pimoroni's spec. The board has NO
charger and NO power-path mixing — never assume battery + USB combine.
Brownout at the pack's protection cutoff = clean death, acceptable.

| Field | Value |
|---|---|
| Chemistry / form | protected single-cell LiPo (3.7 V nominal) — fixed by D18 |
| Connector | JST-PH 2-pin — fixed by D18 |
| Capacity (mAh) | **[OPERATOR]** |
| Protection cutoff (V) | **[OPERATOR — from pack datasheet]** |
| Pack make/model | **[OPERATOR]** |
| Polarity vs board silkscreen | **[OPERATOR — CONFIRMED / date]** — JST-PH polarity is NOT standardized across vendors; verify pin-to-silkscreen with a meter before first plug-in |
| First-connection date | **[OPERATOR]** |

No firmware work depends on these values; they gate the D19 measurement
session, not the code.

## 2. Power-loss glyph (styleguide-first, §7b)

**Placement.** Bottom-right corner, columns 49–52 × rows 6–10 (4×5 px),
painted as a post-render overlay — the same layer order as the alarm inlay
and beacon. It never occupies the top-right beacon cell (alarm beacon keeps
its home) and never enters the label band (geometry law: labels ≤ row 6).
On screens whose ticker crosses rows 6–10, the overlay dims a 1 px halo
around itself before painting, exactly the `panelTextOver` convention, so
scrolling text passes behind it without visual collision.

**Form.** A vertical battery outline: 4 px wide × 5 px tall body drawn as a
1 px rectangle outline, with a 2 px wide × 1 px nub centered on top edge
(nub at columns 50–51, row 6; body rows 7–10). Interior stays dark — state
is carried by hue, not fill level (no battery telemetry exists, D18).

**Color/brightness.** `warn` amber, brightness 0.55 — above the ambient
reflection floor (dim inks are unreadable in daylight; hierarchy by hue).
**Steady, never blinking**: blinking is the alarm beacon's vocabulary; a
steady amber glyph + (possibly acked) episode reads as "condition present,
noise handled", matching the existing acked-alarm UX.

**Visibility rule (D16).** The glyph shows WHENEVER on battery, alarm state
notwithstanding. The alarm inlay, when the power episode is the top unacked
source and no server topAlert exists, shows subject `POWER LOSS` detail
`ON BATTERY` in the standard inlay style; a live server topAlert always wins
the inlay text (D16) while the glyph continues to say "on battery".

## 3. Brightness cap semantics (§7a)

`PANEL_BATT_BRIGHT_MAX` build default **0.20**, field-calibratable exactly
like `PANEL_LUX_FULL`. Applied as a final clamp after the auto/manual
brightness computation: `bright = min(bright, cap)` while on battery. LUX+/−
still work under the cap (they adjust the pre-clamp value); the cap lifts
atomically on VBUS restore. The cap does NOT touch the alarm tone.

## 4. Episode mechanics (D15/D16 — normative summary for the implementer)

- Episode id: `0xC0000000 | (edgeCounter & 0x3FFFFFFF)`; edgeCounter starts
  from a per-boot salt and increments on every debounced VBUS-loss edge.
  Two outages in one boot = two episodes; ack of the first cannot mute the
  second.
- Sources OR: tone active if ANY unacked episode (server or power). Ack
  (any button / CONTROL ackAlarm) applies to ALL currently-unacked episodes.
- A server SNAPSHOT never clears the power episode; VBUS restore clears
  ONLY the power episode and never suppresses a live server episode.
- Tone follows the existing night-profile rules unchanged (60 s repeat,
  5 min auto-silence); power loss does not override quiet-hours handling.

## 5. VBUS sensing (D18)

WL_GPIO2 read via cyw43 (init is unconditional — P3 already guarantees
this). Debounce 100 ms both edges: an edge commits on the first 40 ms tick
sample taken ≥ 100 ms after the raw level moved (in practice the fourth
sample, at 120 ms — the same time-based idiom as the CDC debounce), and any
flap back restarts the window. Boot-on-battery is a VBUS-loss state from
t=0: cap + glyph + one episode, salt-derived, before the first snapshot
ever arrives.

## 6. D19 measurement protocol (measure-then-ratify — [OPERATOR] session)

No runtime number appears in any document until measured. Session plan:

1. **Characterize** — record: pack + starting charge, ambient light, capped
   brightness in effect, WiFi associated to the production AP, workload =
   live snapshots + one full alarm cycle (arm → tone → ack). Measure current
   draw (USB power meter on a bench supply feeding the battery input via the
   JST lead, or inline DMM) and run the pack to protection cutoff once for
   gross runtime.
2. **Ratify** — Jason names the minimum acceptable ride-through from (1).
3. **Accept** — a repeat run meets the ratified minimum AND ≥3 VBUS
   loss/restore cycles behave per §4 above (fresh episode each loss, restore
   clears only the power episode, cap applies/lifts, glyph appears/clears).

| Measurement record | Value |
|---|---|
| Date / conditions | **[OPERATOR]** |
| Draw @ cap, WiFi associated (mA) | **[OPERATOR]** |
| Gross runtime to cutoff | **[OPERATOR]** |
| Ratified minimum ride-through | **[OPERATOR]** |
| Acceptance repeat run result | **[OPERATOR]** |

## 7. Implementation shape (for the P4 lane)

`panelPower.{c,h}`: a pure, host-testable module (no SDK includes) owning
debounce, edge counting, episode allocation from an injected salt, cap
state, and glyph-visibility state — driven by `panelPowerPoll(p, vbusRaw,
nowMs)`. Device glue reads WL_GPIO2 in main.c's tick (cyw43 is already up
via P3's panelNet). Alarm integration reuses the EXISTING episode state
machine in main.c — the power module only supplies episode ids and
clear events; it does not duplicate tone/ack logic (the same no-second-copy
rule the CONTROL path follows). Host tests: debounce bounds, two-episodes-
per-boot, ack-all composition, restore-clears-only-power, boot-on-battery,
salt wraparound at the 0x3FFFFFFF mask.
