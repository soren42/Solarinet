# Status panel v2 contract

Authoritative interface for the screens-v2 redesign (2026-11-05). Everything
here is additive to CONTRACT.md and CONTRACT-AW.md: wire version stays 0x01, and
firmware already in the field ignores every new byte.

## 1. Screens

Sixteen slots, flattened index `theme*4+slot`. Themes are grouped by the question
they answer. Rotation tours every enabled screen across all themes; a theme
button jumps to that theme, pressing it again advances within it.

| Idx | Id | Theme | Screen | Data |
|---|---|---|---|---|
| 0 | A0 | FLEET | Host Tiles | systems + pop tail |
| 1 | A1 | FLEET | Roster | systems |
| 2 | A2 | FLEET | Pools | pools |
| 3 | A3 | FLEET | Head Count | stateRoll, alerts |
| 4 | B0 | TRAFFIC | Swim Lanes | links |
| 5 | B1 | TRAFFIC | WAN and LAN | totals, local history |
| 6 | B2 | TRAFFIC | Top Talkers | sysx |
| 7 | B3 | TRAFFIC | Flow Map | links (parent) |
| 8 | C0 | TROUBLE | Alerts | alert list |
| 9 | C1 | TROUBLE | Fault Board | faultCounts |
| 10 | C2 | TROUBLE | Reachability | probes, local history |
| 11 | C3 | TROUBLE | Alert History | alert heat |
| 12 | D0 | RESOURCES | Host Detail | systems + sysx |
| 13 | D1 | RESOURCES | CPU and Memory | systems + sysx |
| 14 | D2 | RESOURCES | Trends | local history |
| 15 | D3 | RESOURCES | Aurora | systems + sysx |

Config: `CONFIG` payload is 20 bytes: bytes 0-11 slots 0-11, byte 12 flags, bytes
13-15 zero, bytes 16-19 slots 12-15. A decoder accepts 16 bytes (slots 12-15
default to enabled, 1x). `PANEL_CTL_SETSCREEN` arg 0-3; `SCREENEN`/`SCREENWT`
index 0-15. `/api/panel/config` accepts 12 or 16 entries; a 12-entry report is a
pre-v2 firmware. The stored meaning of the old 12 indices does not carry over: on
first boot the firmware migrates its flash record to defaults, and the server
treats its stored per-screen settings the same way.

## 2. Text law

The small font is 5 rows at 4 px pitch, so a row holds 13 characters. BIG is 7 rows at
5 px pitch. Text pixels are full brightness; hierarchy is by hue.

- Two text rows: y=0 and y=6. Row 5 is the gap. One row alone: y=3.
- BIG occupies y=2..8 and shares rows with small text only if their columns are disjoint.
- Nothing scrolls unless a screen says so. Long content pages, at least 3 s a page.
- No text over a graphic, other than a title card dimming the whole frame.
- Text pixels never overlap other text. panelParityTest enforces this on attempted coordinates.

## 3. Wire: TLV trailer

After the gear trailer (always emitted, even with count 0), repeated
`tag u8, len u16 LE, value`. Decoders skip unknown tags, stop quietly at a
truncated TLV and treat a malformed value as absent. Multi-byte values are LE.
`PANEL_MAX_PAYLOAD` becomes 4096. Older firmware drops a frame above 2048, reachable only with a fleet far larger than today's.

| Tag | Name | Value |
|---|---|---|
| 1 | LINKS | count u8 (<=12), then 24-byte records: name[12], kind u8, state u8, parent u8, 0, upKbps u32, downKbps u32 |
| 2 | SYSX | count u8 (=systemCount), then 12-byte records parallel to systems: memPct, diskPct, faults, 0, rxKbps u32, txKbps u32 |
| 3 | ALERTS | total u8, faultCounts[7], count u8 (<=8), then 34-byte records: severity u8, code u8, ageMin u16, host[12], msg[18] |
| 4 | PROBES | count u8 (<=11), then 14-byte records: name[8], rttTenthMs u16, lossPermille u16, state u8, 0 |
| 5 | POP | count u8 (<=191), then 1 byte each: state<<5 | (loadPct*31/100) |
| 6 | HEAT | warn[52], crit[52]: distinct alert firings per 30 min bucket, oldest first, last bucket = now |
| 7 | TOTALS | wanUpKbps, wanDownKbps, lanUpKbps, lanDownKbps: u32 |

`kind`: 0 WAN, 1 TRUNK (gear to gear), 2 WIFI (AP uplink), 3 HOST (agent-reported
host NIC). `parent`: index into this LINKS list, 0xFF for none. `up` is toward
the internet, `down` away from it. A WAN link's name is `INTERNET`, or `BACKUP`
for a failover WAN. `state`: PanelState values. An unmeasured rate is
0xFFFFFFFF and renders as no data, never as zero traffic.

`code`: 0 other, 1 reachable, 2 health.failedUnits, 3 health.fsReadonly, 4
health.smartFail, 5 health.dmesgCrit, 6 health.blockDevMissing. `faultCounts[code]`
counts distinct current conditions. `total` saturates at 255.

## 4. JSON additions to GET /api/panel

Existing keys keep their shape. `systems[]` gains `memPct`, `diskPct`, `faults`,
`rxKbps`, `txKbps`. New top-level keys: `links[]` (`name, kind, state, parent,
upKbps, downKbps`, null rate = unmeasured), `alertList{total, faultCounts[7],
items[]{severity, code, ageMin, host, msg}}`, `probes[]{name, rttTenthMs,
lossPermille, state}`, `popTail[]` (the bytes), `alertHeat{warn[52], crit[52]}`,
`totals{wanUpKbps, wanDownKbps, lanUpKbps, lanDownKbps}`.

## 5. Population rules

- One roster: `stateRoll`, `pools`, `systems` and `popTail` all count the same hosts.
- `systems` is worst-first (state, then criticality, then load, then name), so the
  64 named are the ones an operator most needs. `popTail` carries the rest.
- Host names are the short display name: first DNS label, uppercased, up to 12 chars.

## 6. Alert text

`host`: short host name. `msg`: at most 13 characters, plain words:

| Metric | msg |
|---|---|
| reachable | `NO RESPONSE` |
| health.failedUnits, one unit | `<UNIT> FAILED`, unit name cut to fit |
| health.failedUnits, several | `<n> SVCS FAILED` |
| health.fsReadonly | `DISK READONLY` |
| health.smartFail | `DISK FAILING` |
| health.dmesgCrit | `KERNEL ERRORS` |
| health.blockDevMissing | `DISK MISSING` |
| other | metric's last label, uppercased, cut to fit |

`topAlert.subject` = host, `topAlert.detail` = msg. Vital-down and pool-breach
alerts use the same two-row shape (`VITAL DOWN` / host, `POOL BREACH` / pool).

One current condition appears once, whatever the number of `alertEvent` rows. Conditions come from
the deduplicated set, ordered criticality, severity, then newest.
Alarm predicate, episode ids and vital-down logic do not change.

## 7. Data honesty

- Every moving element on a screen moves because of a measurement. No synthetic jitter.
- `rttTenthMs`/`lossPermille` aggregates exclude targets with no success in 24 h; those
  targets still appear in `probes`.
- Screens never claim a percentile the server did not compute.
- Local history starts empty and draws only real samples: nothing is back-filled.
