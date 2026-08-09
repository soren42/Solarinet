# RETURN-T1

## STATUS

complete — cross-lab review (BLOCK: 1 MUST + 2 SHOULDs) addressed in a second
round; all four suites green; no production code touched (one temporary,
fully-reverted mutation-test edit — see REVIEW-FIX ROUND below).

## ARTIFACTS

tests/dashboard/test_lifecycle_ui.js   (block [11] added, then hardened per MUST)
tests/dashboard/test_panel_aw.js       (block [9] added, export list extended, then fixed per SHOULD #2/#3)
status-panel/RETURN-T1.md

```text
$ git status --porcelain
 M tests/dashboard/test_lifecycle_ui.js
 M tests/dashboard/test_panel_aw.js
 M status-panel/RETURN-T1.md
```

## DELIVERABLE 1 — give-up timer test (test_lifecycle_ui.js block [11])

`withGiveUp` (`dashboard/public/screens.jsx:68`, `LC_GIVE_UP_MS = 15000`) is now
exercised end to end instead of only read. The block:

- stubs `env.win.SOLARI.api.post` to return a `Promise` that never settles
  (so the ONLY way the caller's promise can resolve is the give-up firing);
- swaps `env.win.setTimeout`/`clearTimeout` for a manual fake-timer queue
  (`advance(ms)` moves a virtual clock and fires due callbacks synchronously)
  so the 15 s wait costs no real wall time;
- clicks `CriticalityControl`'s "Vital" tier button, `advance(14999)` and
  asserts nothing has fired yet, then `advance(2)` to cross the 15000 ms
  threshold;
- flushes the resulting promise chain with the same short real 20 ms
  `setTimeout` idiom `failureBlock()` above it already uses (not a 15 s
  sleep — a microtask-flush wait, same pattern already in the file);
- asserts (a) the caller's promise rejected and the optimistic pick rolled
  back to the server's tier, (b) a toast fired matching `/failed/i` and
  `/15 s/`, and (c) the control's own error text names the 15 s reason.

3 new assertions (84 total in the file, up from 80). Wired into the existing
async chain: `failureBlock().then(giveUpBlock).then(assetDetailBlock).then(done)`.

**No real 15 s sleep was added.** Full-suite wall time is unchanged from
baseline (~52 s both before and after this change, dominated by repeated
Babel transforms across the file's many `load()` calls — verified by running
the pre-change file with `git stash`).

## DELIVERABLE 2 — kinds 8/9 send-path body assertions (test_panel_aw.js block [9])

Read `dashboard/public/screens-panel.jsx`:

- `send` (defined inside `PanelScreen`, ~L2233) posts
  `A.post("/api/panel/command", { kind: kind, arg: arg })` — exactly the
  `{kind, arg}` shape, same as every other command kind (1–7); no discrepancy
  with CONTRACT-AW.md's "validates ranges... exactly like kinds 1–7".
- `cmdGroup` (~L1466): `kind===8 → "screenEn:"+(arg>>1)`, `kind===9 →
  "screenWt:"+(arg>>3)`, else the CP-era key — matches §10 A-2 exactly and
  matches CONTRACT-AW.md's normative D2 encoding (`screenIdx = theme*3 +
  slot`, kind 8 `(screenIdx<<1)|enabled`, kind 9 `(screenIdx<<3)|weightCode`).
  **No code/contract discrepancy found** — verified against `packScreenEn`/
  `packScreenWt`, both already covered by block [4]'s existing assertions and
  cross-checked again here.

Block [4] already asserted the packing math and `cmdGroup` keying, but only
against a **mock** `send` prop handed to `ScreenTileCfg` — it never ran the
real `send` closure that actually calls `A.post`. Block [9] closes that gap:

- extended the test's export-substitution list (the same
  `EXPORT_ANCHOR`-replacement trick the file already uses to expose
  internals for testing) to also export `PanelScreen` — no production file
  was touched; the substitution happens only in the test's in-memory copy of
  the source before compilation, identical in kind to every other name
  already in that list.
- `captureSend()` — a small dedicated recursive walker (not the shared
  `render()`) that runs `PanelScreen`'s component function, walks its
  returned element tree, and captures the first function-component element
  carrying a `send` prop **without invoking it** — sidestepping `VirtualPanel`
  (env is null so it doesn't render) and `PanelControls`'s own body
  (needs no `canvas`/`document` stubs beyond what's already present).
  `PanelControls` always renders with `send={send}`, the exact same closure
  `VirtualPanel` would also receive were `env` truthy.
- stubbed `window.SolariAPI = { post, get }` (the `send` closure calls
  `api()` → `window.SolariAPI`), plus real `setTimeout`/`clearTimeout` on the
  sandbox `window` object (`send`'s own internal 12 s give-up timer needs a
  real function to call, not the fake queue from Deliverable 1 — it's never
  exercised since the post stub resolves synchronously-in-effect before any
  wait would matter, and `process.exit()` at the end of the file discards any
  leftover real timer regardless).
- called the captured `sendFn(kind, arg)` directly and asserted the **exact**
  JSON body pushed to the stub: kind 8/{screenIdx=1,11} enable+disable, kind
  9/{screenIdx=1,11} weight, at both encoding extremes (screenIdx 1 and 11 —
  11 is where `(11<<3)|5=93` is the largest legal value).
- re-asserted `cmdGroup` keying (`screenEn:<idx>` / `screenWt:<idx>` / CP-era
  keys for other kinds) explicitly per this deliverable's wording, alongside
  block [4]'s pre-existing coverage of the same contract.

9 new assertions, all passing. Full file: 159 passed, 0 failed (up from 150).

## REVIEW-FIX ROUND (cross-lab review of `52ce2f2` returned BLOCK: 1 MUST + 2 SHOULDs)

### MUST — test_lifecycle_ui.js block [11]: pre-threshold check was vacuous

The original `advance(14999)` assertion checked `env.toasts.length === 0`
**synchronously, immediately after** the fake-clock advance — before any
microtask from a `reject()` inside `withGiveUp` could run. The reviewer
proved this by mutation testing: temporarily changing the production
`ms || LC_GIVE_UP_MS` delay to a hardcoded `1` (keeping the "15 s" error
text unchanged) left the original suite green at 84/84 — the 15 s value
itself was never actually pinned by any assertion.

Fix, two independent mechanisms:
- **Direct timer-firing tracking.** The fake `setTimeout` stub now records
  `{id, delay}` into a `fired` array only when `advance()` actually invokes
  a callback. A new assertion pins `timers[0].delay === 15000` at schedule
  time, and a second pins `fired.length === 0` after `advance(14999)` — both
  synchronous, no flush needed, and both fail immediately under the delay
  mutation.
- **Flush-before-assert.** Added a real 20 ms `setTimeout`-based `flush()`
  (same idiom already used elsewhere in the file) before every assertion
  that depends on the promise chain having settled — the pre-threshold
  "still nothing visible" check and the post-threshold toast/rollback
  checks. `advance(1)` (crossing exactly to 15000 ms) is followed by
  `flush()` before the give-up's toast and rollback are asserted.

Net: 3 assertions in block [11] → now 6 (see updated counts below).

**Non-vacuity proof performed, per the reviewer's instruction:**

```text
$ # temporarily edited dashboard/public/screens.jsx:
$ #   }, ms || LC_GIVE_UP_MS);
$ #   →
$ #   }, 1 /* MUTATION-TEST ONLY: was `ms || LC_GIVE_UP_MS` — text still says 15s, fires at 1ms */);
$ node tests/dashboard/test_lifecycle_ui.js 2>&1 | tail -6
FAIL - the give-up timer fires exactly once, and only at the 15000 ms mark  [[{"id":1,"delay":1}]]
...
83 passed, 4 failed

$ # reverted the edit
$ git diff dashboard/public/screens.jsx
(empty — file restored exactly to its committed state)
```

The mutation was caught (4 failures, including the new delay-pinning
assertion showing the actual fired delay of `1` instead of `15000`), and the
production file was confirmed byte-identical to its committed state after
reverting.

### SHOULD #2 — test_panel_aw.js block [9]: real timers made the block order-dependent

Block [9] previously did `win.setTimeout = setTimeout; win.clearTimeout =
clearTimeout;` for `send`'s internal 12 s give-up timer, relying on the
file's final `process.exit()` to discard any leftover real OS timer —
correct only because nothing runs after this block today.

Fix: replaced with inert local stubs —

```js
win.setTimeout = function () { return 0; };   // send()'s 12s give-up: an inert
win.clearTimeout = function () {};            // stub — no real OS timer is ever armed
```

No real timer is armed, so the block needs no cleanup and is now
position-independent regardless of what runs after it.

### SHOULD #3 — test_panel_aw.js block [9]: captured `send` was never exercised through the real component tree

`captureSend()` pulled the real `send` closure out of `PanelScreen` but the
block only ever invoked it directly with hand-picked `(kind, arg)` pairs.
Combined with block [4]'s use of a **mocked** `send` prop for
`ScreenTileCfg`, a break in the actual `PanelScreen → VirtualPanel →
ScreenTileCfg` wiring (wrong prop name, disconnected reference, wrong `idx`)
would never be caught by either test — both paths were verified in
isolation, never connected.

Fix: added assertions that mount the **real** `VirtualPanel` with the
**captured real** `send` (not a mock), then find actual rendered
`<figure>` tiles and click actual rendered `<button>` elements inside them:

- confirms `VirtualPanel` renders twelve tiles;
- clicks A1's (screenIdx 1) real rendered "Off" button → asserts the exact
  POST body `{kind: 8, arg: 2}`;
- clicks A1's real rendered "5×" button → asserts `{kind: 9, arg: 12}`;
- clicks D2's (screenIdx 11, the top edge of both encodings) real rendered
  "¼×" button → asserts `{kind: 9, arg: 88}`.

This exercises the full production prop chain end to end — a wiring break
anywhere between `PanelScreen` and `ScreenTileCfg`'s button `onClick` now
fails these assertions.

## UPDATED ASSERTION COUNTS

- `tests/dashboard/test_lifecycle_ui.js`: **87 passed, 0 failed** (was 84/84;
  MUST fix added 3 pinning/flush assertions to block [11]).
- `tests/dashboard/test_panel_aw.js`: **164 passed, 0 failed** (was 159/159;
  SHOULD #3 fix added 5 real-wiring assertions to block [9]).
- `tests/dashboard/test_jsx_parse.js`: unchanged, 16/16 parsed.
- `tests/dashboard/test_layout.js`: unchanged, all assertions passed.

## GATES RUN (final, after review-fix round)

```text
$ node tests/dashboard/test_jsx_parse.js
16/16 parsed

$ node tests/dashboard/test_layout.js
all assertions passed

$ node tests/dashboard/test_panel_aw.js
164 passed, 0 failed

$ node tests/dashboard/test_lifecycle_ui.js
87 passed, 0 failed
```

## UNVERIFIED

- **The fake-timer harness in block [11] is a synthetic clock, not a real
  browser event loop.** `advance(ms)` fires due callbacks synchronously and
  in registration order; it does not model re-entrant `setTimeout` calls
  scheduled from within a firing callback (not needed here — `withGiveUp`
  schedules exactly one timer per call — but a future test reusing this
  helper for a multi-timer scenario should check that before trusting it).
- **`captureSend()` in block [9] still captures `send` out-of-band before
  invoking it** — the SHOULD #3 fix closes the biggest gap (a real
  `VirtualPanel`/`ScreenTileCfg` render tree is now exercised with the
  captured closure and real rendered buttons are clicked), but the harness
  is the test file's own shim `React`/DOM model, not a real browser. It does
  not observe an actual `click` DOM event or a real event-loop dispatch —
  the "click" is a direct call to the button element's `onClick` prop
  function, same as this file's other interaction assertions.
- **No real network call was made in either deliverable.** Both stub the
  layer directly beneath the component (`window.SOLARI.api.post` /
  `window.SolariAPI.post`), consistent with every other test in both files —
  this proves what the UI sends, not what the server does with it.
- **`send`'s own 12 s give-up timer (screens-panel.jsx, distinct from
  screens.jsx's 15 s `withGiveUp`) is still unexercised.** Per SHOULD #2 it is
  now an inert stub (`win.setTimeout = function () { return 0; }`) rather than
  a real timer, so nothing in block [9] drives it to fire or asserts its
  delay/message. Out of scope for T1 (the assignment named the 15 s
  `withGiveUp` specifically); flagging in case a future task wants the same
  MUST-style pinning treatment applied to this second timer.
- **The fake-timer `fired`-tracking added for the MUST fix only models a
  single-timer scenario** (consistent with the existing UNVERIFIED note on
  `advance()` above) — `withGiveUp` schedules exactly one timer per call, so
  this was sufficient here, but the mechanism hasn't been exercised against
  overlapping/re-entrant timers.
- The review-fix round above closes the one MUST and both SHOULDs from the
  cross-lab review of commit `52ce2f2`. This updated round has not itself
  been re-reviewed cross-lab yet; that gate belongs to whoever routes the
  next review pass.

## DECISIONS

- Chose to capture the **real** `send` closure out of `PanelScreen` rather
  than settle for block [4]'s existing mock-`send` coverage, because the
  task explicitly distinguishes "assert against what the production code
  sends" from testing a stand-in — the mock in block [4] proves
  `ScreenTileCfg`'s button wiring, not the POST body `send` itself
  constructs.
- Extended the test's own export-substitution list rather than adding an
  exported test-only global inside `screens-panel.jsx` — keeps the
  "production code untouched" rule intact and matches the file's existing
  convention exactly (same anchor, same technique, one more name).
