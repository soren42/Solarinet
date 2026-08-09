# RETURN-T1

## STATUS

complete — both deferred SHOULDs closed, no production code touched.

## ARTIFACTS

tests/dashboard/test_lifecycle_ui.js   (block [11] added)
tests/dashboard/test_panel_aw.js       (block [9] added, export list extended)
status-panel/RETURN-T1.md

```text
$ git status --porcelain
 M tests/dashboard/test_lifecycle_ui.js
 M tests/dashboard/test_panel_aw.js
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

## GATES RUN

```text
$ node tests/dashboard/test_jsx_parse.js
16/16 parsed

$ node tests/dashboard/test_layout.js
all assertions passed

$ node tests/dashboard/test_panel_aw.js
159 passed, 0 failed

$ node tests/dashboard/test_lifecycle_ui.js
84 passed, 0 failed
```

## UNVERIFIED

- **The fake-timer harness in block [11] is a synthetic clock, not a real
  browser event loop.** `advance(ms)` fires due callbacks synchronously and
  in registration order; it does not model re-entrant `setTimeout` calls
  scheduled from within a firing callback (not needed here — `withGiveUp`
  schedules exactly one timer per call — but a future test reusing this
  helper for a multi-timer scenario should check that before trusting it).
- **`captureSend()` in block [9] never renders `VirtualPanel` or
  `PanelControls` themselves**, only intercepts the element carrying `send`
  before either executes. It does not prove the on-screen buttons wire to
  this exact `send` reference at click time in a live browser — that
  wiring is asserted structurally (same JSX prop, same closure) but not
  observed through a real click event or a real DOM.
- **No real network call was made in either deliverable.** Both stub the
  layer directly beneath the component (`window.SOLARI.api.post` /
  `window.SolariAPI.post`), consistent with every other test in both files —
  this proves what the UI sends, not what the server does with it.
- **`send`'s own 12 s give-up timer (screens-panel.jsx, distinct from
  screens.jsx's 15 s `withGiveUp`) is still unexercised** — block [9] gives it
  a real `setTimeout` so the call doesn't throw, but nothing in this task
  drives it to fire. Out of scope for T1 (the assignment named the 15 s
  `withGiveUp` specifically); flagging in case a future SHOULD wants the same
  treatment applied to this second timer.
- Neither block was reviewed cross-lab before this return; per the operator
  standard that gate belongs to whoever routes T1's review.

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
