# REVIEW-AW — adversarial cross-lab review of lanes AW1 + AW2

`reviewer: claude/opus-5 (cross-lab: reviewing gpt-5.6-codex authorship + two Lead-authored fixes)`
`scope: CONTRACT-AW.md §1-§10, RETURN-AW1.md (incl. FIX ROUND A-4), RETURN-AW2.md`
`tree: /home/jason/Code/Solarinet @ feat/flow-gates-weights, uncommitted`
`constraints honoured: no git ops, no deploys, no service restarts, no live-DB writes, NO FLASHING.`
`All mutation probes were reverted; working tree diffstat verified identical to session start (see §6).`

---

## 0. Verdict table

| Component | Verdict | Blocking findings |
|---|---|---|
| `protocol.h` / `protocol.c` codec | **CONDITIONAL** | M2 (untested length guard) |
| `status-panel/daemon/solariPanel.c` | **REJECT** | **M1 (gear never reaches the wire — A1 is dead end-to-end)**, S1 |
| `firmware/panelScreensA.c` (A1 renderer) | **REJECT** | **M3 (stack-buffer-overflow, ASAN-confirmed)**, M4 |
| `firmware/panelScreenCfg.c/.h` + flash path | **CONDITIONAL** | S2, S3 |
| `firmware/main.c` (rotation, rescale, CONFIG emit) | **CONDITIONAL** | M5 (D5 implementation is untested), S3 |
| `firmware/panelCtl.c/.h` (kinds 8/9) | **ACCEPT** | — |
| `dashboard/api/routes/panel.php` | **CONDITIONAL** | M6 (A-4 mapper has no regression test), S4 |
| `db/migrations/019_panel_screen_config.sql` | **ACCEPT** | N1 only |
| `deploy/unifi/unifipolld.py` + unit | **CONDITIONAL** | S5, S6 |
| CMake reserved-sector guard | **ACCEPT** (RETURN-AW2 understates it — it exists) | N2 |

Bottom line: **do not flash yet.** Two defects (M1, M3) mean the feature would not work
and would fault under a routine partial-fleet condition that the real inventory reaches
whenever a hub drops out for 60 s. Both are small, localized fixes.

---

## 1. Mandatory check 1 — both suites run clean

### Daemon (`status-panel/daemon && make test`)

```
./tests/codec_test
2026-08-07T08:13:59Z solariPanel: withholding panel commands until STATE confirms CONTROL capability
2026-08-07T08:13:59Z solariPanel: forwarded panel command id=21 kind=1 arg=0
codec tests passed
```

### Firmware host suite (`status-panel/firmware/test && make`, after `make clean`)

```
cc -std=c11 -Wall -Wextra -Werror -O1 -g -o panelLinkTest panelLinkTest.c ../panelLink.c ../../protocol.c
cc -std=c11 -Wall -Wextra -Werror -O1 -g -o panelCtlTest panelCtlTest.c ../panelCtl.c ../../protocol.c
cc -std=c11 -Wall -Wextra -Werror -O1 -g -o panelScreenCfgTest panelScreenCfgTest.c ../panelScreenCfg.c ../../protocol.c
...
all panelLink cases pass
...
ok:   case 15: SCREENEN accepts flattened screen 11
ok:   case 15: SCREENWT accepts flattened screen 11 weight 5
ok:   case 15: SCREENEN index 12 is rejected and consumed
ok:   case 15: SCREENWT weight 6 is rejected and consumed
all panelCtl cases pass
./panelScreenCfgTest
ok:   A4: absent flash selects enabled 1x defaults
ok:   A4: defaults with no flash record are not cfgPersisted
ok:   A4: rapid changes debounce to one flash write
ok:   A4: persisted config survives simulated reboot and reports cfgPersisted
ok:   A4: bad magic falls back safely
ok:   A4: corrupt or absent config falls back safely
ok:   A4: torn flash tail fails CRC and falls back to unpersisted defaults
ok:   A2: synthetic clock advances at 150/15/30 seconds (+/-1)
ok:   A3: rotation skips a disabled screen
ok:   A3: all-disabled active theme falls back to all enabled
ok:   A2: 20/30*7.5 = 5 seconds, no immediate advance
ok:   A2: rescaled elapsed at new dwell advances immediately
ok:   A5: v1 additive nine-gear snapshot round trip
ok:   A6: legacy payload with no gear section decodes as no gear
ok:   A6: oversized additive gear count decodes as no gear
ok:   A5: CONFIG round trip
ok:   A5: CONFIG rejects nonzero screenCfg reserved bits
ok:   A5: CONFIG rejects nonzero reserved payload bytes
ok:   A5: parser dispatches v1 additive SNAPSHOT, CONFIG, and kinds 8/9
ok:   A5: 0x84/0x85 dispatch; valid 0x86 skips without desync
```

`grep -c "^ok:"` → **142**, zero FAIL. RETURN-AW2's "142 ok, 0 fail" reproduces exactly.

---

## 2. Mandatory check 2 — mutation verification (break, run, restore)

| # | Mutation | Caught? | By |
|---|---|---|---|
| (a) | `knownType()` upper bound `PANEL_FT_CONFIG` → `PANEL_FT_STATE` (0x85 no longer admitted) | **YES** | daemon `CONFIG parser dispatch missing`; firmware `FAIL: A5: parser dispatches …` + `FAIL: A5: 0x84/0x85 dispatch …` (142 → 140 ok) |
| (b) | v1-additive gear **length** guard dropped: `if (gears > PANEL_MAX_GEAR \|\| len < off + 4u*gears)` → `if (gears > PANEL_MAX_GEAR)` | **NO** | daemon `codec tests passed`; firmware 142 ok, 0 fail → **finding M2** |
| (c) | D5 dwell rescale in `main.c` | **NO — unreachable** | `main.c` is in no test binary → **finding M5** |
| (d) | `panelGearState()` `2 → 0` arm swapped to `2 → 2` | **NO** | no PHP test references the mapper at all → **finding M6** |
| (e1) | CONFIG `screenCfg` reserved-bit mask `0xf0u` → `0x00u` | **YES** | firmware `FAIL: A5: CONFIG rejects nonzero screenCfg reserved bits` |
| (e2) | CONFIG reserved **bytes** 13/14/15 check removed | **YES** | firmware `FAIL: A5: CONFIG rejects nonzero reserved payload bytes` |

Quoted mutation (a) output:

```
184:static int knownType(uint8_t type) { return type >= PANEL_FT_SNAPSHOT && type <= PANEL_FT_CONTROL ? 1 : (type >= PANEL_FT_HELLO && type <= PANEL_FT_STATE ? 1 : 0); }
./tests/codec_test
CONFIG parser dispatch missing
make: *** [Makefile:13: test] Error 1
FAIL: A5: parser dispatches v1 additive SNAPSHOT, CONFIG, and kinds 8/9
FAIL: A5: 0x84/0x85 dispatch; valid 0x86 skips without desync
```

Quoted mutation (b) output (the silent one):

```
175:  if (gears > PANEL_MAX_GEAR) return 0;
codec tests passed
142            <- firmware ok-count unchanged, zero FAIL
```

Quoted mutation (c) unreachability proof — `status-panel/firmware/test/Makefile:10-13`:

```
LINK_SRC = panelLinkTest.c ../panelLink.c ../../protocol.c
CTL_SRC  = panelCtlTest.c ../panelCtl.c ../../protocol.c
CFG_SRC  = panelScreenCfgTest.c ../panelScreenCfg.c ../../protocol.c
```

Neither `main.c` nor `panelScreensA.c` appears in any test target.

Quoted mutation (d) unreachability proof:

```
$ grep -rn "panelGearState|panelGearRole|panelGearLogScale" tests/ deploy/ status-panel/ dashboard/public/
status-panel/RETURN-AW1.md:...      (packet prose only)
status-panel/RETURN-AW3.md:...      (packet prose only)
status-panel/CONTRACT-AW.md:279     (contract prose only)
```
Zero code references outside `panel.php` itself. `tests/dashboard/test_panel_aw.js` is JS and
cannot reach a PHP function; the only PHP tests in the repo are `test_solari_ctl.php` and
`test_rackwire_api.php`, neither of which touches `panel.php`.

---

## 3. MUST-FIX

### M1 — the gear section never reaches the wire; A1 is dead end-to-end
`status-panel/daemon/solariPanel.c:71-95` (`parseSnapshot`)

`parseSnapshot()` builds `PanelSnapshot staged` from the `GET /api/panel` JSON. It reads
`ts, score, stateRoll, alerts, pools, systems, topAlert, rxKbps, txKbps, rttTenthMs,
lossPermille`. It **never reads `gear` or `gearCount`.**

```
$ grep -n "gear\|Gear" status-panel/daemon/solariPanel.c
(no output)
```

`staged.gearCount` therefore stays 0 through `memset`, `panelEncodeSnapshot` emits a
gearCount-0 trailer, `panelDecodeSnapshot` yields `gearCount == 0`, and
`panelScreenA1()` takes its `panelScreenNoData(t); return;` path forever.

**Failure scenario:** flash the firmware, deploy `unifipolld`, apply migration 019, and A1
still shows NO DATA permanently, with no error anywhere — panel.php serves the gear array
correctly, the firmware renders it correctly, and the daemon in between silently drops it.
This is the *identical* failure class AW3 flagged in RETURN-AW3.md against the PHP side
(helpers defined, never called); it was fixed there and reintroduced one hop downstream.

Cause is a contract gap, not lane misconduct: §5's daemon bullet names only "decode CONFIG
frames, POST /api/panel/config; forward kinds 8/9", and §8 assigns AW1 "daemon CONFIG
decode+POST". Nobody was told to ingest gear. It still has to be fixed before flash.

**Minimal fix** — in `parseSnapshot`, after the `topAlert` block (`solariPanel.c:93`):

```c
gearArr = cJSON_GetObjectItemCaseSensitive(data,"gear");
if (cJSON_IsArray(gearArr)) {
  int g = cJSON_GetArraySize(gearArr);
  if (g > (int)PANEL_MAX_GEAR) g = (int)PANEL_MAX_GEAR;
  staged.gearCount = (uint8_t)g;
  for (i = 0; i < g; ++i) {
    item = cJSON_GetArrayItem(gearArr, i);
    staged.gear[i].role    = byteValue(cJSON_GetObjectItemCaseSensitive(item,"role"), 0);
    staged.gear[i].state   = byteValue(cJSON_GetObjectItemCaseSensitive(item,"state"), 0);
    staged.gear[i].rxLevel = byteValue(cJSON_GetObjectItemCaseSensitive(item,"rxLevel"), 0);
    staged.gear[i].txLevel = byteValue(cJSON_GetObjectItemCaseSensitive(item,"txLevel"), 0);
  }
}
```
Clamp `role <= 4`, `state <= 2`, `rx/txLevel <= 7` on the way in (fail-dark: unknown role →
drop the entry, unknown state → 0). Add a `codec_test` case that drives
`panelParseSnapshotForTest()` with a 9-gear JSON fixture and asserts `gearCount == 9` —
the test seam already exists at `solariPanel.c:197`.

### M2 — the v1-additive gear length guard is untested; dropping it is an OOB read
`status-panel/protocol.c:175`

Mutation (b) removed `len < off + 4u * (size_t)gears` and **no test failed**. The guard is
the only thing stopping `panelDecodeSnapshot` from reading up to 48 bytes past the payload
on a truncated frame — on the firmware that is a read past `PanelParser.buf` into
neighbouring RAM.

Why the existing cases miss it: `A6: legacy payload with no gear section` truncates by
exactly 37 bytes so `len == off` and the *early return* at `protocol.c:173` catches it;
`A6: oversized additive gear count` sets `gears = 13 > PANEL_MAX_GEAR` so the *count* arm
catches it. Neither exercises a well-formed count with a **short** entry array.

**Failure scenario:** any frame whose payloadLen was corrupted such that CRC still passes
over the truncated content (or, more realistically, a future server that emits `gearCount`
then fewer entries) reads uninitialised/adjacent memory into `snap.gear[]`, which then
feeds `env->snap.gear[...].role` in the renderer.

**Minimal fix** — add to `panelScreenCfgTest.c` beside the other A6 cases:

```c
/* A6: a PRESENT but SHORT gear trailer is no-data, never a partial read. */
len = panelEncodeSnapshot(&sent, payload, sizeof(payload));
check(panelDecodeSnapshot(payload, len - 4u, &got) == 0 && got.gearCount == 0u,
      "A6: short additive gear trailer decodes as no gear");
```
(`len - 4u` keeps `gearCount = 9` in the payload but leaves only 8 entries' worth of bytes.)

### M3 — `panelScreenA1` reads a negative array index whenever a role band is empty (ASAN-confirmed)
`status-panel/firmware/panelScreensA.c:208-209` and `:210-211`

The particle source-selection chain subtracts before it validates:

```c
208:    } else if ((ordinal -= apCount) < hubCount && switchCount > 0) {
209:      source = &hubs[ordinal]; destination = &switches[ordinal % switchCount];
210:    } else if ((ordinal -= hubCount) < switchCount && haveRouter) {
211:      source = &switches[ordinal]; destination = &router;
```

If `ordinal < apCount` but `hubCount == 0`, branch 1 fails on its `hubCount > 0` clause,
`ordinal -= apCount` goes **negative**, and `negative < hubCount` is true — so line 209
indexes `hubs[-2]`, and `ordinal % switchCount` is also negative. Line 217 then does
`env->snap.gear[gearIndex]` with a garbage index. Same shape at 210-211 when `switchCount == 0`.

Confirmed empirically with a temporary scratchpad driver (`/tmp/.../scratchpad/a1probe.c`,
outside the repo) compiling the **unmodified** `panelScreensA.c` under
`-fsanitize=address,undefined`:

```
--- CASE 1: 2 AP + 1 switch + router, NO hubs ---
panelScreensA.c:209:21: runtime error: index -2 out of bounds for type 'A1Gate [12]'
ERROR: AddressSanitizer: stack-buffer-overflow ... READ of size 4
    #0 panelScreenA1 panelScreensA.c:217
    [416, 608) 'hubs' (line 131) <== Memory access at offset 384 underflows this variable
--- CASE 2: 2 AP + 2 hub + router, NO switch ---
panelScreensA.c:211:25: runtime error: index -2 out of bounds for type 'A1Gate [12]'
ERROR: AddressSanitizer: stack-buffer-overflow
--- CASE 3: full 9-device real fleet (router,switch,3 hub,3 ap,wanBackup) ---
SURVIVED: full fleet
--- CASE 4: APs only ---            SURVIVED
--- CASE 5: router only ---         SURVIVED
```

The happy path is safe, which is exactly why it shipped.

**Failure scenario, and it is routine:** panel.php drops any `gearInterfaceCurrent` row
older than 60 s (`panel.php:434-440`). The three USW Ultra hubs rebooting, a UniFi API
hiccup lasting >4 poll cycles (E7), or one 12 s-timeout stats fetch stalling a cycle all
produce `hubCount == 0` with APs and a switch still present. On the RP2350 the garbage
`gearIndex` (a signed int scaled by 4) indexes off `env->snap.gear[]`; benign values give
wrong colours, a large one is a bus fault — a hard panel lockup during exactly the network
event the screen exists to show.

**Minimal fix** — validate before subtracting:

```c
if (ordinal < apCount) {
  if (hubCount == 0) continue;
  source = &aps[ordinal]; destination = &hubs[ordinal % hubCount];
} else if ((ordinal -= apCount) < hubCount) {
  if (switchCount == 0) continue;
  source = &hubs[ordinal]; destination = &switches[ordinal % switchCount];
} else if ((ordinal -= hubCount) < switchCount) {
  if (!haveRouter) continue;
  source = &switches[ordinal]; destination = &router;
} else if ((ordinal -= switchCount) == 0 && haveRouter) {
  source = &router; destination = &internet;
} else {
  continue;
}
```

### M4 — the A1 renderer has no host test at all
`status-panel/firmware/test/Makefile` (no `panelScreensA.c` target)

M3 is a direct consequence. RETURN-AW2 lists this under UNVERIFIED ("A1 renderer has no
direct host-test … geometry pinned via AW3's ported-constant harness on the virtual side"),
but AW3's harness tests the *JSX* renderer — it cannot catch a C memory error in the
firmware, and CONTRACT §7/A1 requires "virtual + firmware host-test parity on the same
fixture bytes", which does not exist today.

**Minimal fix** — a `panelScreensATest` target is cheap: the probe above links
`panelScreensA.c + panelFb.c + panelHist.c + panelFont.c + protocol.c` with two stubs
(`panelScreenNoData`, `panelHwSetPixel`) and nothing else. Pin, at minimum:
gate columns 2/9/16 · 20/27/34 · 41 · 47 · 51 · 52 for the 9-device fixture; hub stagger
(`y0` 0 and 6); the down-state every-other-pixel skip; A-3 internet inheritance in all
three router states; `gearCount == 0` → no-data; **and the five partial-fleet cases above,
which must not fault.**

### M5 — the D5 dwell-rescale implementation is not the code the "passing" test covers
`status-panel/firmware/main.c:380-397` vs `status-panel/firmware/test/panelScreenCfgTest.c:71-77`

`panelScreenCfgTest.c` defines its own `rescaleDwell()` helper — a **re-implementation** of
the D5 arithmetic — and the two green cases (`A2: 20/30*7.5 = 5 seconds…`,
`A2: rescaled elapsed at new dwell advances immediately`) assert against that copy.
The shipped logic lives in `main.c:388-396` inside `applyControl`'s `PANEL_CTLACT_SCREENWT`
arm, which no test binary compiles. Reverting `main.c` to reset semantics
(`gScreenT = 0.0f;`) leaves the suite fully green.

To be clear on what I *did* verify by inspection: the shipped logic is **correct** —
`oldDwell` is sampled before `panelScreenCfgSet`, `newDwell` after, the formula matches D5
exactly, the rescale is correctly gated on `index == active`, and there is **no
double-advance or skip-two bug** (mandate item 7): on immediate advance `gScreenT` is
reset to 0 and the main-loop check at `main.c:598` then sees `0 + dt < dwell`, and
`panelScreenCfgNext` is used for the advance so a disabled next screen is skipped rather
than double-stepped. The finding is the *test gap*, not the arithmetic.

**Minimal fix** — extract the rescale into `panelScreenCfg.c` (e.g.
`float panelScreenCfgRescale(float elapsed, float oldDwell, float newDwell, int *advance)`),
call it from `main.c`, and point the existing two test cases at the real function instead of
the local copy. Same for the rotation-advance decision if it can be lifted cheaply.

### M6 — the A-4 mapper (`panelGearState`) has no regression test anywhere
`dashboard/api/routes/panel.php:159-166`, called at `:683`

The A-4 mapping is correct and **is** wired in — `:683` calls `panelGearState(...)`, not the
raw cast, and the null-guard expression matches the packet. RETURN-AW1's FIX ROUND
verification (four stage rows + a full IF-MIB sweep) is real work and the results are right.
But it was ad-hoc `php -r`; nothing was committed. Mutation (d) — adding a `2 => 2` arm —
is caught by nothing in the repo.

A-4 closes with "cross-lab review mutation-checks the mapper", and the mapper is the exact
place a well-meaning future edit ("IF-MIB 2 means down… but the device is *present*, so
degraded?") re-introduces the original bug that made state 0 unreachable and defeated A-3.

**Missing test, exact shape** — I did **not** add this; it is a required deliverable.
New file `tests/dashboard/test_panel_gear.php`, patterned on `tests/dashboard/test_solari_ctl.php`:

```php
<?php
// A-4 (NORMATIVE): IF-MIB operStatus -> §3.1 wire gear state. FAIL DARK.
// panel.php returns a closure and registers nothing until invoked with a
// Router, so requiring it only defines the helpers.
require __DIR__ . '/../../dashboard/api/routes/panel.php';

$cases = [
    [1,    1, 'up'],
    [3,    2, 'testing -> degraded'],
    [5,    2, 'dormant -> degraded'],
    [2,    0, 'down'],
    [7,    0, 'lowerLayerDown -> down'],
    [4,    0, 'unknown -> down (fail dark)'],
    [6,    0, 'notPresent -> down (fail dark)'],
    [null, 0, 'NULL -> down (fail dark)'],
    [99,   0, 'unrecognised -> down (fail dark)'],
];
$fail = 0;
foreach ($cases as [$in, $want, $label]) {
    $got = panelGearState($in);
    if ($got !== $want) { printf("FAIL: %s: got %d want %d\n", $label, $got, $want); $fail++; }
    else { printf("ok:   A-4 operStatus=%s -> %d (%s)\n", var_export($in, true), $got, $label); }
}
// All three wire states must be REACHABLE — the original bug made 0 unreachable.
$reach = array_unique(array_map('panelGearState', [1, 3, 2]));
sort($reach);
if ($reach !== [0, 1, 2]) { print("FAIL: not all three wire states reachable\n"); $fail++; }
else { print("ok:   A-4 all three wire states reachable\n"); }

// Role mapping (CONTRACT §3.1) and the 'other' drop.
foreach ([['gateway',0],['router',0],['switch',1],['hub',2],['ap',3],['wanBackup',4]] as [$k,$w]) {
    if (panelGearRole($k) !== $w) { printf("FAIL: role %s\n", $k); $fail++; }
}
if (panelGearRole('other') !== null) { print("FAIL: 'other' must drop\n"); $fail++; }

// Log scale: 0 ONLY for idle, saturates at 7.
if (panelGearLogScale(0) !== 0 || panelGearLogScale(1) < 1 || panelGearLogScale(999999) !== 7) {
    print("FAIL: log scale bounds\n"); $fail++;
}
exit($fail === 0 ? 0 : 1);
```
Register it in `tests/CMakeLists.txt` alongside the other dashboard tests.

---

## 4. SHOULD

### S1 — the daemon decodes CONFIG by hand instead of through the shared codec
`status-panel/daemon/solariPanel.c:111` (`handleConfig`)

```c
if(length!=16u){...return;} memcpy(config.screenCfg,payload,12u); config.flags=payload[12];
```

`protocol.h:5-8` is explicit: *"neither side may cast wire bytes to native structs — all
access goes through the encode/decode functions below."* `panelDecodeConfig()` exists, is
declared in the shared header, and is what the firmware and both test suites use. The
daemon reimplements it with hardcoded offsets and **skips the D3 reserved-bit and
reserved-byte validation entirely** — the very checks mutations (e1)/(e2) proved are
load-bearing. `handleConfig` also uses `length != 16u` where the codec uses `>= 16`,
so the two sides disagree about a trailing-byte frame.

**Failure scenario:** a firmware bug or a torn flash record emits `weightCode` 6 or 7
(`panelDecodeConfig` would not reject that either — see S4 — but the reserved-bit check
would catch a set bit 4-7). The daemon forwards it verbatim; `panelConfigInput()` rejects
with 400; the daemon logs `panel config post failed (HTTP=400)` and retries on every
subsequent CONFIG emission with no backoff. A malformed panel becomes a silent POST-error
loop instead of one logged bad frame.

**Fix:** `if (panelDecodeConfig(payload, length, config.screenCfg, &config.flags) != 0)
{ logMessage("malformed panel CONFIG (%lu bytes)", …); return; }`. Also replace the literal
`type==0x85u` at `solariPanel.c:113` with `PANEL_FT_CONFIG` — `protocol.h` now defines it,
so the "AW2 owns protocol.h" comment justifying the literal is stale.

### S2 — a failing flash write retries at tick rate, forever, with no backoff or signal
`status-panel/firmware/panelScreenCfg.c:85-95` + `main.c:604`

`panelScreenCfgSaveDue` returns `false` without touching `cfg->changedMs` or `cfg->dirty`
when `flash->writeSector` fails. The debounce condition `(nowMs - changedMs) >= 2000` is
already satisfied, so the main loop retries the write **every 25 Hz tick** for as long as the
failure persists. `cfg.persisted` stays false, so `cfgPersisted` on the wire stays 0 — which
is *correct* reporting, but nothing logs and nothing backs off.

This is reachable today: `flash_safe_execute` returns `PICO_ERROR_NOT_PERMITTED` if core 1 is
ever launched without `flash_safe_execute_core_init()` on it, and `main.c:559`'s comment
("revisit this if a core-1 task is added") is precisely the trap that gets walked into. It
does not currently burn flash — the erase lives inside the callback, which is not reached on
a lockout failure — but it does burn CPU in the render loop indefinitely.

**Fix:** on failure set `cfg->changedMs = nowMs` (so the retry respects the 2 s debounce) and
emit one `sendLog("cfg persist failed")` on the false→true edge.

### S3 — `flash_safe_execute_core_init()` is called on the wrong core, and the callback is not SRAM-resident
`status-panel/firmware/main.c:557-560`, `panelScreenCfg.c:137-142`

Two D4 discipline gaps. Neither breaks the current single-core build; both are contractual
and both are the kind of thing that bites later.

1. `flash_safe_execute_core_init()` initialises the calling core as a *lockout victim* — it
   is meant to be called on the core that is **not** driving the write. `main.c` calls it on
   core 0 and then calls `flash_safe_execute` from core 0. Harmless today (core 1 is never
   launched, so the SDK's other-core check passes trivially), but it reads as "the D4 lockout
   protocol is satisfied" when it is not: the moment a core-1 task is added, the init is on
   the wrong core and every write fails into S2's retry loop.
2. `panelScreenCfgFlashSafeWrite` (`panelScreenCfg.c:137`) carries a comment asserting
   "This SRAM callback…", but it has no `__not_in_flash_func()` / `__no_inline_not_in_flash_func()`
   attribute — it is a normal flash-resident function. It happens to work because the SDK's
   own `flash_range_erase`/`flash_range_program` are themselves SRAM-resident and restore XIP
   before returning, so the flash-resident return path is never executed with XIP down. D4
   asks for an "SRAM-resident callback" explicitly; the comment currently documents an
   intent the code does not implement.

**Fix:** annotate the callback `static void __no_inline_not_in_flash_func(panelScreenCfgFlashSafeWrite)(void *param)`,
and either drop the core-0 init with an honest comment ("core 1 is never launched; if one is
added, call `flash_safe_execute_core_init()` **from core 1**") or gate it behind an actual
core-1 launch.

### S4 — reserved `weightCode` values 6/7 pass the shared decoder but are rejected by the server
`status-panel/protocol.c:127`, `dashboard/api/routes/panel.php:274-278`

`panelDecodeConfig` validates screenCfg bits 4-7 and reserved bytes 13-15, but not that
`(byte >> 1) & 7 <= 5`. D3 lists "weightCode 6-7" among the reserved values.
`panelScreenCfg.c:58` and `:77` *do* enforce `<= 5` on both load and set, so the firmware
will not originate one — but the decoder is the shared contract surface and the PHP side
(`panelConfigInput`, `weightCode > 5` → 400) disagrees with it. Cheap to align; add
`|| (((payload[i] >> 1) & 0x07u) > 5u)` to the existing loop and one test case.

### S5 — `unifipolld` has uncaught exception classes that defeat fail-soft, plus a start-limit interaction
`deploy/unifi/unifipolld.py:242-246`, `:196`, `deploy/unifi/unifipolld.service:5,14`

`poll_cycle` catches `HTTPError, URLError, OSError, ValueError, RuntimeError, MySQLError`.
Not caught: `KeyError` from `os.environ["SOLARI_DB_PASS"]` at `:196`, and `TypeError` /
`AttributeError` from an unexpected Integration-API shape (`pick()` returns whatever the API
gave, and `int(rx)` at `:186` will raise `TypeError` on a dict or list). Any of these escapes
the loop and exits the process.

Combined with `Restart=on-failure` + `StartLimitBurst=5` / `StartLimitIntervalSec=60`, five
such exits in a minute puts the unit in `failed` and it **stops permanently** — with the
freshness filter that means A1 silently degrades to no-data and stays there until someone
notices. A8 asks for "no crash, next cycle recovers".

**Fix:** broaden to `except Exception:` in `poll_cycle` (it is the fail-soft boundary; the
narrow list buys nothing there), and read the DB password with `os.environ.get()` + an
explicit `RuntimeError` so it is caught and logged as a skipped cycle.

### S6 — per-device statistics fetches can make a cycle outrun its own freshness window
`deploy/unifi/unifipolld.py:81-91`, `:171-174`, `:272`

`normalise_devices` calls `stats_fetcher` once per device — 9 additional serial HTTPS
requests per cycle, each with `timeout=12`. A gateway that is slow rather than down (the
common UDM failure mode, and exactly what the chemistry UDR7 work has been chasing) makes
one cycle take longer than the 60 s freshness window in `panel.php`, so gear rows expire and
A1 blanks even though the poller is nominally healthy. `time.sleep(15)` is also a fixed
sleep after a variable-length cycle, so the effective cadence drifts.

**Fix:** drop the per-device timeout to ~4 s, and/or track cycle duration and log when it
exceeds 15 s. A deadline for the whole cycle (skip remaining stats fetches, keep the device
rows) preserves E7's "degrade to no-data, no alarm" intent without losing the rows entirely.

### S7 — E3's "logs once" logs once *per request*
`dashboard/api/routes/panel.php:690-693`

`error_log("[panel] gear section truncated from $gearCount to 12 entries")` runs inside
`GET /api/panel`, which the daemon polls on a 2 s cadence. §6 E3 says "logs once". With a
13th device this writes ~43k lines/day to the PHP error log. Gate it on a state row or a
short-lived marker file, or drop it to a payload field the dashboard can surface.

---

## 5. NITs

- **N1** — `db/migrations/019_panel_screen_config.sql:9-10` inserts `'hub','wanBackup'`
  *before* `'other'` rather than appending. No data loss (MariaDB rewrites ENUMs by string
  value and no member is removed — 002's set `('gateway','switch','ap','router','other')` is
  fully preserved), but a non-append-only ENUM change cannot use `ALGORITHM=INSTANT` and
  forces a full table copy. Reordering to `…,'router','other','hub','wanBackup'` keeps it instant.
- **N2** — RETURN-AW2's UNVERIFIED item "§10 D4 linker reservation … no build-time assert …
  the assert is contractual and absent" is **out of date**: `firmware/CMakeLists.txt:148-157`
  plus `firmware/panelFlashLayoutCheck.cmake` implement exactly that guard, and it is added
  *after* `pico_add_extra_outputs`, so the `.bin` exists when it runs. Two caveats:
  (i) `SOLARI_PICO_FLASH_SIZE_BYTES` is hardcoded to `4194304` in CMake while
  `panelScreenCfg.c:139,147` uses the board's real `PICO_FLASH_SIZE_BYTES` — a board swap
  silently desynchronises the guard from the actual erase offset; add a C-side
  `static_assert`/`bi_decl` or pass the value through from the board definition.
  (ii) **I could not measure the actual image size**: there is no build tree or `.uf2`/`.bin`
  in this working copy (`find status-panel/firmware -name "*.uf2" -o -name "*.bin"` → empty);
  the build was done on lithium. RETURN-AW2's "~150 KB vs 4 MB" is plausible for this
  firmware but is **unverified by me**. Severity of the original gap: low — the guard exists,
  the headroom is ~27×, and the guard is a build failure not a runtime one.
- **N3** — `panel.php:143-152` `panelGearLogScale`'s `min(7, $level - 1)` is unreachable
  (seven thresholds cap `$level` at 8). Harmless; I verified the scale itself is sound:
  0 only for `< 1` Kbps, 1 at 1 Kbps, 7 at ≥ 20 Mbps, all eight levels reachable.
- **N4** — `unifipolld.py:74` uses the private `ssl._create_unverified_context()`. The
  rationale in the comment at `:260-266` is correct and well-argued (SolariNet's CA does not
  sign the gateway cert), but the API key then travels over an unauthenticated TLS session
  on the LAN. Prefer pinning the gateway's self-signed cert into `UNIFI_CA_FILE` as the
  documented deployment step rather than the skip-verify default.
- **N5** — `unifipolld.py:270-271` `--once` returns 0 even when `poll_cycle` returned False,
  so a one-shot invocation cannot signal failure to a caller.

---

## 6. Coherence checks (mandate items 4, 5, 7, 8) — results

**Wire coherence, three ways.** CONFIG bit layout is consistent end to end:
firmware packs `bit0 = enabled`, `bits1..3 = weightCode` (`main.c:373-375` for kind 8,
`:381-383` for kind 9) → `panelEncodeConfig` (`protocol.c:113-120`) writes `u8[12] + flags +
3 reserved` → daemon reads `&1` / `(>>1)&7` (`solariPanel.c:59`) → PHP
`panelConfigInput` expects exactly `{enabled: 0|1, weightCode: 0..5} × 12` + `flags`
(`panel.php:268-282`). No mismatch. Only divergence is S4 (weightCode 6/7 admitted by the
codec, rejected by PHP).

**Kinds 8/9 packing.** PHP `panel.php:219-224` validates kind 8 `arg <= 23` and kind 9
`($arg>>3) <= 11 && ($arg&7) <= 5`; firmware `panelCtl.c:130-138` validates `(arg>>1) < 12`
and `(arg>>3) < 12 && (arg&7) <= 5`. Bit-for-bit equivalent, D2 `screenIdx = theme*3+slot`
honoured on both sides, invalid values consumed-not-applied on the firmware side with test
coverage (`case 15` ×4). **ACCEPT.**

**panel.php gear assembly (item 5).** A-4 mapper called, not the raw cast (`:683`) ✓.
Freshness `i.sampledAt > DATE_SUB(NOW(6), INTERVAL 60 SECOND)` ✓. Role-priority ordering via
`usort` on `[role, gearId]` then `array_slice(…, 0, 12)`, with `gearCount => min($n, 12)` ✓
(E3). Log scale sane, 0 only for idle ✓ (N3). `'other'` rows dropped rather than
mis-rendered ✓. **The assembly is correct**; its problems are the missing test (M6) and the
log spam (S7).

**E4/D5 boundary (item 7).** No double-advance, no skip-two. Detail in M5.

**A1 renderer vs §9 A-1 constants (item 8).** I checked every constant against the amendment:

| A-1 requirement | Code | Result |
|---|---|---|
| ap band x 2..16, h=3 | `a1BandX(2,16,n,i)`, `height = 3` (`:170-174`) | ✓ n=3 → x 2/9/16 |
| ap `y0 = round(i*(11-3)/(n-1))`, 4 when n==1 | `(i*(PANEL_H-3) + (n-1)/2)/(n-1)`, `n==1 ? 4` (`:172`) | ✓ → 0/4/8 |
| hub band 20..34 | `a1BandX(20,34,n,i)` (`:177`) | ✓ n=3 → 20/27/34 |
| hub `h = i even ? 6 : 5`, `y0 = i even ? 0 : 11-h` (stagger) | `:178-179` | ✓ |
| switch band 38..44, h=8, y0=1 | `a1BandX(38,44,…)`, `a1Gate(x,1,8,…)` (`:183-185`) | ✓ n=1 → x 41 |
| router band 46..48, h=11, y0=0 | hardcoded `x=47, y0=0, h=11` (`:145`) | ✓ |
| internet x=51, wanBackup x=52 | `internet = {…,51,0,11}`, `wan = {…,52,3,5}` (`:132`) | ✓ |
| legs band gate i → next band (i mod nNext) | `:193-197` | ✓ (correctly guarded, unlike the particle path) |
| wire brightness from src **rx**Level | `a1Leg(..., gear[src].rxLevel)`, `b = 0.04 + rx*0.06` (`:120`) | ✓ |
| particle spawn/brightness from src **tx**Level; level 0 = no particles | `speed = v*level*0.18`, `if (level == 0) continue` (`:219-222`) | ✓ |
| gate colour up=accent / degraded=warn / down=crit + every-other-pixel "broken" | `a1GateColor` (`:87-91`), `a1Gate` skip (`:96-101`) | ✓ |
| `gearCount 0` → existing no-data treatment | `:138` `panelScreenNoData(t); return;` | ✓ (probe-confirmed) |

**A-3 internet-column state inheritance (Lead-authored line, `panelScreensA.c:188-190`).**
Reviewed as ordinary code, and it is right: `a1Gate(internet.x, 0, 11, haveRouter ?
env->snap.gear[router.gear].state : 0u, 0.60f)` — the column takes the router's state
treatment in all three states, and fails **dark** (state 0 → crit + broken) when no router is
present, which is the correct reading of "never a healthy full-height marker over a dead
router". wanBackup at `:191` correctly keeps its own device state, so a router outage renders
a broken internet column beside a live backup column — the intended at-a-glance story.
Two gaps, both already counted above: the states are pinned by no firmware test (M4), and the
whole particle layer around it faults on a partial fleet (M3).

**Poller role mapping vs §2 (item 6).** `model_kind` (`unifipolld.py:26-40`) orders
`u5g backup` → `udm*` → `usw pro` → `usw ultra|flex|lite` → `u6|u7|u5g` → `other`, matching
§2 including the backup-before-AP precedence. RETURN-AW1's live dry-run maps all 9 real
devices correctly. `operStatus` is `1` / `2` (IF-MIB up/down), which composes correctly with
A-4. **Key never logged, confirmed:** the key appears only at `:70-77` (constructor and the
`X-API-KEY` header) and `:256-258` (presence check, name only); `poll_cycle` logs the fixed
string `"poll failed; skipping cycle"` with no exception detail, so a request URL or header
can never reach the journal.

**systemd unit (item 6).** `After=network-online.target mariadb.service` + `Wants=` ✓,
`Restart=on-failure` + `RestartSec=5` ✓, dedicated `.venv` interpreter ✓ (AW1's fix is
correct and necessary — `pymysql` is absent from the system interpreter and the
`except ImportError` at `:14-17` would have made that a *silent* no-write). Two notes:
env comes from `ExecStart=/bin/bash -lc 'source … && exec …'` rather than
`EnvironmentFile=`, which drags in a login shell's profile and makes the unit's environment
non-obvious to `systemctl show`; and see S5 for the `StartLimitBurst` interaction.

---

## 7. Working-tree restoration

Every mutation was reverted from a pristine copy taken before the first edit
(`scratchpad/protocol.c.orig`). Post-review state:

```
$ git diff --stat
 dashboard/api/routes/panel.php            |  202 ++-
 dashboard/public/screens-panel.jsx        |  602 +++++++-
 dashboard/public/styles.css               |   31 +
 status-panel/daemon/solariPanel.c         |    7 +-
 status-panel/daemon/tests/codec_test.c    |   15 +-
 status-panel/firmware/CMakeLists.txt      |   17 +
 status-panel/firmware/main.c              |   76 +-
 status-panel/firmware/panelCtl.c          |   11 +
 status-panel/firmware/panelCtl.h          |    4 +-
 status-panel/firmware/panelScreensA.c     |  188 ++-
 status-panel/firmware/test/Makefile       |    7 +-
 status-panel/firmware/test/panelCtlTest.c |   21 +
 status-panel/fixtures/panel-snapshot.json | 2164 ++++++++++++++++++++++++++++-
 status-panel/protocol.c                   |   49 +-
 status-panel/protocol.h                   |   24 +-
 15 files changed, 3259 insertions(+), 159 deletions(-)
```

Byte-for-byte identical to the diffstat at session start. `panel.php` was **never modified** —
mutation (d) was established by proving the absence of any test that could observe it, which
avoided touching a live-served file. Probe sources live only in the session scratchpad
(`/tmp/claude-1000/…/scratchpad/`), not in the repo. No git operations, no deploys, no
service restarts, no writes to `solarinet` or `solarinet_stage`, no flashing.

## 8. UNVERIFIED (mine)

1. **Actual UF2/`.bin` size vs the reserved sector** — no build artifacts exist in this
   working copy; the target build was on lithium. RETURN-AW2's ~150 KB figure is unchecked
   by me (see N2).
2. **Real flash behaviour on hardware** — everything about `flash_safe_execute`, XIP
   suspension and core lockout in S2/S3 is from code reading against pico-sdk semantics, not
   from a device. No flashing was performed.
3. **Live/stage database behaviour** — the live `solarinet` DB is read-only to me and I made
   no queries against either database; the migration-019 ENUM analysis in N1 is from the SQL
   text and `002_c2_capabilities.sql:20`, not from a live `DESCRIBE`.
4. **The M1 fix sketch is untested** — I wrote no code into the tree, so the `parseSnapshot`
   patch above is a design, not a verified diff.
5. **`dashboard/public/screens-panel.jsx`** (AW3's lane) was out of scope; virtual/firmware
   A1 parity is therefore asserted only against the A-1 constants table, not against AW3's
   renderer output.
