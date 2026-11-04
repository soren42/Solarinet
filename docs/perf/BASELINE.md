# Dashboard load baseline

Captured before performance changes on 2026-11-04. The browser harness is
[`tools/perf/dashboard_load.py`](../../tools/perf/dashboard_load.py). It uses an
iPad-sized 834×1194 viewport, 4× CPU slowdown in Chromium, a fresh context for
the cold run, and a reload in the same context for the warm run. It records the
per-request waterfall and waits for quiet API traffic and visible loading cues
to clear before marking a panel rendered.

## Authentication and browser coverage

The checked-in docs describe local password auth and OIDC, but contain no
reusable test principal or browser session. No password or session cookie was
available to this task. The public dashboard therefore reaches the login gate;
full authenticated cold/warm panel timings and waterfalls are **UNVERIFIED**.
The harness accepts an operator-provided Playwright storage-state file using
`--storage-state`; it reads that file without copying or printing it. No credential
was added to the repository.

Server-side curl from `xenon` to its localhost HTTPS vhost returned HTTP 401 for
`/api/nodes`, `/api/discovery`, `/api/netgear`, `/api/forgejo`, and `/api/ca`
without a session (about 8 ms each). Those values measure only the authentication
gate, not API work. A read-only EXPLAIN was available through the host's database
admin path; the unprivileged default DB client was denied. No response bodies or
configuration values were read.

The user-provided server observations below are the only available authenticated
API baseline. They are endpoint response times, not full-page panel completion
measurements. `nodes` is the known slowest API call. The browser harness could
not produce comparable panel timings without an authenticated session.

| Endpoint / panel source | Baseline | Evidence |
|---|---:|---|
| `/api/nodes` (fleet) | ~14 s p50 | Operator-supplied prior measurement |
| `/api/discovery` | ~1.2 s | Operator-supplied prior measurement |
| `/api/netgear` | ~214 ms | Operator-supplied prior measurement |
| Other core APIs | 9–60 ms | Operator-supplied prior measurement |
| `/api/forgejo` | HTTP 500 | Operator-supplied prior observation |
| `/api/ca` | HTTP 500 | Operator-supplied prior observation |
| Full page, each panel, cold/warm | Not measured | No authenticated storage state available |

## Host-history query plan

Read-only `EXPLAIN FORMAT=JSON` on `xenon` before the query change showed the
windowed query scanning and sorting all **3,630,498** rows in `hostHistory`,
with estimated cost **3,225.56**. The bounded per-node query used the existing
`nodeId` index (`access_type: ref`) and estimated **2** rows and cost **0.00692**
for the probe node. This is optimizer cost/row evidence, not a wall-clock
measurement; live endpoint latency remains unverified without an authenticated
session.

The browser run could not be completed here: WebKit downloaded but lacks host
runtime dependencies, and the Chromium CDN lookup failed. The Playwright venv and
browser artifacts are outside the repository. The committed harness remains
ready to run on a host with an installed browser.

## Reproduction

Create an isolated virtualenv outside the repository, install Playwright and its
browser, then run `tools/perf/dashboard_load.py`. To capture live panel timings,
pass a valid storage state created by an already-authorized operator session.
The JSON output contains the per-run readiness results and request waterfall.
