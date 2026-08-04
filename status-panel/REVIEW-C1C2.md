# Adversarial review — C1 (/api/panel) + C2 (protocol.c, solariPanel daemon)

`2026-08-04 · reviewer: Claude Opus (cross-lab gate; code under review authored by GPT-5.6) · against CONTRACT.md v1.1 §3 §4 §6 §9 + DESIGN-BRIEF.md FINAL DECISIONS`

Three of the findings below are **empirically proven**, not inferred, with
throwaway harnesses in the scratchpad (repo untouched). Every other finding is
from code reading and is marked as such where the distinction matters.

**Headline:** the daemon has demonstrably never been executed. It exits 1 on
every valid config (D1); patching only that, it segfaults on the first login
(D2); patching that, it segfaults again on the first poll that carries a
topAlert (D3) — and there are 95 active warn alerts live right now, so that
path is not hypothetical. "Builds clean on x86-64 and aarch64" was true and
told us nothing.

---

## C1 — `dashboard/api/routes/panel.php`

### P1 · MUST-FIX · `panel.php:231,262` — `episodeId` regresses, re-arming an acknowledged alarm
`$critEpisodeId = max($critEpisodeId, (int) $alert['eventId'])` over all active
crits, used verbatim as `episodeId`. It is a maximum over a *shrinking* set, not
a monotonic counter.
**Failure:** crit events 100 and 105 are active → `episodeId=105`; operator acks;
105 clears while 100 is still active → `episodeId` drops to 100 → firmware sees a
changed episodeId and re-arms the two-tone alarm for a fault that was already
acknowledged and is strictly *less* severe than a minute ago. §9 calls episodeId
"monotonic per alarm episode"; this is not.
**Fix:** persist the episode in a small server-side table (or a `GREATEST()` over
a monotonic column) so an episode id is allocated once and never reused or lowered.

### P2 · MUST-FIX · `panel.php:253` vs `panel.php:265` — the same pool breach yields two different episodeIds
Line 253 computes `crc32` over the breaching pools' **names**; line 265 computes
`crc32` over their **poolIds**. Which branch runs depends only on whether
`$topAlert` happens to be non-null.
**Failure:** a tier-0 pool breaches with no alert rows → `episodeId = crc32(names)`.
An unrelated *warn* alert fires 5 s later → `$topAlert` becomes that warn →
control moves to line 265 → `episodeId = crc32(ids)` → different value → alarm
re-arms with no change in the fault. The warn clearing flips it back.
**Fix:** compute the breach key once, from one field, above the branch, and reuse it.

### P3 · MUST-FIX · `panel.php:193` — `retired` nodes hit an undefined array key
`$poolAgg[$poolId][$system['state']]++` indexes by verbatim `node.state`, but
`$poolAgg` is initialised with only `up/degraded/down/unknown/maint`.
`node.state` is `ENUM('up','degraded','down','unknown','retired')`
(`db/migrations/002_c2_capabilities.sql:79`) — **`retired` is a real value**.
**Failure:** any retired node in a pool emits a PHP 8 `Undefined array key
"retired"` warning (which corrupts the JSON body outright if `display_errors` is
on for this vhost) and its count silently vanishes from the five rendered
states while still inflating `total`, so the panel's pool bars stop summing.
**Fix:** `$state = isset($poolAgg[$poolId][$state]) ? $state : 'unknown';` before the increment.

### P4 · SHOULD · `panel.php:170,292` — `stateRoll` and `pools[]` count different populations
`stateRoll` is grouped over `node`; `pools`/`systems` are built from `asset LEFT
JOIN node`. An asset with `nodeId IS NULL` contributes `unknown` to the pools but
nothing to `stateRoll`; a node with no asset row does the reverse. The panel
renders both on the same screen.
**Failure:** Theme D shows "9 UP" in the roll while the pool bars total 11.
Equivalence with `/api/summary` still passes, because that test only exercises
the `node`-derived half.
**Fix:** derive `stateRoll` by summing the pool aggregates, or state in the contract that the two are deliberately different populations.

### P5 · SHOULD · `panel.php:117,173` — `asset.nodeId` is not UNIQUE, so telemetry double-counts
`db/migrations/004_assets_pools.sql`: `nodeId BIGINT UNSIGNED` — nullable, no
unique constraint. Two assets pointing at one node each join the same
`hostCurrent` row.
**Failure:** fleet `rxKbps`/`txKbps` double for that host and the node is counted
twice in its pool's state counts (while `stateRoll` counts it once — compounding P4).
**Fix:** `GROUP BY a.nodeId` for the telemetry sum, or add the unique constraint.

### P6 · SHOULD · `panel.php:133-143` — unbounded alert scan
The query selects *every* uncleared `alertEvent` with no `LIMIT`, then uses
exactly one row (the first) plus three counters.
**Failure:** 95 warns today; a flapping rule over a weekend puts thousands of
uncleared rows through PHP object hydration every 5 s. This is the one query
here that grows without bound.
**Fix:** split into a `GROUP BY severity` count query plus a `LIMIT 1` top-alert query.

### P7 · SHOULD · `panel.php:91-100` — tier 2 is unreachable, so its threshold rule is dead code
`panelTier()` returns only 0, 1 or 3. CONTRACT §3 asks for `server/monitor → 0/1,
clients → 2/3`, and DESIGN-BRIEF's tier table gives tier 2 a distinct meaning
("HA and DR standby").
**Failure:** DR standby hosts are classified tier 3 alongside workstations; nothing
distinguishes them, and no pool ever carries tier 2.
**Fix:** map a `class`/`role` value (e.g. `class === 'standby'`) to tier 2, or delete tier 2 from the contract.

### P8 · SHOULD · `panel.php:210` — the `>= 5` gate makes small tier-2/3 pools un-alarmable
`($downFraction >= 0.2 && $pool['down'] >= 5)`. Faithful to CONTRACT §3, but the
DESIGN-BRIEF's verbatim rule is 20% only ("sixty workstations out of nine
hundred do not").
**Failure:** a 4-node tier-2 HA/DR pool going 100% dark scores 85 and never alarms.
**Fix (spec, not code):** make the ≥5 floor apply only to tier 3, or drop it — Lead's call, this is a contract defect, not an authoring error.

### P9 · SHOULD · `panel.php:311` — `dataStale` is hardcoded 0 and can never become 1
§9 assigns staleness derivation to the endpoint, but `ts` is `NOW()` at
composition, so a server-side comparison against `ts` is tautological.
**Failure:** the field is dead on the wire. In practice the *daemon* covers the
real case (API unreachable → its cached `ts` ages), which is arguably the correct
place — but then §9 is wrong and the field should carry monitoring-data staleness
(`MAX(hostCurrent.updatedAt)`), which is the thing an operator actually needs.
**Fix:** derive from the newest `hostCurrent`/`probeCurrent` sample age, or delete the field and amend §9.

### P10 · SHOULD · `panel.php:292` — `maint` is always 0
`node.state` has no `maint` member. `stateRoll.maint` and every `pools[].maint`
are structurally always zero, so the firmware's maint colour path is unreachable.
**Fix:** source maint from the maintenance-window table, or record in the contract that maint is reserved-unused for v1.

### P11 · NIT · `panel.php:306` — `meanLoadPct` divides by every asset
Assets with no linked node contribute `loadPct = 0` to the mean, deflating it
proportionally to how much of the inventory is unmonitored.

### P12 · NIT · `panel.php:311` — `$episodeId ?? 0` is dead
Every branch of the 246-268 chain assigns `$episodeId`. The coalesce reads as if
a path were missed; it hides a real omission if one is later introduced.

### Checked, clean (C1)
- **SQL injection:** no string interpolation into any of the six statements — all literals. Clean.
- **Auth gating:** `index.php:47` applies `Auth::requireSession()` to everything outside `/api/auth/*`; the route needs no local gate and matches every peer route's pattern. Clean.
- **`Cache-Control: no-store` + `Content-Type: application/json`:** both set unconditionally in `Response::send()` (`lib/Response.php:63-66`). Clean.
- **Single consistent read:** `START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY` with commit/rollback around all six selects; `ts` sampled inside. Clean.
- **Name charset/widths vs protocol widths:** `panelText()` emits `[A-Z0-9 .:-]` and truncates to 8/12/24/48 with a trailing `.`, matching `PANEL_NAME_POOL`/`_SYS`/`ALERT_SUBJ`/`_DETAIL` exactly. It also composes at width-1+`.` so `protocol.c:writeName` sees `strlen == width` and does *not* append a second dot. Clean.
- **JSON field names vs §3/§9:** `ts score stateRoll alerts meanLoadPct rxKbps txKbps rttTenthMs lossPermille pools systems topAlert alarmActive episodeId dataStale`, and per-pool `total` — all present and correctly named. Clean. (The daemon reads them from the wrong nesting level — see D3.)
- **Empty fleet:** `$systemsAll === []` guarded at 306; `$poolAgg` empty → `$score = 0`, empty arrays, `topAlert = null`. Clean.
- **Per-request cost:** aggregates only, no per-node window query, six statements flat. p95 15.7 ms is consistent. Clean (except P6's unbounded row count).
- **Score arithmetic vs DESIGN-BRIEF:** tier 0/1 alarm on any `down`; tier 2/3 on ≥20%; worst-pool `max()`, not a sum; the 100 threshold is the inlay trigger. Faithful. (P8 is a contract-level quarrel, not an arithmetic error.)

---

## C2a — `status-panel/protocol.c`

### C1 · MUST-FIX · `protocol.c:81,90` — the parser discards every buffered frame after a resync — **proven**
After `parserResync()` repositions the buffer, the loop `continue`s to the next
*input* byte. It never re-evaluates the header/CRC of what it just recovered, and
when the frame is eventually dispatched, `p->have = 0` throws away every byte
buffered beyond it. So one resync can swallow an arbitrary number of complete,
CRC-valid frames sitting in the buffer.

Harness result (noise header claiming a 40-byte payload, followed by four valid
frames, one `panelParserFeed` call):

```
noise-header-then-4-good-frames: dispatched=1 (want 4) crcErr=1 resyncs=1
  after next frame 5 s later:    dispatched=2 (want 5)
```

**Failure:** one line-noise burst on the CDC link makes the firmware miss up to
~2 KB of stream. A full snapshot is 1316 bytes (8 pools + 64 systems + alert), so
that is up to two snapshots — ~4 s — plus the deferred dispatch of the survivor,
which pushes the panel toward the 15 s LINK LOST threshold on the very
event resync exists to recover from.
**Fix:** make the post-header/CRC section a `while (p->have >= PANEL_HDR_SIZE)` loop, and on dispatch `memmove` the `p->have - total` remainder to the front instead of zeroing `have`.

### C2 · SHOULD · `protocol.c:57,64` vs `:72` — encoder and decoder disagree on `hasTopAlert`
`panelEncodeSnapshot` treats `hasTopAlert` as truthy (`s->hasTopAlert ? ... : 0`)
and writes the raw byte at `p[21]`; `panelDecodeSnapshot` rejects `alert > 1u`
outright.
**Failure:** a caller setting `hasTopAlert = 2` produces a frame with a valid CRC
that the peer rejects as malformed — a silent, permanent snapshot outage with no
CRC error to point at it. The daemon assigns `1u` today, so this is latent, not live.
**Fix:** normalise in the encoder — `p[21] = s->hasTopAlert ? 1u : 0u;`.

### C3 · NIT · `protocol.c:46` — `total` is computed before the bound check
`size_t total = PANEL_HDR_SIZE + payloadLen + PANEL_CRC_SIZE;` runs at
declaration, before the `payloadLen > PANEL_MAX_PAYLOAD` guard on the next line.
Unsigned wrap is defined and the `||` short-circuits correctly, so this is safe
today — but it reads as an overflow and will be "fixed" wrongly by someone later.
**Fix:** move the guard above the computation.

### Spec defects in `protocol.h` (Lead-authored, flagged separately as instructed)

**S1 · SHOULD · `protocol.h:33` + CONTRACT §9 — the seq guard is mandated but not provided.**
The header's parser rules require receivers to ignore duplicate and
RFC1982-older `seq` values, and §9 lists a "seq-based duplicate/ordering guard"
as part of the shared codec — but the header declares no comparison helper and
`protocol.c` implements none. Both sides will hand-roll `(int16_t)(a - b) > 0`
independently, which is exactly the kind of thing one side gets wrong.
**Fix:** add `int panelSeqNewer(uint16_t candidate, uint16_t lastApplied);` to the header and implement it once in `protocol.c`.

**S2 · NIT · `protocol.h:96` vs CONTRACT §3:60** — the header documents `score` as
`0..1000`, §3's JSON sample comments it `0-100`, and the endpoint actually emits
0..140. All three should say the same thing; 0..140 is the truth.

### Checked, clean (protocol.c)
- **CRC range:** `panelCrc16(out + 2, 4u + payloadLen)` on encode and `panelCrc16(p->buf + 2, 4u + plen)` on decode — version..payload, magic excluded, exactly as `protocol.h:22`. Clean.
- **Byte offsets / LE:** every globals, pool, system and alert offset cross-checked field-by-field against the header's layout block; strides 20/16 and `PANEL_ALERT_SIZE 84` all correct; encode/decode are exact inverses. Clean.
- **Overlapping magic (`A5 A5 53`):** `protocol.c:90`'s `have==1` branch holds position on a second `0xA5` instead of resetting. Harness-verified: frames=1 (want 1). Clean.
- **`payloadLen > MAX` rejected before buffering:** checked at `have == PANEL_HDR_SIZE`, before any further accumulation; `buf` is `HDR+MAX+CRC = 2056` and `total` maxes at exactly 2056. No overrun on any path. Clean.
- **Frame-timeout wraparound:** `(uint32_t)(nowMs - p->lastByteMs) > PANEL_FRAME_TIMEOUT_MS` — unsigned, wrap-correct. Clean.
- **memcpy bounds:** every `memcpy`/`memmove` in encode, decode and resync is bounded by a length already validated against `cap`/`len`/`have`. Clean.
- **Counts over maxima:** encoder returns 0, decoder returns -1. Clean.
- **Name sanitisation:** uppercase, `[A-Z0-9 .:-]` filter to `.`, NUL-pad, truncate with trailing `.` only when the source genuinely overruns (`in[width] != '\0'`). Reads `in[width]` only when `strlen >= width`, so no over-read. Clean.
- **Unknown type / version:** frame is consumed and skipped, never desyncs. Clean.

---

## C2b — `status-panel/daemon/solariPanel.c`

### D1 · MUST-FIX · `solariPanel.c:37` — `readPassword` always fails; the daemon cannot start — **proven**
`snprintf(out, cap, "%s", trim(out))` — source and destination alias. This is
undefined behaviour, and glibc's realisation of it is an **empty string**, so the
final `return out[0]=='\0' ? -1 : 0` always returns -1.

```
$ ./rp /path/to/pw          # extracted verbatim from solariPanel.c
rc=-1 pw=[]
$ ./solariPanel --config valid.conf
2026-08-04T06:28:19Z solariPanel: configuration or password file unavailable   # exit 1
```

**Failure:** every start, on every host, with a perfectly valid 0600 password
file, dies with a message that blames the config. Under `Restart=always` +
`RestartSec=3` that is a permanent 3-second crash loop that `StartLimitIntervalSec=0`
guarantees systemd will never stop retrying.
**Fix:** `memmove(out, trim(out), strlen(trim(out)) + 1);` — or trim in place without a copy.

### D2 · MUST-FIX · `solariPanel.c:47` — `curl_easy_init` is missing its `()` — **proven**
`CURL *curl = curl_easy_init;` assigns the *address of the function*. `CURL` is
`typedef void`, so GCC accepts a function-pointer→`void*` conversion without a
diagnostic even under `-Wall -Wextra -Werror` (only `-Wpedantic` catches it) —
which is precisely why acceptance criterion §7.4 passed while the program was
non-functional. `curl` is then never NULL, so the guard on the next line is dead,
and the first `curl_easy_setopt` runs against a code address.

With D1 patched in a scratch copy:
```
$ ./solariPanel --config valid.conf ; echo $?
139        # SIGSEGV, immediately, inside login()
```
**Fix:** `curl_easy_init()`.

### D3 · MUST-FIX · `solariPanel.c:53` — NULL dereference on `topAlert.episodeId`
`(uint32_t)cJSON_GetObjectItemCaseSensitive(top,"episodeId")->valuedouble` —
dereferenced with no NULL check. The endpoint emits `episodeId` at the **top
level of `data`**, not inside `topAlert` (`panel.php:311`). The pointer is
therefore always NULL whenever `topAlert` is an object.
**Failure:** segfault on the first poll that returns any crit *or warn* alert.
There are 95 active warns right now, so this fires on poll #1, every time. `id`
is dereferenced the same way and is only safe by luck.
**Fix:** read `episodeId` from `data`, and route both through a checked accessor (`cJSON_IsNumber(x) ? x->valuedouble : 0`).

### D4 · MUST-FIX · `solariPanel.c:53` — alert severity decoded with the *state* mapper
`snapshot->topAlert.severity = stateValue(...)`. `stateValue()` maps
`up/ok/degraded/down/maint`; it has no `crit`/`warn`/`info` case, so it returns
`PANEL_ST_UNKNOWN` (3) — outside `PanelSeverity`'s 0..2 entirely.
**Failure:** every alert reaches the firmware with severity 3. Any severity-driven
colour or rail treatment is fed a value the enum does not define.
**Fix:** add `severityValue()` mapping `info/warn/crit → 0/1/2`.

### D5 · MUST-FIX · `solariPanel.c:53` — throughput and probe metrics clamped to 255
`rxKbps`, `txKbps` (u32 on the wire), `rttTenthMs` and `lossPermille` (u16) are
all read through `byteValue()`, which clamps to `0..255`.
**Failure:** §3's own worked example — `throughputKbps: 340` — arrives as 255.
Every throughput ribbon and RTT sparkline in the design saturates permanently
and the panel's headline figures are wrong by orders of magnitude.
**Fix:** add `u32Value()`/`u16Value()` helpers with the correct clamps and use them for these four fields.

### D6 · MUST-FIX · `solariPanel.c:53` — a malformed response destroys the last-good state
`memset(snapshot, 0, sizeof(*snapshot))` runs at the *top* of `parseSnapshot`,
before the `ts`/`score`/`pools`/`systems` type validation and before the
per-element `cJSON_IsObject(item)` checks — and `snapshot` **is** `latest` in
`runDaemon`.
**Failure:** the API returns `{"ok":true,"data":{...}}` with one malformed pool
object. `parseSnapshot` returns -1 so the poll counts as failed, but `latest` is
already zeroed while `haveLatest` stays 1 → the daemon proceeds to ship an
all-zero snapshot: score 0, empty fleet, no alarm. This directly violates §9's
"last good state retained and never overwritten by partial data" — the
requirement the memset defeats.
**Fix:** parse into a local `PanelSnapshot` and `*snapshot = staged;` only on success.

### D7 · MUST-FIX · `solariPanel.c:53` — `alarmActive` is recomputed locally, discarding the server's
`snapshot->alarmActive = snapshot->alertCounts[2] != 0u;`. The endpoint computes
`alarmActive = critEpisodeId > 0 || score >= 100` and sends it; the daemon
ignores that field and substitutes "is the crit count nonzero".
**Failure:** the entire tier-threshold half of the alarm rule is silently
dropped. A tier-0 pool with a node down and no crit *alertEvent* row scores 140,
the endpoint says `alarmActive: true` and synthesises a POOL BREACH topAlert — and
the panel stays quiet. That is the headline behaviour of the whole design ("one
DNS server interrupts at any fleet size"), disabled.
**Fix:** `snapshot->alarmActive = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data,"alarmActive"));`

### D8 · MUST-FIX · `solariPanel.c:68` — deadlines initialised to 0 stall the daemon for 25 days
`nextPoll = nextFrame = nextSerial = 0u`, compared with the wrap-safe idiom
`(int32_t)(now - nextPoll) >= 0`. `nowMs()` is `CLOCK_MONOTONIC`, i.e. milliseconds
since boot.
**Failure:** start the daemon on a host whose uptime is between 24.9 and 49.7
days (`now` ∈ [2³¹, 2³²)) and `(int32_t)(now - 0)` is **negative** — so the first
poll, the first frame and the first serial open are all deferred until the
monotonic clock wraps. The daemon runs, logs nothing, opens nothing, and the
panel sits dark for up to 25 days. This is exactly the `systemctl restart` case
after a long uptime.
**Fix:** initialise all three to `nowMs()`.

### D9 · SHOULD · `solariPanel.c:70` — no `SIGPIPE` disposition, no `CURLOPT_NOSIGNAL`
Neither `signal(SIGPIPE, SIG_IGN)` nor `CURLOPT_NOSIGNAL, 1L` is set.
**Failure:** a TLS write onto a socket the dashboard has already closed delivers
SIGPIPE to a process with the default disposition — the daemon dies. systemd
restarts it, so this presents as unexplained restarts in the journal rather than
an outage, which makes it worse to diagnose, not better.
**Fix:** add both.

### D10 · SHOULD · `solariPanel.c:47` — `curl_slist` leaked on every login
`curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_slist_append(NULL, ...))` — the
list is never captured and never `curl_slist_free_all`'d, and `curl_easy_cleanup`
does not free it.
**Failure:** small, but it leaks per login attempt, and D-series auth backoff can
retry every second for a long time. Unbounded over a bad-credentials weekend.
**Fix:** hold the list in a static/struct and free it alongside the handle.

### D11 · SHOULD · `solariPanel.c:47` — the password is interpolated into JSON unescaped
`snprintf(body, sizeof(body), "{\"user\":\"%s\",\"password\":\"%s\"}", user, password)`.
**Failure:** a password containing `"` or `\` — both entirely legal, and likely
from a generator — produces malformed JSON. The login 400s and the daemon reports
"login failed (HTTP 400)" forever with no hint that the password is the problem.
**Fix:** escape both fields, or build the body with cJSON, which is already linked.

### D12 · SHOULD · `solariPanel.c:37` — `passFile` mode is never checked
§6 requires a 0600 file; nothing verifies it.
**Failure:** a world-readable `/etc/solari-panel/password` is used silently. The
one check that would catch the most likely deployment mistake is missing.
**Fix:** `fstat` the fd and refuse if `st_mode & (S_IRWXG|S_IRWXO)`.

### D13 · SHOULD · `solariPanel.c:56` — glob takes only the first match, with no fallback
`snprintf(selected, ..., matches.gl_pathv[0])` — §9 says "tried in order".
**Failure:** with a second USB CDC device enumerating ahead of the Unicorn
alphabetically in `/dev/serial/by-id/`, the daemon opens the wrong device, or
opens a stale entry, and never tries the next candidate.
**Fix:** loop over `gl_pathv[0..gl_pathc)` until one opens and passes `tcgetattr`.

### D14 · SHOULD · `solariPanel.c:68` — serial reconnect is a flat 1 s, not 1→30 s backoff
`nextSerial = now + 1000u`, unconditionally. CONTRACT §4 specifies "backoff
(1 s → max 30 s)".
**Failure:** with the panel unplugged, the daemon calls `glob` + `open` once a
second indefinitely and logs on every write failure — journal noise that buries
the real events during exactly the incident someone is reading the journal for.
**Fix:** mirror the `authDelay`/`transientDelay` doubling already used for the API.

### D15 · SHOULD · `solariPanel.c:68` — `dataStale` latches on if the server clock leads
`latest.dataStale = (uint8_t)((uint32_t)(wall - latest.ts) > 35u)` — unsigned
subtraction. §9 explicitly requires tolerating ±5 s of skew; this tolerates skew
in one direction only.
**Failure:** xenon's clock is 1 s ahead of lithium's → `wall - latest.ts`
underflows to ~4.29 billion → `dataStale = 1` on every frame, forever. The panel
permanently displays stale data while the feed is perfectly healthy.
**Fix:** compute in `int64_t` and compare `> 35`, so negative skew reads as fresh.

### D16 · NIT · `solariPanel.c:47` — password copy is never scrubbed, and `CURLOPT_POSTFIELDS` dangles
`body[1024]` holds the plaintext password and goes out of scope un-zeroed (`main`
scrubs only its own `password[512]`), leaving it in a core dump. Separately,
`CURLOPT_POSTFIELDS` does not copy — the handle retains a pointer to that dead
stack frame. Harmless today because `request()` sets `CURLOPT_POST, 0L` for the
GET, but it is one refactor away from sending stack garbage to the API.
**Fix:** `memset` the body after login; use `CURLOPT_COPYPOSTFIELDS`.

### D17 · NIT · `solariPanel.c:68` — 20 ms busy-poll
`sleepMs(20L)` at the bottom of the loop wakes the process 50×/s to service work
that arrives every 2 s. On battery-adjacent or thermally-tight hardware this is
the daemon's entire power profile.
**Fix:** `poll()` on the serial fd with a timeout computed from the next deadline.

### D18 · NIT · `daemon/tests/codec_test.c` — 18 lines total
The codec test is a single round-trip. It exercises no parser path at all, which
is why C1 — a total frame-loss bug in the one component both sides share — shipped
as "tests pass". The parser harness in this review took ten minutes to write.
**Fix:** port the three scratchpad cases (overlapping magic, CRC-error recovery, noise-header resync) into `codec_test.c`.

### Checked, clean (daemon)
- **401 handling:** exactly one re-login retry inline, then the auth path backs off on `authDelay` (1→60 s) while transient fetch failures back off separately on `transientDelay`. Split is correct per §9, including `curl_easy_cleanup(NULL)` being a safe no-op on the failed-relogin path. Clean.
- **HTML-login-page guard:** requires 2xx **and** `Content-Type: application/json` **and** top-level `ok: true` **and** `data` being an object. An HTML login page cannot parse as data. Clean.
- **TLS:** `SSL_VERIFYPEER 1`, `SSL_VERIFYHOST 2`, `CAINFO /etc/solari-panel/ca.pem`, `FOLLOWLOCATION 0`, `CONNECTTIMEOUT 5`, `TIMEOUT 10` — all set in `setCurlCommon`, which every request routes through. Matches §9 exactly. Clean.
- **Cookie engine:** `CURLOPT_COOKIEFILE, ""` — in-memory, no disk. Clean.
- **`writeAll`:** EINTR retries, EAGAIN sleeps 10 ms against a wrap-safe 2 s deadline (`(int32_t)(nowMs() - deadline) >= 0`), partial writes accumulate correctly. Clean.
- **Reopen path:** `panelParserInit(&parser)` + `nextFrame = now` on successful open — parser state reset and an immediate fresh snapshot, per §9. Clean.
- **cJSON lifetime:** every `parseSnapshot` return path after the parse calls `cJSON_Delete(root)`; the early-return on parse failure has nothing to free. No leak. Clean.
- **curl handle lifetime:** cleaned up on login failure, on 401 re-login, and at loop exit. No leak (the slist is the exception — D10). Clean.
- **Body bound:** `bodyWrite` returns 0 past `BODY_MAX`, aborting the transfer rather than overflowing. Fails closed. Clean.
- **Signal handling:** SIGINT/SIGTERM set `running = 0`; the 20 ms loop tick bounds shutdown latency well inside systemd's default `TimeoutStopSec`. `close(fd)` and `curl_easy_cleanup` run on the way out. Clean (but see D9 for SIGPIPE).
- **Config parsing:** unknown keys ignored, comments and blank lines skipped, all four `snprintf` bounds correct, required-key and positive-interval validation present with a clear message. Clean.

---

## C2c — systemd unit, Makefile, conf.example

### U1 · SHOULD · `solari-panel.service` — no hardening directives at all
The unit has none of `NoNewPrivileges`, `ProtectSystem`, `ProtectHome`,
`PrivateTmp`, `ProtectKernelTunables`, `RestrictAddressFamilies`. It runs as
`jason` — a real interactive user with a home directory, an SSH key and sudo —
rather than a dedicated system account, so an exploited daemon inherits all of it.
**Fix:** add `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, and `ReadOnlyPaths=/etc/solari-panel`; move to a `solari-panel` system user if the deadline allows.

### U2 · NIT — config path diverges from CONTRACT §6 (spec defect)
§6 says `/etc/solari-panel.conf`; the unit, `DEFAULT_CONFIG` and `conf.example`
all say `/etc/solari-panel/solari-panel.conf`. The directory form is the better
choice (it is where §9 already puts `ca.pem` and the password file) — but §6
should be amended rather than left contradicting three files.

### U3 · NIT · `solari-panel.conf.example:1` — documents 0644 for the conf, says nothing about the password file
The one file whose mode actually matters (§6: 0600) gets no comment. Compounds D12.

### U4 · NIT · `Makefile:2` — `CFLAGS ?=` lets an override silently drop `-Werror`
Acceptance criterion §7.4 is "builds `-std=c99 -Wall -Wextra -Werror` clean"; a
packager setting `CFLAGS=-O2` in the environment removes every flag that criterion
names. **Fix:** keep the warning set in a non-overridable `CFLAGS +=` line.

### Checked, clean (unit/Makefile)
- **§9 unit requirements:** `Wants=network-online.target` + `After=network-online.target`, `Restart=always`, `RestartSec=3`, `StartLimitIntervalSec=0` (correctly placed in `[Unit]`), `User=jason`, `SupplementaryGroups=dialout`, `WantedBy=multi-user.target`. Every one present and correct. Clean.
- **Paths:** unit `ExecStart`, `DEFAULT_CONFIG`, `CA_FILE` and `conf.example` all agree on `/etc/solari-panel/`. Internally consistent. Clean.
- **Makefile:** no cmake, no bundler, vendored cJSON and `../protocol.c` compiled in directly, `test` target wired to the codec test, correct prerequisites, `clean` complete. Clean.

---

## Verdicts

| Component | Verdict | Rationale |
|---|---|---|
| **C1 · `dashboard/api/routes/panel.php`** | **FIX-THEN-SHIP** | Structurally sound — clean SQL, correct auth inheritance, correct headers, one consistent read, faithful score arithmetic. But `episodeId` is not fit for its stated purpose as the re-arm key (P1, P2), and P3 breaks pool counts on data that exists in the schema today. Three focused fixes. |
| **C2a · `status-panel/protocol.c`** | **FIX-THEN-SHIP** | Byte-level codec is exactly right — offsets, endianness, CRC range, bounds, sanitisation all verified field-by-field. One real defect (C1), in the parser, proven to drop 3 of 4 valid frames. Contained fix, then it is genuinely solid. |
| **C2b · `status-panel/daemon/solariPanel.c`** | **REWORK** | Eight MUST-FIX defects, three of them proven to be immediate hard failures on the first execution — the program has never been run. D6 and D7 are not typos but misreadings of the contract's two most important behavioural requirements (last-good-state retention; the tier-threshold alarm rule). The volume and the character of the errors mean a fix-list patch will not produce trustworthy code; the poll/parse/serve loop wants rewriting against §9 clause by clause, with the passing pieces (curl hardening, 401 split, `writeAll`, reopen path) carried over intact. |
| **C2c · unit / Makefile / conf.example** | **FIX-THEN-SHIP** | Every §9 requirement met. Missing hardening (U1) and cosmetic/spec tidy-ups only. |

**Blocking for deploy:** P1, P2, P3, C1, D1–D8.
**Spec amendments for the Lead:** S1, S2, P8, P9, P10, U2.

**UNVERIFIED:** the endpoint was not exercised against the live database (read-only static analysis plus schema cross-reference only) — P3's PHP-8 warning behaviour and P5's double-count are derived from the schema (`002_c2_capabilities.sql:79`, `004_assets_pools.sql:17`), not observed. D3, D4, D5, D6, D7 are read from code and the endpoint's emitted field set; they were not reached at runtime because D1 and D2 abort execution first. D8's 25-day stall is arithmetic on `CLOCK_MONOTONIC` semantics, not observed. D1, D2 and C1 were reproduced directly and their output is quoted verbatim above.

---

## RE-CHECK

Second pass against FIX ROUND 2 (`RETURN-C1.md`, `RETURN-C2.md`) plus the four Lead fixes applied during live execution. Scope: verify resolution, not re-review. Everything below is read from the uncommitted working tree and, where marked, re-exercised in a scratch copy.

### What I re-ran myself

I did not take the fix round's own test results as evidence. Independent harnesses, built in the scratchpad against the real `protocol.c` / `solariPanel.c`:

- **Parser harness** (`rt.c`, `cc -fsanitize=address,undefined`): R1 resync-drain 4/4 · R2 overlapping magic 1/1 · R3 CRC error mid-stream 3/3 (crcErrors=1) · R4 byte-at-a-time 1/1 · R5 timeout-then-good 1/1 · R6 oversize payloadLen 1/1 · R7 unknown type consumed without desync · R8 RFC1982 vectors incl. wraparound all correct · R9 20 000-iteration fuzz survived, ends `have=0`, no ASAN/UBSAN report. **EXIT=0.** R1 is the exact input that failed before the fix.
- **Daemon-decode harness** (`dt.c`, ASAN/UBSAN, via the new `SOLARI_PANEL_TEST` hook): wide counters survive (`rxKbps=3400000`, `txKbps=999999`) · `poolCount=1 systemCount=1` assigned · `episodeId=4242` read at the `data` level · `severity crit → 2` · malformed-pool document returns −1 with the caller's last-good snapshot bit-identical · an HTML login page likewise rejected without clobber · absent `id`/`episodeId` defaults to 0 without crashing · `hasTopAlert=2` normalised to wire byte `1` and accepted by the decoder · max roster (8 pools / 64 systems / alert) encodes to exactly 1308 B = 40 + 20·8 + 16·64 + 84 and round-trips.
- `php -l dashboard/api/routes/panel.php` clean. `make clean && make && make test` clean in a scratch copy under `-std=c99 -Wall -Wextra -Werror`.

### Disposition — round-1 findings

| # | Disposition | Note |
|---|---|---|
| **C1** parser drops frames after resync | **RESOLVED** | Rewritten as feed-one-byte-then-drain-inner-loop. Re-proven by R1/R9; termination is safe because `parserResync` strictly decreases `have`. |
| **C2** `hasTopAlert` non-boolean rejected by decoder | **RESOLVED** | `protocol.c:66` normalises to `? 1u : 0u`. Exercised. |
| **S1** no RFC1982 seq comparison exported | **RESOLVED** | `panelSeqNewer` added (`protocol.h:193`, `protocol.c:45`); wraparound vectors correct. |
| **D1–D15** (all eight MUSTs + SHOULDs) | **RESOLVED** | `readPassword` aliasing bug fixed via `memmove`; `curl_easy_init()` parenthesised; `username` key; staged-snapshot commit-on-success; wide-field types; severity map; `alarmActive` from API; monotonic-clock arithmetic in `int64_t`; glob iterates all matches; `SIGPIPE` ignored; `CURLOPT_NOSIGNAL`; header list freed; body zeroed. Verified by reading plus the `dt.c` run above. |
| **P2** two divergent `episodeId` expressions | **RESOLVED** | Single `$poolEpisodeId` computed once after `sort($breachingPools, SORT_NUMERIC)`, used in both branches. |
| **P3** `retired` state crashes pool counts | **RESOLVED** | `retired` skipped at `panel.php:221`, added to the `$poolAgg` init, and guarded by `array_key_exists` at `:258`. |
| **P6** alert counts and top alert from one over-fetched query | **RESOLVED** | Split into `$alertCountRows` (GROUP BY severity), `$critRows`, `$topAlertRow` (LIMIT 1). |
| **P9** `dataStale` hardcoded | **RESOLVED** | Derived from `UNIX_TIMESTAMP(MAX(sampledAt))` vs `ts > 30`. Emitted as a JSON boolean; `cJSON_IsTrue` handles it. Empty `hostCurrent` yields one NULL row → `false`, matching §9's zero-data rule. |
| **P12**, **U1**, **U4** | **RESOLVED** | Unit gained `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `ProtectKernelTunables`, `RestrictAddressFamilies`. |
| **P5** roster double-count | **RESOLVED** | Superseded by Lead fix [c]; `$seenNodeIds` dedupe verified. |
| **P1** `episodeId` not stable across an episode | **NOT-RESOLVED (residual, downgraded to SHOULD)** | `$critEpisodeId` is now the **minimum** active crit `eventId`, which fixes my documented scenario (newest crit clears mid-episode). The general defect stands: when the **oldest** crit clears while others remain active, the minimum moves, `episodeId` changes mid-episode, and a firmware-acked alarm re-arms. Probability reduced, not eliminated. A real fix needs server-side episode persistence — out of scope for a fix round; record as a known limitation. |
| **P4** `stateRoll` vs `pools`/`systems` count different populations | **NOT-RESOLVED (by design, now documented)** | `stateRoll` is node-verbatim; `pools`/`systems` are nodes plus adopted probe-only assets. Live numbers make the divergence visible: roll `[9 0 0 0 0]` against 15 systems in 4 pools. Acceptable, but **C3/firmware must be told these are different populations** or the panel will look self-contradictory. |
| **P7** tier 2 unreachable | **NOT-RESOLVED** | `panelTier` gained a `standby → 2` branch, but `standby` appears in neither `asset.class` (`server\|appliance\|network\|iot\|host\|other`) nor the `node.role` ENUM (`client\|monitor\|server`). Tier 2 remains data-unreachable; the branch is dead code. |
| **P8**, **P10** | **OPEN (spec)** | Unchanged; Lead-side amendments as originally filed. |
| **P11** `meanLoadPct` dilution | **NOT-RESOLVED — worsened**, see **N5**. |

### Lead's four fixes

| Fix | Verdict |
|---|---|
| **[a]** `username` not `user` | **Correct.** Matches `auth.php:18` (`$body['username']`). |
| **[b]** `staged.poolCount`/`systemCount` assigned | **Correct** (`solariPanel.c:74`), exercised in `dt.c`. |
| **[c]** node-driven roster + adopted probe-only assets | **Correct.** Genuinely eliminates P5. See **N1**, **N3**, **N4** for new issues in the join, none of them row-multiplication. |
| **[d]** suffix join for topAlert labels | **Correct** for the NULL case — `NULL = NULL` is not true, so a NULL `e.targetId` falls through to `hostFqdn`. See **N2**. |

**Direct answer to the two questions asked.** Neither join multiplies rows in the output. The OR-join *does* produce multiple rows per node when several assets match, but `$seenNodeIds` collapses them; the suffix join *does* match multiple `probeTarget` rows, but `LIMIT 1` collapses them. In both cases the bug is not duplication — it is **non-determinism about which duplicate survives**, because neither query has a tiebreak. That is N1 and N2.

### New defects

| # | Sev | Location | Defect |
|---|---|---|---|
| **N1** | SHOULD | `panel.php` roster query, `ORDER BY n.nodeId` | No tiebreak. When a node matches several assets, the surviving row is whichever the optimiser emits first, so `poolId`/`tier`/`displayName` can flip between 5 s polls. Fix: `ORDER BY n.nodeId, a.nodeId IS NULL, a.assetId`. |
| **N2** | SHOULD | `panel.php` topAlert suffix join | Matches every `probeTarget` sharing a `host:port` suffix across protocols; `LIMIT 1` has no `pt` tiebreak → alert label flickers between protocols for one unchanging alert. Fix: add `pt.targetId` to the ORDER BY, or match on protocol too. |
| **N3** | SHOULD | `panel.php` roster + adopted queries | An asset with a **non-NULL but dangling** `nodeId` matches neither query (the join fails; the NULL-branch and the adopted query both exclude it). It vanishes from the roster with no diagnostic. Latent today; a stale `nodeId` after a node deletion triggers it. |
| **N4** | SHOULD | `panel.php` OR-join + `NOT EXISTS` subquery | `SUBSTRING_INDEX(...)` predicates are non-sargable → O(nodes × assets) every 5 s. Fine at 30 nodes; contradicts the design's scale-free 30→3000 claim. Fix: populate `asset.nodeId` and drop the name-matching branch. |
| **N5** | SHOULD *(was NIT P11)* | `panel.php` `meanLoadPct` | Divides by all 15 systems including 6 adopted probe-only ones that report zero load → ~40 % understatement. The roster change made this materially worse; upgrading. |
| **N6** | **MUST-FIX** *(was NIT U3)* | `solariPanel.c:38` + `conf.example` | The new `fstat` mode check hard-fails a 0600-required password file, but `conf.example` documents only `mode 0644` and never mentions the password file's permissions. Failure message is the misleading "configuration or password file unavailable", and `readPassword` returns −1 for four distinct reasons with no logging to tell them apart. With `Restart=always` + `StartLimitIntervalSec=0` this is a **permanent silent crash loop on first install**. See deferral note below. |
| **N7** | NIT | `solariPanel.c:48` | `CURLOPT_COPYPOSTFIELDS` is immediately overridden by `request()` setting `CURLOPT_POSTFIELDS`. Behaviour is still correct (the buffer outlives the call), but `RETURN-C2.md`'s "copied by curl" justification is false — the zeroing is what makes it safe. |
| **N8** | NIT | `solariPanel.c:99` | `serialDelay`/`authDelay` overshoot their stated maxima by one doubling (32 vs 30, 64 vs 60) before clamping. |
| **N9** | NIT | `solari-panel.service` | `ReadOnlyPaths=/etc/solari-panel` is redundant under `ProtectSystem=strict`. |
| **N10** | NIT | `panel.php` `$critRows` | Fetches all crit rows to take a minimum; `SELECT MIN(eventId)` does it in the engine. |
| **N11** | NIT (doc) | `RETURN-C2.md` | Self-contradicts on U1 — listed under both FIXED and SKIPPED. |
| **N12** | NIT | `panel.php:258` | `array_key_exists` tests against the aggregate array (which also holds `name`, `tier`, `loadSum`, `total`, `loadPct`), not a five-state whitelist. Safe under today's ENUM; a state named `total` would collide. |
| **N13** | NIT | `protocol.c` `panelParserFeed` | After a drain leaving `remain==1`, that byte is treated as a confirmed magic0 without re-validation. Self-correcting, ~1/65536, pre-existing. |

### Deferred SHOULDs

- **D16** (login body scrub) — **acceptable to defer.** Partially done already; the `memset` is real.
- **D17** (20 ms busy-poll) — **acceptable to defer.** Costs idle CPU, threatens nothing.
- **U3** — **NOT acceptable to defer.** Re-filed as **N6, MUST-FIX**. It was a NIT when it was only a docs gap; the fix round added a hard permission check on the same file, which converts the docs gap into a guaranteed crash loop on a clean install. Two-line fix: document `0600` for the password file in `conf.example`, and log the four `readPassword` failure modes distinctly.

### Final verdicts

| Component | Round 1 | Now | Rationale |
|---|---|---|---|
| **C1 · `panel.php`** | FIX-THEN-SHIP | **FIX-THEN-SHIP** | All three MUST-FIX resolved. Residual: P1's narrowed re-arm window, plus N1/N3/N4/N5. None blocking; N1 and N5 are the ones users would notice. |
| **C2a · `protocol.c`** | FIX-THEN-SHIP | **SHIP** | Every defect resolved and independently re-verified under ASAN/UBSAN plus a 20 000-iteration fuzz. N13 is a documented curiosity, not a fault. |
| **C2b · `solariPanel.c`** | REWORK | **FIX-THEN-SHIP** | Upgraded two steps. All eight MUST-FIX genuinely resolved, not resolved-in-claim — the staged-snapshot rewrite in particular is the correct shape, and I exercised it. The fix round earned this. |
| **C2c · unit / Makefile / conf.example** | FIX-THEN-SHIP | **FIX-THEN-SHIP** | Hardening complete. Now blocked only by **N6**, which lives half in `conf.example`. |

**Blocking for deploy: N6 alone.** Everything else is a SHOULD or a spec item.

**UNVERIFIED:** I did not exercise the live endpoint against the real database — P1's residual, N1, N2, N3 and N5 are derived from the schema and the query text, not observed on live data; N4's complexity claim is read from the query plan shape, not measured. N6's crash loop is reasoned from the `fstat` check plus the unit's restart directives, not reproduced against systemd. The daemon was not run against real USB hardware or the live xenon API by me; my daemon coverage is the `SOLARI_PANEL_TEST` entry point only, so the poll loop, TLS path and serial reopen remain unexercised on my side. Everything in "What I re-ran myself" was executed and its output is quoted.
