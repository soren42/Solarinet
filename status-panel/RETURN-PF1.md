# RETURN — PF1 · framebuffer-assertion parity fixture

`worker: claude (opus) · role: Worker · task: PF1 · governed by CONTRACT.md §4/§9, CONTRACT-AW.md §9 A-1, §10 A-3`
`status: COMPLETE · committed in the worktree, NOT pushed · no deploys, no flashing, no service or DB state touched`

## The headline

**A real production rendering defect was found and fixed.** `panelScreenA1` painted the
connective legs *after* the gates, and `a1Leg()` assigns with `panelFbSet`, so a diagonal
wire crossing a gate's column replaced that gate's pixel with `cQuiet`. On the real
9-device inventory it ate the top pixel of a **degraded** hub (x=27, row 6) and a pixel of
a healthy one (x=34, row 4) — a state pixel reading as connective texture, which
CONTRACT-AW §4 makes `cQuiet` explicitly *not*. The page renderer has always had the
layering right (`sA1`: "Legs first, dimmest"); nothing had ever compared them, which is the
whole of RETURN-AW3 UNVERIFIED #3. Fix, findings and evidence in
[Defect found](#defect-found-a1-legs-were-painted-over-the-gates) below. Nothing buggy was
baked into a golden: the goldens were regenerated after the fix.

## Files authored

| Path | Change |
|---|---|
| `status-panel/fixtures/parity-fixture.json` | **New.** The canonical dataset both renderers read. 12 chronological samples in the GET `/api/panel` `data` shape, plus a `routerDown` gear variant. |
| `status-panel/firmware/test/panelParityTest.c` | **New.** Host harness: renders all 12 real screens + inlay + help + link-lost + no-data, asserts goldens and three structural invariants. **90 assertions.** |
| `status-panel/firmware/test/testJson.{c,h}` | **New.** Small read-only JSON DOM so the C side reads the *same bytes* as node instead of a transcription. Host-only; never linked into firmware. |
| `status-panel/fixtures/golden/panel-*.txt` | **New.** 17 committed golden framebuffers (312 KB). |
| `status-panel/firmware/test/Makefile` | `panelParityTest` added to `BINS`, with its build rule and its run line under `all`. |
| `tests/dashboard/test_panel_parity.js` | **New.** Renders A1 through the real page renderer from the same fixture and diffs against the C golden. **55 assertions.** |
| `status-panel/firmware/panelScreensA.c` | **Renderer fix** — legs before gates. See below. |

`.gitignore` needed no change: `status-panel/.gitignore` line 2 already carries
`firmware/test/panel*Test`, which covers `panelParityTest` and does not catch the `.c`.

## How to run

```sh
make -C status-panel/firmware/test          # builds and runs the whole C suite
node tests/dashboard/test_panel_parity.js   # the JS half of the parity diff
node tests/dashboard/test_panel_aw.js       # unchanged, still 150/0
```

Regenerating goldens, when a renderer change is intended:

```sh
cd status-panel/firmware/test && PARITY_REGEN=1 ASAN_OPTIONS=detect_leaks=0 ./panelParityTest
```

`PARITY_REGEN=1` rewrites every golden. **Read the diff before committing it** — the whole
value of the mechanism is that a surprising diff is a bug report.

| Suite | Result |
|---|---|
| `make -C status-panel/firmware/test` | green — all six binaries, **90 passed / 0 failed** in the new one |
| `node tests/dashboard/test_panel_parity.js` | **55 passed / 0 failed** |
| `node tests/dashboard/test_panel_aw.js` | **150 passed / 0 failed** (unchanged) |

## Design decisions

### No reimplementation, enforced structurally

The hard requirement was that the test drive the real renderers. It links
`panelScreensA-D.c`, `panelInlay.c`, `panelHelpOverlay.c`, `panelFb.c`, `panelFont.c`,
`panelHist.c` and `protocol.c`, and observes them through **GNU ld `--wrap`** on the nine
framebuffer and text primitives. The test file contains no band, row or column constant.
Where the JS side needs a gate's extent it asks the page's own `a1Layout()`; where the C
side needs one it watches the renderer paint it.

The text wrappers set a **paint tag** for the duration of the call, so every framebuffer
write arrives labelled: structural / small text / watermark-knockout text / BIG / ticker.
That is what makes text-vs-structure assertions possible without knowing any geometry.
`--wrap` only redirects *undefined* references, so `panelTextOver`'s internal call to
`panelText` inside `panelFont.c` is not intercepted — which is what we want: the outer
tag stays in force through the glyph pass.

### Observing attempted coordinates, not surviving pixels

`panelFbSet` drops out-of-range writes silently. A golden image therefore can *never* show
you a label that ran off the bottom of the panel — the pixels simply are not there. That is
exactly how the C2 defect survived. The wrappers see the coordinates the renderer *tried*,
before the real call, so the geometry law ("eleven rows fit at most two five-row text
elements; labels start at row ≤6") becomes directly testable.

### The three invariants, and proof that they fire

| | Asserts | Defect it would have caught |
|---|---|---|
| **T1** | no small-font glyph shares a row with the scrolling ticker | B1's y=4 reading clipped by the y=5 ticker |
| **T2** | no glyph is painted outside rows 0..10, measured on attempted coordinates | C2's 5-row label at y=9 |
| **T3** | a warn/crit cell painted structurally still reads as its palette constant at its brightness after every text and halo pass | B1's `panelTextOver` halo eating the bar |

These were **verified by reintroducing all three defects** into `panelScreensB.c` and
`panelScreensC.c` and confirming each fires, then reverting:

```
FAIL: B1 (throughput): T1 ... small-font glyph shares row 5 with the scrolling ticker
FAIL: C2: T2 ... glyph pixel attempted at row 11 (panel has rows 0..10)
FAIL: B1: T3 ... text halo dimmed a warn cell at (43,1) by x0.12
```

T3 deliberately distinguishes a **text halo** from a renderer's own deliberate knockout:
D2's crit-spark box and the LINK LOST plate legitimately dim conditional pixels, so an
untagged `panelFbDim` merely disarms the guard while a dim issued *inside* a text primitive
is a violation. `panelFbDimAll` (the inlay) disarms everything. T3's second leg compares
the flushed pixel against `cWarn`/`cCrit` scaled by the intended brightness — pointer
identity against the palette extern, not "non-zero".

**Coverage is asserted, not assumed.** An invariant nothing triggers proves nothing, so the
case table carries `wantArmed` / `wantScroll` claims: A0, B1, C2 and the inlay must actually
paint conditional cells, and B1, D1 and the inlay must actually paint a ticker. If a future
fixture edit makes T1 or T3 vacuous on those screens, the run fails.

### The fixture

Authored in the dashboard's GET `/api/panel` `data` payload shape so `buildEnv()` consumes
it unmodified, and decoded on the C side into a `PanelSnapshot` replayed through
`panelEnvApply()` — one file, two readers, no transcription. It is shaped to drive the
screens into their interesting bands rather than their default ones: the newest sample is
700 000 / 300 000 kbps, exactly the adaptive peak, so B1's utilisation bar reaches 100 % and
lights both its warn and crit cells; `APPS` sits at 90 % load so C2's bar crosses both
thresholds; `CORE` is tier-0 with a host down; rtt 42 ms and loss 1.8 % clear B2's
thresholds; and the alarm is active with a top alert so the inlay and the D1/B1 tickers
carry real text. Rate label renders as `1.0G`.

**Every gear row carries `txLevel: 0` deliberately.** §3.1 makes level 0 idle and *both*
renderers draw no particle at all for it. That removes the one part of A1 the two
implementations do not share, so everything left on screen is a pure function of the
fixture.

### Render order is load-bearing and fixed

A1 (`gA1`) and D0 (`gWf`) keep file-static integrator state that persists across calls, so a
case's output depends on how many frames of that screen ran before it. Cases run in a fixed
order and each screen is ticked 251 frames at the firmware's real 25 Hz — never teleported
to a sample time it would not pass through — with frames captured at t = 0, 5 and 10 s.
**New cases go at the end of the table.**

### The JS parity diff: what is compared, what is excluded

Compared: for every gate `a1Layout()` places, the exact set of lit rows within that gate's
own span on its own column, plus each lit pixel's **hue class**; and the internet (x=51) and
wanBackup (x=52) columns in full. That is §9 A-1's normative geometry and §10 A-3's
inheritance rule asserted on pixels rather than on constants.

Excluded, and why:

1. **Brightness.** The firmware uses fixed per-role brightnesses (0.75 gates / 0.80 router /
   0.60 internet and wan); the page modulates by level and pulses degraded gates. Both are
   within contract — only geometry and hue are normative.
2. **The leg layer.** The two leg brightness curves differ: firmware `0.04 + rx*0.06` (up to
   0.40), page `0.035 + (rx/7)*0.05` (up to 0.085), roughly 5× dimmer. At any fixed
   threshold the firmware's wires read as lit and the page's do not.
3. **Particles.** The integrators differ by design (90 keyed to a source gate vs 72 keyed to
   a leg) and reconciling them is out of scope. With `txLevel: 0` neither draws any, so the
   exclusion costs nothing on this fixture.

Exclusion 2 is *bounded* rather than waved at. Test block [4] asserts that every pixel lit
in the firmware but dark on the page is `cQuiet`-hued — i.e. a leg — and that there is **no**
pixel lit on the page but dark in the firmware. If the two A1s ever drift in anything that
is not the documented leg-brightness gap, that assertion fails. It currently reports 73
firmware-only lit pixels, all leg layer, in both gear variants.

## Defect found: A1 legs were painted over the gates

The first run of the JS diff failed on exactly two pixels per variant:

```
FAIL - primary gear: gate 4 (x=27) lit pixels agree on hue
       [page=6:warn,7:warn,... firmware=6:quiet,7:warn,...]
FAIL - primary gear: gate 5 (x=34) lit pixels agree on hue
       [page=...,4:azure,5:azure firmware=...,4:quiet,5:azure]
```

**Mechanism.** `panelScreenA1` drew gates, then legs. `a1Leg()` writes with `panelFbSet`
(assign, not accumulate), so wherever a diagonal leg's rounded y landed on a gate's column
inside that gate's row span, the gate pixel was replaced by `cQuiet` at leg brightness. Two
legs do this on the real inventory: the AP2→hub2 leg crosses x=27 at row 6, which is the top
row of hub1 — **a degraded hub, so a warn state pixel was rendering as texture** — and the
hub0→switch leg crosses x=34 at row 4, inside hub2.

**Minimal fix applied** (`status-panel/firmware/panelScreensA.c`): the band loops now only
*compute* geometry, the leg loops run next, and a second pass draws the gates. Same
geometry, same brightnesses, same within-band order; only the layering moved. Because the
legs now land on a just-cleared framebuffer, `panelFbSet` and an accumulate are equivalent
there, so nothing else about the leg layer changes. This is also what the page has always
done, and `screens-panel.jsx`'s own `a1Gate` comment asks for exactly this ("Both sides must
paint identical pixels from identical gear bytes; keep this function and that one in step").

After the fix the gate layer matches pixel for pixel and hue for hue across both gear
variants. Goldens were regenerated **after** the fix, so no buggy output is baked in.

This closes RETURN-AW3 UNVERIFIED #3 for the gate, internet and wanBackup layers.

## UNVERIFIED

1. **Nothing was run on hardware.** Every golden is a host render. The panel was not flashed
   and the RP2350 was never in the loop; the fleet is in a maintenance window and the panel
   is asleep. That the fixed A1 *looks* right on the physical matrix is unconfirmed, and the
   defect fix is a visible change to a shipping screen.
2. **Golden portability across toolchains is untested.** The goldens were produced by one
   gcc on one x86-64 host at `-O1`. Several renderers call `sinf`/`cosf`/`fabsf` (the inlay
   pulse, D2's field, LINK LOST), and libm results are not bit-identical across libm
   versions or architectures; `-ffp-contract` differences could also move a rounded channel
   by one. A different machine may see spurious golden diffs. No tolerance was added on
   purpose — a tolerance would blunt the mechanism — so the mitigation is to regenerate and
   *read* the diff. Whether this actually bites has not been measured on a second host.
3. **The leg-brightness divergence is documented, not adjudicated.** The firmware's legs are
   ~5× brighter than the page's. I did not change either, because CONTRACT-AW does not
   specify leg brightness and the task scoped me to geometry parity. Someone should decide
   which is correct; the panel and its web twin currently do not look alike.
4. **Particle parity remains unproven**, exactly as RETURN-AW3 left it. The fixture sidesteps
   it with `txLevel: 0` rather than resolving it. 90-vs-72 particle counts and different
   speed and brightness curves are still there and still untested against each other.
5. **T1's BIG exemption is a judgement call.** The ticker crossing the BIG watermark is the
   design's own watermark technique, so `panelBig` is exempt from the ticker-band rule. If a
   future screen puts a *readable* BIG figure under a ticker, T1 will not object.
6. **Coverage is per-screen, not per-element.** T3 is asserted everywhere but only *proven
   non-vacuous* on A0, B1, C2 and the inlay; T1 only on B1, D1 and the inlay. Screens whose
   conditional cells this fixture never lights (A2, C0, C1, D0, D2) are covered by their
   goldens alone.
7. **Only three frames per screen are pinned** (t = 0, 5, 10 s). Animation states between
   those samples are unpinned, and a defect that only appears at, say, t = 7.3 s would pass.
8. **The help overlay is captured at a single t (3.0 s)** for all twelve indices, so a
   scrolling help line is pinned at one scroll offset only.
9. **`testJson.c` is minimally exercised.** It parses one file. Surrogate pairs, deep
   nesting, and pathological numbers are unhandled or untested; it is a harness convenience,
   not a general parser, and should not be reused elsewhere.
10. **No push, no deploy.** The commit sits on the worktree branch only. The firmware fix has
    not been built for the RP2350 target — only under the host gcc — so the Pico SDK build
    is unverified against this change.

---

# Fix round — Codex cross-lab review of 792aedc (BLOCK)

Three items addressed. No golden framebuffer changed as a result: a full `PARITY_REGEN=1`
pass after the round produced byte-identical files, which is also a second, unplanned piece
of evidence for the determinism UNVERIFIED #2 worries about.

| Suite | After the fix round |
|---|---|
| `make -C status-panel/firmware/test` | green — new suite **107 passed / 0 failed** (was 90) |
| `node tests/dashboard/test_panel_parity.js` | **59 passed / 0 failed** (was 55) |
| `node tests/dashboard/test_panel_aw.js` | **150 passed / 0 failed** (unchanged) |

## 1 (MUST) — golden regeneration is now CI-unsafe by design

`panelParityTest` refused nothing before: `PARITY_REGEN=1` rewrote every committed golden
wherever it ran. In CI that is a hole rather than a convenience — a real rendering
regression would rewrite its own expectation and report success, and the diff nobody reads
would land in the branch.

`regenRefusedInCI()` now runs as the first statement of `main()`, before the fixture is even
loaded. If `PARITY_REGEN` **and** `CI` are both truthy the binary prints a plain explanation
to stderr and **exits 2** without touching `fixtures/golden/`. Truthiness is
`envTrue()`: set, non-empty, and not `0` / `false` / `FALSE` / `no` / `off`, so GitHub
Actions' `CI=true`, a runner's `CI=1`, and a developer's deliberate local `CI=false` all
behave as intended.

Test-visible note, and how it was checked:

```
$ CI=true PARITY_REGEN=1 ./panelParityTest ; echo $?
panelParityTest: REFUSING to regenerate goldens.
  PARITY_REGEN and CI are both set. ...
2
```

`md5sum` of `panel-A1.txt` was taken before and after — unchanged. `CI=false PARITY_REGEN=1`
still regenerates, also confirmed. This is a *refusal path*, not an assertion, so it does not
appear in the pass count; the exit-2 invocation above is the check, and it is written into
the `main()` comment so the next reader can rerun it. CI wiring itself is the coordinator's
in `ci.yml`.

## 2 (SHOULD) — `testJson` rejects malformed input, with a corpus

Three parser changes:

- **Unescaped control bytes are now a parse error.** RFC 8259 §7 requires U+0000–U+001F to be
  escaped inside a string; accepting them raw let a stray newline or NUL reshape a fixture
  value while still "parsing fine".
- **Lone surrogates are rejected** (this was the optional item; it was cheap). A high
  surrogate must be followed by its low half, which is now decoded into a real code point,
  and a bare surrogate of either kind fails. `pushUtf8()` gained the 4-byte branch it needed
  to encode the result — previously any astral code point would have been mis-encoded.
- **`jsonParseMem()`** was factored out of `jsonParseFile()` (which now delegates) so the
  corpus can be asserted in-memory without scattering temp files. Trailing-garbage rejection
  already existed and is now covered by a test rather than assumed.

`runJsonCorpus()` in `panelParityTest.c` runs before anything renders: **10 reject cases**
(raw control byte, embedded NUL, truncated object, truncated array, unterminated string,
trailing garbage, lone high surrogate, lone low surrogate, bad escape, missing colon) and
**3 accept cases** (escaped control byte, valid surrogate pair, trailing whitespace). Every
case runs under the existing ASAN/UBSAN build and frees its result, so a leak or an
overread on the error paths fails the suite.

## 3 (SHOULD) — the leg exemption is now by coordinate, not by hue

The reviewer was right that "any firmware-only `cQuiet` pixel is a leg" could swallow a new
non-leg quiet divergence anywhere on the panel. **It turned out to be tractable without
reimplementing `a1Leg` geometry**, via palette identity:

Inside an A1 render `cQuiet` has exactly one source. `a1Leg()` is the only thing that uses
it; gates and particles take their colour from `a1GateColor()`, which returns
`cCrit`/`cWarn`/`cAzure` and never `cQuiet`. CONTRACT-AW §4 is what keeps that true —
`cQuiet` is connective texture and never a state carrier. So the existing `panelFbSet`
wrapper now records, for the two A1 cases only, every coordinate painted in `cQuiet`, and
dumps it to a sidecar beside the golden:

```
status-panel/fixtures/golden/panel-A1.legs.txt            96 coordinates
status-panel/fixtures/golden/panel-A1-routerdown.legs.txt 96 coordinates
```

These are a **dump of what the real renderer painted**, not a recomputation — no leg
geometry is derived on either side. They are treated exactly like goldens: `PARITY_REGEN=1`
writes them, a normal run compares them (`A1: leg path matches the sidecar`), and a
non-empty check keeps the JS exemption from going vacuous.

`test_panel_parity.js` block [4] now asserts four things instead of two:

1. no pixel is lit on the page but dark in the firmware (unchanged);
2. **every firmware-only lit pixel sits on a coordinate `a1Leg()` actually painted**;
3. every exempted leg pixel is still `cQuiet`-hued (the old hue check, kept, now applied
   *on top of* the coordinate check rather than instead of it);
4. the exemption count cannot exceed the painted leg path (73 ≤ 96 in both variants).

**Both detectors were verified by negative test.** Deleting a single real coordinate
(`7 2`) from the sidecar produced, from the two sides independently:

```
FAIL - primary gear: every firmware-only lit pixel sits on a real a1Leg() coordinate  [7,2=quiet]
FAIL: A1: leg path matches the sidecar — leg path drifted: 1 newly painted, 0 no longer painted
```

**Residual weakness, stated precisely** (and written into both the test header and the
block [4] comment): this proves *position*, not *brightness*. A leg whose brightness changed,
or a non-leg element that moved onto a leg coordinate and lit it, is still forgiven by block
[4]. What pins leg brightness is the committed C golden, which is a full RGB frame — block
[4]'s job is only to bound the C-vs-page divergence. A second, narrower gap: the palette-
identity trick is sound only while `cQuiet` has a single user inside A1. If a future A1
element adopts `cQuiet` the sidecar over-collects, which is why assertion 4's count cap is
there as a backstop rather than being redundant.

## Additional UNVERIFIED from this round

11. **The CI refusal is verified by hand, not by a test.** It is a process-exit path, so
    asserting it from inside the same process is not possible; a shell-level check in the
    Makefile was considered and skipped as more machinery than it earns. If someone
    refactors `main()` and drops the call, nothing fails — the comment above the function
    is the only guard.
12. **`envTrue()` interprets `CI` heuristically.** GitHub Actions and the common runners set
    `CI=true`/`CI=1`, but a CI system that sets some other spelling, or none, gets no
    protection. The refusal is a safety net, not a guarantee.
13. **The corpus is small and hand-picked.** Ten malformed inputs are not a fuzzer. Deeply
    nested input, numeric edge cases (huge exponents, `-0`, leading zeros) and duplicate
    keys are still untested, and `testJson` remains a harness convenience that should not be
    reused elsewhere.
14. **The leg sidecar is only as good as its regeneration discipline.** Like the goldens, a
    regen that is committed without reading the diff launders a real change into the
    expectation. The count cap and the golden bound the damage; they do not prevent it.
