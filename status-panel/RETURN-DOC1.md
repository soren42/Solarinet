# RETURN — SOLNET-DOC1

    TASK      SOLNET-DOC1 · Panel documentation (manual, quickref section, dashboard blurbs)
    FROM      opus / worker
    TO        fable-5 / lead
    DATE      2026-08-08
    STATUS    COMPLETE — three of task #13's four forms delivered; nothing deployed, nothing committed

---

## 1. Deliverables

| # | File | State |
|---|---|---|
| 1 | `docs/panel/SolariNet_Panel_Manual.html` | NEW · 23 pages · standalone |
| 2 | `docs/SolariNet_Operator_Quickref.html` | MODIFIED · one section added + one footer line |
| 3 | `dashboard/public/screens-panel.jsx` | MODIFIED · additive only · all harnesses green |

### 1.1 The manual

23 letter pages (816×1056) in the Rev2 house style. Cover, contents, then:

- **§01 The instrument** — the xenon → lithium → panel chain; what is computed server-side versus owned by the firmware; the two populations (`stateRoll` agent nodes versus the wider roster) and the two independent sources of staleness.
- **§02 The physical panel** — all nine buttons, the LUX manual latch, the 25% floor, boot to D1, sleep semantics, and the rule that every button is an acknowledge while an alarm is armed.
- **§03 Reading the matrix** — the six-colour vocabulary, azure-is-never-a-status, quiet-healthy, the three distinct meanings of brightness, and the 3×5 / 4×7 text geometry that explains the two clipping artefacts documented later.
- **§04–§15** — one page per screen, each with its normal-state SVG, numbered overlay callouts, and a "reading it" pair (what is normal / what to act on).
- **§16 Alarms** — over `b0-alarm.svg` with callouts, plus a second page showing the inlay over A1/B2/C1/D2 and the constant-versus-changed breakdown for acknowledge.
- **§17 The control page**, **§18 Rotation & weights**, **§19 The glance matrix** (with a hand-drawn 13×8 SVG diagram), **§20 Troubleshooting**.

**Callout geometry is derived, not estimated.** The renders are 742×154 for a 53×11 matrix at a 14 px pitch, so LED `(x,y)` centre is `(7+14x, 7+14y)` and its cell box is `(14x, 14y, 14, 14)`. Every region rect, marker and leader line is placed on those coordinates in an overlay `<svg>` sharing the image's `viewBox`, so the two scale together at any width. Markers sit above (`cy=-18`) or below (`cy=172`) the panel with leader lines, so nothing occludes the render.

I verified the geometry against the actual lit pixels rather than trusting the contract: A1's gates map out as APs at x 2/9/16, hubs at 20/27/34 staggered top-bottom-top, switch at 41, router at 47, internet at 51, backup WAN at 52 — an exact match to `CONTRACT-AW.md` §9 A-1.

### 1.2 The Quickref section

"The status panel" section inserted between *Endpoints & hosts* and *If something looks off*, using that document's own idiom (`h2`, `.grid.two` of `.card`, `table`, `ol.flow`, `.note` / `.note.warnb`) and its own palette — deliberately **not** the Rev2 tokens, since the Quickref carries its own self-contained CSS. Covers the chain, the colour vocabulary, the four themes, the alarm sequence, panel-side faults, and the two doctrine points an operator most needs (ack silences but never clears; the panel is the authority on its own state). The footer's deeper-reading list now points at the manual.

### 1.3 The dashboard blurbs

- Added a module-level `SCREEN_BLURBS` table — 12 entries, 2–3 sentences each, drawn from the same source-of-truth reading as the manual, including the honest notes (A2's lanes are not MQ/SNMP; B1's bar is against an adaptive peak; C1's history is lost on reboot; D0's rows are systems, not named probe targets).
- One `<p className="vp-footnote">` renders the blurb for the screen the wall is showing now, keyed off the existing `currentIdx`, placed immediately after the chips block and before `VirtualPanel`.
- Each tile's title span gained a `title={…}` hover of the same text.
- Prose notes added to the four control blocks that had none — Theme, Screen, Alarm, Power. Brightness and Dwell already had notes and were left alone.

**No CSS was written or changed.** `styles.css` is out of scope, so the blurb reuses the already-styled `vp-footnote` class and the control notes reuse `vp-ctl__note`. No layout change, no new class names, no restructuring.

---

## 2. Verification — what I actually ran

| Check | Result |
|---|---|
| `node tests/dashboard/test_jsx_parse.js` | **16/16 parsed** (baseline 16/16) |
| `node tests/dashboard/test_panel_aw.js` | **150 passed, 0 failed** (baseline 150) |
| `node tests/dashboard/test_lifecycle_ui.js` | **80 passed, 0 failed** (baseline 80) |
| Manual — HTML well-formedness | Python `HTMLParser` tag-stack walk: zero unclosed, zero mismatched |
| Manual — image refs | All 17 `src="img/*.svg"` resolve as files relative to `docs/panel/` |
| Manual — external resources | Zero `http(s)` `src`/`href` in the document |
| Manual — structure | 23 `.page` sections, 22 `.foot` blocks (cover has none, by design) |
| Quickref — well-formedness | Same parser walk: clean |
| `screens-panel.jsx` export anchor | `Object.assign(window, { PanelScreen: PanelScreen });` byte-identical, confirmed by tail inspection |

Baselines were captured before I touched any file, so the three green numbers are a genuine no-regression result rather than an assertion.

---

## 3. UNVERIFIED

Non-empty and honest. None of the following was exercised.

1. **No human has read the manual through.** It has not been proofread by anyone but its author, and self-review does not catch the class of error that matters most here — a confidently-worded wrong reading.
2. **The manual has never been opened in a browser.** Well-formedness was checked by a parser, not a renderer. Nothing confirms that pages break where intended, that the two-column callout lists do not overflow their page box, or that the print stylesheet produces 23 clean letter sheets. **Page overflow is the most likely defect** — the screen pages carry a figure plus eight callouts plus two cards, and I sized that by judgement, not measurement.
3. **Callout marker positions are computed, not visually confirmed.** The coordinate maths is verified against the SVGs' actual pixels, but no one has looked at a rendered page to confirm a leader line points where a reader would expect, or that markers at `cy=-18` clear the figure's padding at every viewport width.
4. **The light theme is untested.** The token set is copied from the Rev2 guide, but no rendering has confirmed contrast or legibility in light mode, particularly the callout markers over the dark LED renders — which do not change with the page theme.
5. **The dashboard blurb was not rendered.** The JSX parses and every harness passes, but nothing was loaded in a browser. Specifically unconfirmed: that the blurb reads well in its position above `VirtualPanel`, that its `vp-footnote` styling suits a per-screen note as well as it suits the two existing static footnotes, and that the four new `vp-ctl__note` blocks do not make the controls panel noticeably taller than the design accepted.
6. **`title=` hover text is untested for length.** A 3-sentence native browser tooltip may render awkwardly or be truncated on some platforms. It is additive and harmless if so, but it is not verified.
7. **The glance-matrix diagram on §19 is illustrative.** It shows the layout and an example lit state I composed to demonstrate the regions; it is not a capture of a real frame and does not depict any real fleet reading.
8. **Screen semantics are read from source, not observed on hardware.** Every description comes from the firmware renderers and the contracts. I did not watch the physical panel to confirm that, for example, D2's field visibly warms at 35% load in practice, or that C1's traces refill over roughly two minutes after a restart. The timings are computed from the constants.
9. **No accessibility audit.** `alt` text is present on every image and is descriptive, but nothing has been checked with a screen reader, and the numbered-callout pattern (marker in an image, explanation in a separate list) is not verified as navigable non-visually.
10. **Version and date on the cover are my own** — "1.0.0", 2026-08-08. If the panel subsystem carries a version elsewhere, mine will not match it.
11. ~~Stale B1/C2 images.~~ **Closed** — see the final pass below. Both were regenerated by the Lead from the fixed renderers and re-verified against the manual's callouts.
12. **The other ten screens' callouts were not re-verified against the regenerated images.** The Lead regenerated all 24 renders, not just the two that changed. I re-derived B1 and C2 from the new files pixel by pixel, and spot-confirmed that the regenerated `a0-normal.svg` carries the same timestamp as the rest, but I did not re-map the remaining ten. Their underlying renderers were not touched in this round, so the images should be identical in geometry — that is an inference, not a check.

---

## 4. Findings for the Lead

Firmware and asset fixes are outside my scope; I raised each rather than working around it.

1. **B1 rendered two labels over each other.** `panelScreensB.c` drew the `%d%% LINK` reading at `y=4` (rows 4–8) and the ticker at `y=6` (rows 6–10); `panelScroll` does not clip vertically and the ticker paints last. **Accepted as a real defect. Fixed in both renderers, in two rounds** — see item 4 for the round that failed and why. Final state: the standalone reading is dropped entirely, the bar keeps rows 1–2, the ticker moves to `y=5`. Manual written to that geometry.

2. **C2's label was clipped.** `panelText(2, 9, "BY POOL", …)` needed rows 9–13 on an 11-row matrix. **Accepted and fixed** — the label is removed entirely; pool rows occupy rows 2–8 and identify the screen by shape. Manual page rewritten, callout removed, `alt` text corrected.

3. **`docs/brand/index.html` does not exist.** The assignment named it as the first thing to read. The directory contains `README.md`, `SolariNet Interface Guide Rev2.html` and `archive/`; the README names the Rev2 guide as canonical, so I conformed to that. **Lead confirms this was a packet error and the Rev2 fallback was correct.** Closed.

5. **The §01 roster claim is confirmed.** I flagged that the manual's most load-bearing inference — the panel roster can exceed the 64 systems the wire carries, and pool aggregates are computed server-side over the full fleet — deserved a second pair of eyes, since B0's tier flag and B2's hot-count denominator both rest on it. **Lead verified against `panelEncodeSnapshot` (systems capped at 64) and `panel.php` (`poolAgg` over `systemsAll`), with the reasoning recorded in `panelScreensB.c:40-43`.** The claim stands as written.

4. **RESOLVED — the first B1 fix relocated the collision onto the bar's warning end.** Raised with the Lead, who accepted the analysis and re-fixed it. Nothing in the firmware was touched by me.

   The percent is drawn with `panelTextOver(52 - panelTextW(line), 0, …)`, and `panelTextOver` dims a 1 px halo to 0.12 before painting. The utilisation bar is still at rows 1–2 spanning `x0 … x0+23` where `x0 = max(26, panelBigW(gLabel)+4)`. Text at `y=0` covers rows 0–4 and its halo rows −1…5, so bar rows 1 and 2 are inside the knockout in **every** width combination:

   | gLabel | bar | percent | halo from | bar cells hit | frac range |
   |---|---|---|---|---|---|
   | `1.2G` | x26–49 | `0%` | x44 | 6 of 24 | 0.75–0.96 |
   | `1.2G` | x26–49 | `45%` | x40 | 10 of 24 | 0.58–0.96 |
   | `1.2G` | x26–49 | `100%` | x36 | 14 of 24 | 0.42–0.96 |
   | `12.4G` | x28–51 | `100%` | x36 | 16 of 24 | 0.33–0.96 |

   The bar's amber band starts at `frac > 0.7` and its red band at `frac > 0.85` — cells k=17…23, at x43–49 — which sit inside the halo in all cases. So the cells the bar exists to show are exactly the ones the percent knocks out, and it degrades as utilisation rises: at 100% the reading is widest and the entire red zone is dimmed. That is a worse trade than the original defect, where the casualty was a text reading the ticker already duplicated.

   I offered two options: move the percent left into the gap before the bar, or drop it. **The Lead took the second, and was right to** — my first option had a collision of its own that I had missed: `AGGREGATE` is nine characters, so it runs to x37, past the bar's start at x26–30. There is no gap. Today's screen only works because the bar paints *after* the title and carves through it.

   Final ruling, implemented in both renderers: the standalone percent is gone. B1 is the watermark numeral, the `AGGREGATE` title, the 24-cell positional bar on rows 1–2, and the ticker at `y=5` carrying the absolutes. The root cause of both rounds was the same — the screen never had room for a third five-row text element.

   **Lesson worth keeping:** the two defects I found by mapping pixels were both real, and the first attempt to fix one of them introduced a worse regression that also would not have been caught by any test in the harness. Nothing in `tests/dashboard/` renders a screen and inspects the resulting framebuffer. A parity fixture that asserts on lit-pixel maps would have caught all three.

A fourth, smaller note: the manual's §01 states that the panel's roster can exceed the 64 systems the wire carries and that pool aggregates are computed over the full fleet. That reading comes from the contracts. If it is wrong, it is the single most load-bearing claim in the document, because B0's tier flag and B2's hot-count denominator are both explained by it — worth a second pair of eyes.

---

## 4a. Final pass — against the regenerated images

Both changed screens were re-derived from the fresh SVGs by mapping their lit pixels, not from the change summary. That check has now caught something three times, so it is the method rather than a formality.

**B1** (`b1-normal.svg`, regenerated). Verified from the pixels: `AGGREGATE` occupies rows 0–4 from x1; the watermark numeral sits behind it on rows 2–8; the bar runs x26–49 on rows 1–2, lit through x45 with the last three cells amber — exactly the positional threshold (`frac > 0.7` begins at k=17, x43); the ticker occupies rows 5–9; **row 10 is empty and there is no text anywhere on rows 0–4 right of x35.** The percent is genuinely gone. Manual updated: callout 4 removed, ticker renumbered 4, band moved to rows 5–9, `alt` text corrected, and the "three readings" note replaced with "two readings, at two distances" explaining why there is deliberately no third element.

**C2** (`c2-normal.svg`, regenerated). Verified: four pool rows at 2–5 (key pixel at x0, bar from x2, tier-0 marker at x49 on row 5), and **rows 9 and 10 are entirely unlit** — the label is gone. Manual updated: callout removed, remaining five renumbered, the expected-not-a-fault note replaced with a plain statement that the screen carries no title, `alt` text corrected.

**Knock-on caught during this pass:** §03 "Reading the matrix" carried a sidebar citing both defects as things that "occur in the shipped renderers and are noted where they do" — false once they were fixed. Rewritten to state the geometry rule positively: a label can start no lower than row 6, and no more than two five-row text elements fit in eleven rows, which is why screens that tried for three were cut back.

Re-verified after every edit above: manual parser-clean (zero unclosed, zero mismatched), 17/17 image refs resolve, zero external refs; `test_jsx_parse.js` 16/16, `test_panel_aw.js` 150/0, `test_lifecycle_ui.js` 80/0 — the last three with the Lead's parity edits present in `screens-panel.jsx` alongside mine.

## 5. Scope

Touched exactly the three files in `IN-SCOPE`, plus the new manual and this packet. Not touched: firmware, API routes, `/var/www`, `styles.css`, any other `docs/` file, and git — no `add`, no `commit`, no `push`, no deploy. The manual is ready for you to deploy; it needs `docs/panel/img/` alongside it, since every image reference is relative.
