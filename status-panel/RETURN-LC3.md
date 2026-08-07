# RETURN-LC3

## STATUS

complete (lane); the read routes on LC2's side still have to emit the two
columns before any of it is visible

## ARTIFACTS

dashboard/public/screens.jsx
dashboard/public/screens3.jsx
dashboard/public/styles.css
dashboard/public/api.jsx   (§8 A-1 grant, passthrough only)
tests/dashboard/test_lifecycle_ui.js   (new — the acceptance harness)
tests/dashboard/test_jsx_parse.js      (new — the Babel parse gate)
status-panel/RETURN-LC3.md

Zero files outside IN-SCOPE-plus-the-grant were touched by this lane.
`git status --porcelain` also lists LC1's C/migration files and LC2's
`dashboard/api/**` files — those are the sibling lanes working in the same tree,
not this one. Lane proof:

```text
 M dashboard/public/api.jsx
 M dashboard/public/screens.jsx
 M dashboard/public/screens3.jsx
 M dashboard/public/styles.css
?? tests/dashboard/test_jsx_parse.js
?? tests/dashboard/test_lifecycle_ui.js
?? status-panel/RETURN-LC3.md
```

The api.jsx diff is +10 / -0: five lines of passthrough (`criticality` and
`lifecycle` in `mapNode` and `mapAssetNode`, `tombstone` in `mapDiscovered`) and
five lines of comment citing A-1/A-2/A-3, at the comment density of the
surrounding mappers. No other line of api.jsx changed.

`screens2.jsx` was checked first and left alone: Discovery renders in
`screens3.jsx`, not `screens2.jsx`. No git operations were run beyond `status`
and `diff`; nothing was staged, committed, or deployed.

## WHAT SHIPPED

**Shared surface** (`screens.jsx`, new LIFECYCLE + CRITICALITY block, exported on
`window` so `screens3.jsx` reads it lazily and script order stays irrelevant):

- `TIERS` — the five tiers with their one-line offline-behaviour text, worded
  from §3.2 semantics rather than restating the column.
- `CriticalityControl` — compact segmented control (not a dropdown), optimistic
  selection, rolls back and toasts on failure, and renders as a plain chip with
  zero buttons for viewers.
- `LifecycleActions` — Decommission / Restore / Delete / Purge with the existing
  `DangerModal` + `btn-danger` idiom. Decommission/delete/restore are `confirm:true`
  modals; purge is a typed-name modal and is only rendered for `role === 'admin'`.
- `TierChip` / `LifecycleChip` — subtle and monochrome, accent only at tier 4,
  dimmed at tier 0, nothing at all at tier 2 or when the field is absent.
- `SolariLC` — the four §3.3 calls, each wrapped in a 15 s give-up that mirrors
  `screens-panel.jsx` `send()`. A 404/501 toasts and clears the pending state; it
  never wedges the control.

**FleetOverview** — deleted hidden by default, decommissioned/retired greyed via
`lc-dormant`, Deleted and Retired filter chips carrying live counts, tier chips
on the host cell.

**NodeDetail** — criticality bar under the hero, plus a Retire action in the
existing action row (nodes have no tombstone column, so they get retire, not the
decommission→delete→purge ladder).

**AssetDetail** — `LifecycleActions` in the page head, lifecycle chip on the
title, criticality bar under the KPI row.

**ServiceDetail** — a `page-head__right` was added (it had none) with a
"Stop monitoring" danger action, and a criticality bar that edits the **parent
asset** with the hint "inherited from <asset> — changing it here changes it for
the whole system", because §3.2 gives services no tier of their own.

**Discovery** — a warn callout above the table listing hosts that matched a
tombstoned asset, plus a `reappeared` chip on the affected rows. The callout is
built from all rows, not the filtered view, because these arrive as status
`ignored` and the default "New" filter would otherwise bury them; a "Show them"
button switches the filter.

**styles.css** — new classes only, appended. Nothing existing was edited.

## VERIFIED

1. **Parse.** Both touched JSX files transform clean under the dashboard's own
   vendored `vendor/babel.min.js` with `presets:['react']`.

   Landed as `tests/dashboard/test_jsx_parse.js`; it now covers **all fifteen**
   `dashboard/public/*.jsx`, not just this lane's three, because a syntax error
   in any of them blanks the page identically.

```text
$ node tests/dashboard/test_jsx_parse.js
  ok   - api.jsx
  ok   - screens.jsx
  ok   - screens3.jsx
  … (15 files)

15/15 parsed
```

2. **Behaviour harness**, landed as `tests/dashboard/test_lifecycle_ui.js`.
   A headless node harness with a minimal React shim
   renders the real components out of `screens.jsx` and asserts 43 properties
   across six blocks — role gating (viewer sees no mutation affordance; operator
   never sees Purge even on a deleted asset; admin does), the DangerModal flow
   (nothing posts before confirm; delete posts
   `/api/assets/7/lifecycle {action:"delete",confirm:true}`; purge demands the
   exact name), the tier selector (five tiers, correct labels and `aria-pressed`,
   posts `{tier:4}` to the asset route and `{tier:0}` to the node route,
   read-only for viewers), the chips, the failure path (rejected post rolls the
   selection back, toasts, and marks the control), and FleetOverview filtering.

```text
$ node tests/dashboard/test_lifecycle_ui.js

43 passed, 0 failed
```

   **Superseded by the fix round** — the harness now runs ten blocks and
   80 assertions, and loads `screens3.jsx` too. See FIX ROUND below for the
   current output; the numbers above are the pre-fix-round record.

   Both files resolve `dashboard/public` from `__dirname`, carry no scratchpad
   paths, and exit non-zero on failure. Verified to run identically from the
   repo root and from `/`. They follow `tests/dashboard/test_layout.js` — same
   `ok   - ` / `FAIL - ` output, same shebang-without-exec-bit, same
   `node tests/dashboard/<file>` invocation. Nothing wires `tests/dashboard`
   into CMake today (`test_layout.js` isn't either), so these are manual
   battery entries until someone adds a target; `tests/CMakeLists.txt` was out
   of scope and untouched.

   Note that block [6]'s fixture now puts `criticality`/`lifecycle` on the fleet
   rows themselves, mirroring what `mapAssetNode` emits post-grant. Two
   workaround fetches this lane had added while the mapper still dropped the
   fields — an extra `/api/nodes` read in FleetOverview and an extra
   `/api/nodes/{id}` read in NodeDetail — were deleted; both now read the fields
   off the record they already have.

3. **Contract alignment against LC2's landed routes**, read directly from
   `dashboard/api/routes/assets.php` and `control.php`: paths, bodies, the
   `confirm:true` requirement, the integer 0..4 tier, and the admin gate on purge
   all match. Two mismatches were found this way and fixed in this lane:
   - purge `confirmName` now derives as `displayName || host || ip`, matching the
     server's `displayName`-or-`host` check (it was `displayName || ip`, which
     would have made purge unconfirmable for a nameless asset);
   - the discovery notice now reads the frozen §8 A-2 field,
     `tombstone: {assetId, lifecycle, displayName} | null` — my earlier
     speculative shapes are gone, and it renders only on non-null.

## DEVIATIONS

**D1 — purge `confirmName` derivation changed (reviewer: look here).** The UI
originally derived the name the operator must type back as
`displayName || ip`. LC2's server checks it against `displayName`-or-`host`.
For any asset with no `displayName`, the UI would have demanded the IP while
the server demanded the hostname, and **purge would have been impossible to
confirm** — a dead end reachable only on exactly the nameless, ambiguous assets
most likely to need purging. Now `displayName || host || ip`, matching the
server's order. Caught by reading `routes/assets.php`, not by a test; the
harness asserts the happy path (`confirmName === "pihole-01"`) only, because a
nameless-asset fixture would assert my own assumption about the server rather
than the server.

**D2 — harness landed in `tests/dashboard/`, not `tests/ui/`.** The grant said
`tests/ui/` (new dir OK), but `tests/dashboard/` already exists and already
holds exactly this kind of test — `test_layout.js`, a headless node test of a
`dashboard/public/*.jsx` file. A second directory for the same category would
split the battery. Reversible with one `git mv` if you want the new dir anyway;
say so and I'll move both files.

## UNVERIFIED

- **No live browser render.** Nothing was loaded in a real browser against a real
  server. Layout, spacing, and the dark-theme look of the new CSS are unexercised;
  they follow existing house classes and variables but have not been seen.
- **No live endpoint was called.** Every assertion is against a spying stub. The
  routes exist in LC2's working tree but were never exercised end-to-end from
  this UI, and the migration has not been applied here.
- **The Discovery reappearance notice has never rendered with real data.** It is
  asserted only by reading LC2's serialiser against the frozen A-2 shape.
  (The "no harness block" half of this gap is now closed — block [8] covers the
  notice across five tombstone shapes. Still stubs, still no real row.)
- `api.removeAsset` is now unused by AssetDetail (see below) and was left in
  place per the lead's ruling; no caller was audited beyond the files in scope.
- **The api.jsx passthrough is verified by reading, not by executing.** The
  mappers are private to that module's IIFE, so the harness cannot call them
  directly; what it proves is that the UI consumes row-level `criticality` /
  `lifecycle` correctly once they arrive. The five added lines are literal
  passthrough and should be read in the diff.

## OPEN — LC2 read routes still do not emit the new columns

Half of the blocker I raised is closed: the A-1 grant landed and api.jsx now
passes the fields through. The server half is still open and belongs to LC2:

- `routes/assets.php` — both `GET /api/assets` and `GET /api/assets/{id}` select
  a fixed column list that omits `criticality` and `lifecycle`.
- `routes/nodes.php` — `GET /api/nodes` and `GET /api/nodes/{id}` likewise omit
  `criticality`.

Until those four SELECTs carry the columns, tier chips show nothing, the
selector reads "not set", and the Deleted/Retired filters find nothing to hide.
That is A-3 behaving correctly, not a UI bug — absent renders as unknown, never
as tier 0. `panel.php` already guards the same columns through
`information_schema`; the same guard is the pattern to copy.

The discovery notice is unblocked end to end: LC2's read-time join emits
`tombstone`, `mapDiscovered` now passes it, and the UI renders on non-null.

## DECISIONS TAKEN

- **AssetDetail's legacy "Remove system" flow was retired** in favour of the
  contract's decommission → delete → purge ladder. Keeping both would have given
  the same page two different destructive vocabularies. Lead ACCEPTED; matches
  §3.1's rework-is-not-a-new-mechanism stance. `api.removeAsset` stays in place
  and dies in a later cleanup pass, not this branch.
- ~~**`canOperate()` is permissive when the operator profile is absent**~~
  **REVERSED in the fix round (MF-1).** Both predicates are now fail-closed: an
  absent, malformed, or unrecognised profile resolves to viewer. See FIX ROUND.
- **Absent column renders "not set", never tier 0.** Asserted in the harness.

## FOLLOW-UPS

1. LC2 (routed by the lead): emit `criticality`/`lifecycle` from the four asset
   and node read SELECTs. Nothing on the read side renders until this lands.
2. A live smoke pass once the migration is applied — this lane cannot
   self-verify past the stub boundary.
3. ~~A harness block for the Discovery reappearance notice~~ — **done in the fix
   round**, block [8]. `screens3.jsx` loads in the sandbox after `screens.jsx`.

---

# FIX ROUND (post cross-lab review)

Codex verdict FIX-THEN-SHIP; lead-authorised. All four MUST-FIX items and both
SHOULDs are applied. Same IN-SCOPE files plus the already-granted `api.jsx`;
nothing new was touched, nothing was committed or deployed.

## MF-1 — fail-closed role gating (the big one)

`screens.jsx` resolved the role permissively: `operator === null` — the exact
state `api.jsx` sets when there is no session, and the state after a failed
`whoami` — returned **true** from `canOperate()`. Every mutating affordance in
the lane was therefore visible to an unauthenticated or half-loaded page.

Replaced with the same whoami-only resolution `screens-panel.jsx` uses
(CONTRACT-CP §10): one `opRole()` that returns `null` for a missing or
non-string role, `"viewer"` for any string that is not `operator`/`admin`, and
otherwise the lowercased role. `canOperate()` and `canAdmin()` both derive from
it, so unresolved identity is read-only with no exceptions.

`CriticalityControl` also changed default: `readOnly` now defaults to
`!canOperate()` rather than to `false`, and the read-only branch renders the
value as an inert chip. A caller that forgets the prop gets the safe answer.

## MF-2 — screens3's pre-existing mutating affordances

Gated with one module-level predicate that borrows the same resolution:

```js
function canOp() { return !!(window.solariCanOperate && window.solariCanOperate()); }
```

Applied to: PoolCards delete, Assets "New pool", the Assets rename affordance
(now a plain `<span>` for viewers), the Assets pool select, AssetDetail's rename
icon, its class and pool selects, the heartbeat switch, and the target-remove
button. Two idioms, chosen deliberately: **selects and the switch are
`disabled`** so the current value stays legible, **destructive buttons are not
rendered at all**. `styles.css` gained one appended rule so a disabled control
reads inert rather than merely dim.

## MF-3 — purge modal now shows the derived name (residual only)

The ip-fallback half was stale — the lead's §9 J4 fix made the server compare
`displayName || host || ip`, which is what the UI already derived, so the
fallback **stays**. The residual was taken: the purge modal now names which
field it used, e.g. "The server checks the name below against this system's
stored name (hostname), so it has to match exactly." An operator purging a
nameless asset can now see *why* the string is an IP.

## MF-4 — target removal can no longer wedge the modal

`removeTargetGo` went through `window.solariWithGiveUp(...)` (the same 15 s
give-up the lifecycle calls use), toasts on failure, and rethrows so
`DangerModal` stays open rather than closing as if the removal worked. The
offline branch rejects explicitly instead of returning `undefined`.

## SHOULD — both taken

- **Tombstone shapes.** Block [8] renders Discovery against five rows:
  `tombstone` undefined, `null`, full, full-but-no-`displayName`, and partial
  `{assetId}` only. The first two render no notice at all; the last three
  render one, and only the full one names the asset.
- **`screens3.jsx:58` no longer calls a partial lifecycle "deleted".**
  `lcNoticeText` now says "a decommissioned system" / "a deleted system" only
  when the field is one of those two literals, and "a system that was removed"
  otherwise — A-3 applied to partial objects, not just absent fields.

## GATES RE-RUN

```text
$ node tests/dashboard/test_jsx_parse.js
  ok   - api.jsx … ok   - screens8.jsx     (15 files)

15/15 parsed

$ node tests/dashboard/test_lifecycle_ui.js
 … [7] fail-closed identity   [8] Discovery tombstone shapes
 … [9] Systems list gating    [10] AssetDetail gating

80 passed, 0 failed
```

Blocks [7]–[10] are new (37 assertions). [7] drives five identity shapes
through the gate: `null` profile, missing `role`, non-string `role`, an
unrecognised role string, and `"ADMIN"` uppercase — the first four render
read-only, the fifth is accepted. [10] is async: it runs the component's
effects, lets the stubbed `api.asset()` settle, then rerenders.

**One harness defect fixed while re-running, not a product defect:** `AssetDetail`
takes `toast` as a **prop** (it shadows the module-level `toast`), and the
fixture had not passed it — so `toast && toast(...)` was a silent no-op and the
failure-notice assertion failed even though the product code was correct.
The fixture now passes it exactly as `app.jsx:250` does. Worth noting because
the harness came within one assertion of certifying a component in a
configuration the real app never uses.

## FIX-ROUND UNVERIFIED

- **The 15 s give-up timer itself is never exercised.** Block [10] asserts the
  rejection and the toast on an immediately-failing call; no test holds a
  promise open for 15 s, so the timer path is verified by reading
  `withGiveUp` (shared with the already-shipped lifecycle calls), not by
  running it.
- **Still no live browser render, and still no live endpoint call.** Everything
  above the stub boundary is unchanged from the original packet — including all
  of the new CSS.
- **The gating is client-side only.** It hides affordances; it is not a
  security boundary. A viewer who calls the routes directly is stopped by
  LC2's server-side checks or by nothing — that is LC2's surface, not audited
  here.
- **`screens3.jsx`'s gated affordances were audited at the eight review-named
  line numbers plus what I found reading around them.** I did not sweep the
  whole 2 kloc file for every mutating control; a mutating affordance outside
  those regions could still be ungated.
