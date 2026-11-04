# Dashboard load performance — results (2026-11-04)

Goal: cut full-page load (boot + every panel rendered, API traffic quiet) by at least 75%.

## Method

`tools/perf/dashboard_load.py --insecure --browser chromium --timeout-ms 30000`, run on xenon
against https://dashboard.akoria.net with a temporary viewer principal (removed afterwards).
iPad portrait viewport (834x1194, DPR 2, touch), Chromium with 4x CPU throttle, cold then warm.
"Ready" means no API request in flight for 500 ms and no visible loading cue. A panel that never
gets there within 30 s is counted as 30 s, so the "before" totals are lower bounds. WebKit could
not be run on xenon (missing host libraries), so Chromium is the measurement of record.

## Full page (boot + all 18 panels)

| | Before (rc-20261103-c39f565-io) | After (rc-20261104-c431280-io) | Change |
|---|---:|---:|---:|
| Cold, full page | >= 218.0 s (6 panels never quiet) | 27.3 s | **-87%** |
| Warm, full page | >= 217.2 s (6 panels never quiet) | 17.2 s | **-92%** |
| Cold, first render | 28.6 s | 12.0 s | -58% |
| Warm, first render | 27.9 s | 2.5 s | -91% |
| Panels ready within 30 s | 12 / 18 | 18 / 18 | |

Checkpoints: -25%, -50% and -75% were all passed by this single release.

## API (server-side curl, authenticated)

| Endpoint | Before | After |
|---|---:|---:|
| `/api/nodes` | 14.2 s | 0.019 s |
| `/api/forgejo` | HTTP 500 | 200, 0.39 s (30 s cache) |
| `/api/ca` | HTTP 500 | 200 |
| `/api/discovery?status=new` | 1.3 s | 1.3 s (unchanged) |

## Causes and fixes

1. `/api/nodes` ranked all ~3.7 M `hostHistory` rows with `ROW_NUMBER()` to keep 40 per node.
   Now one indexed `LIMIT 40` lookup per node on `(nodeId, sampledAt)`.
2. The 10 s poll fired a fresh ~25-request fan-out even while the last one was still waiting on
   the 14 s nodes read, and parallel reads serialized on the PHP session lock. The queue never
   drained, which is how panels reached 60-90 s. Now `API.refresh()` is single-flight with at most
   one queued follow-up, and authenticated GETs release the session lock after authz.
3. `curl_close()` is deprecated in PHP 8.5 and the error handler promotes deprecations, so the
   Forgejo and CA endpoints threw after every successful fetch. Removed (a no-op since PHP 8.0).
4. Forgejo and CA summaries no longer gate first render. They are cached for 30 s and degrade
   to "unavailable" instead of 500.

## Remaining levers (not needed for the goal)

- Cold first render (12 s under 4x throttle) is dominated by in-browser Babel transforms of about
  1.1 MB of JSX on a first visit. The transform cache already makes warm boots 2.5 s.
- The poll bundle still re-reads six inventory lists (`limit=1000`) and discovery (95 KB, 1.3 s)
  every 10 s. These could move to screen-mounted reads.
