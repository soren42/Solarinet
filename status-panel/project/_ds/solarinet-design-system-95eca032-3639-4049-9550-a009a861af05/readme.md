# SolariNet Design System

SolariNet is a **unified systems, network and asset monitoring** product — a
self-hosted NOC console for operators watching a fleet of hosts, the applications
running on them, and the network paths between them. It is an operator tool, not a
consumer product: dense, dark by default, monospace where values live, and built to
stay readable from three systems up to three hundred.

The interface is a single console. A left rail groups the work into **Monitor**
(Fleet Overview, Reachability, Topology Map, Alerts) and **Manage** (Discovery,
Provisioning, Config & Rules). Everything is touch-viable — the stated primary
client is Safari on iPad — and the product self-hosts its fonts and assets so it
runs air-gapped, with no CDN dependency at page load.

## Sources this system was built from

Two files were supplied, both single-file bundles under
`uploads/SolariNet Monitoring Dashboard 3/`. Both were unpacked and their real
sources read — CSS, React JSX, the icon module, the mock data layer — rather than
being reconstructed from screenshots.

1. **`SolariNet Dashboard Prototype (standalone).html`** — the working console.
   A pinned React + Babel app: `~1500` lines of CSS plus seven JSX modules
   (mock data layer, icon set, shared components & charts, three screen modules,
   app root). This is the source of truth for layout, density, column sets,
   interaction model and copy. Its unpacked sources are kept in `src/` for
   reference.
2. **`SolariNet Interface Guide Rev2 (standalone).html`** — *Interface & Identity
   Standards, Rev 2.0.0*, dated 2026-07-29. An 18-page Letter document that audits
   the first generated identity pass and specifies the corrections: colour
   architecture, palette, contrast, the six-state status system, the quiet-healthy
   principle, logo and construction, typography and scale, iconography,
   components, layout, empty states and vocabulary, and an ordered handoff list.
   This is the source of truth for **rules**.

No Figma file, repository or live URL was provided. No third source exists for
anything not in those two files.

### Where the two disagree, the standards win

The prototype predates Rev 2, so its shipped values differ from the standards in
places. This system implements the standards. Specifically:

| Concern | Prototype ships | This system (Rev 2) |
| --- | --- | --- |
| Ground | `#05080e` | `#070A0F` |
| Ink | `#d8e6f4` | `#DFE8F2` |
| Card surface | `#0a121c` | `#0E151D` |
| Degraded | `#ffb23d` | `#F5A623` |
| Down | `#ff3d72` | `#FF4D5E` |
| Ink faint | `#6e8399` (below 3:1 in places) | `#6E8399`, licensed for 10px uppercase Mono only |
| Nominal cards | glowing underline + halo | neutral chrome, no glow |
| Operational tiles | green border + tint + glow | hairline border, no fill, no glow |
| Nav / chips / buttons | monospace | Sans |
| Heartbeat | on/off switch | sparkbar + age indicator |
| Metric row | six auto-fit cards | fixed four columns + two-up strip |
| Maintenance state | absent | present, violet, dashed border |

Rev 2 also directs a migration of the icon set to Lucide outline (handoff item 14).
That has not shipped: the product's own solid glyph set is what exists, so it is
what this system ships — see **Iconography** below.

---

## Content fundamentals

The product's voice is that of a **competent engineer writing for another
engineer**. It states what is true, names things once, and does not perform.

**Tone.** Declarative and unhedged. Findings are stated as facts, not softened:
"Reachability lost from every vantage." "No vantage can reach this target. Treat it
as down." Where the standards give a rule, it is an imperative with a reason
attached: "Status is observed, never operable." No marketing register anywhere, no
"we", no "Oops", no exclamation marks.

**Person.** Second person for instruction, never first person for the product. The
system does not say "I found 5 hosts" or "We couldn't reach that". The operator is
"you" only in guidance copy; UI strings are usually impersonal — "Awaiting first
sample", "Segments not configured", "Enrolment token issued".

**Casing.** Sentence case for every sentence, heading and button — "Survey now",
"Push config", "Acknowledge all". UPPERCASE is reserved for 10px monospace micro
labels with `+0.15em` tracking (`SYSTEMS`, `OPERATIONAL`, `ACTIVE C2`) and for the
uppercase Mono table headers. Never uppercase a sentence.

**Vocabulary — one word per concept.** This is a hard rule in the standards:

| Use | Means | Never |
| --- | --- | --- |
| System | A physical or virtual host under monitoring | node, machine, box |
| Application | A watched service or process on a system | proc, service, app |
| Target | A probed network endpoint — proto, host, port | check, probe, endpoint |
| Vantage | A monitor's viewpoint onto a target | agent, collector |
| Pool | An operator-defined group of systems | segment, group, cluster |
| Segment | A network range — CIDR-defined, not chosen | subnet, zone, VLAN |

**Counts must agree.** One count per concept, derived once, rendered everywhere
from the same source. Rev 1 reported 18 systems in one header and 9 in another;
that is treated as a defect, not a rounding difference.

**Absence is a state with copy, not a dash.** Never ship a placeholder as content:
not `—`, not `V—`, not `0/0`, not `0 SEGMENTS`. Three kinds of nothing, each with
distinct copy:

- *Not configured* — the feature exists, the operator has not set it up. Name it,
  offer the primary action: "Segments not configured".
- *No data yet* — configured, awaiting first report. Say what is expected and when:
  "Awaiting first sample".
- *Filtered to nothing* — data exists, the filter excludes it. Restate the filter,
  offer to clear it: "No systems match this filter".

**Emoji: never.** Not in UI, not in docs, not in commit-style copy. Unicode
geometric primitives (● ▲ ■ ◆ ○ ◐) are used as the status glyph vocabulary and are
the only non-Latin characters in the product; `·` separates fields in Mono meta
lines; `▲ ▼` mark table sort direction; `↵` and `⌘K` appear as keyboard hints.

**Copy rhythm.** Alert titles are a subject plus a state — "PiHole 1 unreachable",
"Split vantage on tcp/443", "Disk /var over tolerance on molybdenum". Detail lines
are Mono, field-separated: `core-infra · all 2 vantages · 4m`. Toasts state what
happened: "Fleet survey dispatched to all vantages". Captions under a metric must
agree with the metric — a red `1` above "0 critical alerts" is a bug.

**The standing test**, quoted from the standards, settles anything not covered:
*does this pixel tell the operator something they did not already know?*

---

## Visual foundations

**Character.** Restrained cyberpunk. Dark, technical, hairline-ruled — but
disciplined: every colour on screen is information. The product looks like an
instrument, not a poster.

**Colour architecture.** One rule governs the palette: *a hue that reports
condition may not also decorate the interface.* Azure owns chrome, links, focus,
selection and info. Green, amber and red belong to condition and appear nowhere
else. Violet is maintenance; slate is unknown. Three tiers — primary (capped at
three), neutrals and surfaces, functional. Coverage ceilings are part of the spec:
accent ≤ 10% of a surface, amber and red together ≤ 5%, outer glow on at most one
element in view.

**Type.** Two faces, self-hosted as WOFF2. IBM Plex Sans (variable, 400–700) for
human language — nav, buttons, headings, prose, captions, alert titles, form
labels, chips. IBM Plex Mono (400/500/600/700) for machine values — hostnames, IPs,
ports, metric numerals, byte rates, durations, timestamps, config keys, log
excerpts, fingerprints. Nine steps, floors stated: 13.5px for any sentence, 12px
for values, 10px for uppercase Mono labels, nothing below 10. `tabular-nums`
wherever numbers stack, so digits do not shimmy as values tick.

**Backgrounds.** No photography, no illustration, no gradient washes. The app field
is a **34px azure hairline grid at 4.5% opacity** over Void — the only texture in
the product. Panels sit on it as flat card surfaces. Print pages are flat ground
with a 2px ink rule under the header. There are no brand illustrations, no hero
imagery and no stock photography in the sources, and none have been invented.

**Cards and panels.** 1px hairline border in `--sn-divider`, 14px radius, flat
`--sn-surface` fill, no drop shadow. Shadow is used only where something floats:
modals, the command palette, toasts, and the drawer rail on narrow widths
(`0 10px 30px rgba(0,0,0,.55)`). Node tiles are the tight variant — 5px radius,
84px minimum height, 132px minimum width.

**Borders.** Two weights: `--sn-divider` hairline for structure, and
`--sn-divider-strong` for a table's header underline and for floating surfaces.
Dashed hairlines separate metric rows inside a panel; a dashed border means
*maintenance* on a tile. A 3px left edge marks alert severity; a 3px left bar marks
the active nav item.

**Shadow and glow.** Inner shadow is used once: `inset 0 0 0 1px` on the selected
segment of a segmented control. Outer glow is rationed — `--sn-glow` is `.55` in
dark theme and **`0` in light**, so glow simply does not exist on paper-like
surfaces. Only `down` may glow, and only for the single worst item in view.

**Transparency and blur.** Blur appears in exactly two places: the scrim behind
modals and the command palette (`blur(3px)` over `rgba(2,5,10,.62)`), and the top
bar in the source build. Functional tints are the only other transparency: 12% fill
for degraded, 16% for down, ~13% for azure and severity pill backgrounds. No
frosted panels, no translucent cards.

**Animation.** Fast and unshowy. 120ms for colour and background changes on
controls, 150ms for switches and theme, 250ms for the rail collapse and the toast
entrance (`translateY(8px)` + fade). Easing is `cubic-bezier(.4,0,.2,1)`. No
bounces, no springs, no scale-in modals, no animated status pulses — a blinking
fault is harder to read than a static one. Gauges animate their stroke offset over
500ms when a value changes.

**Hover.** Desktop hover raises the surface one step (`--sn-raised` →
`--sn-elevated`) and lifts ink from secondary to primary; interactive borders shift
to `--sn-border-interactive` (azure at 22%). Hover never reveals information — the
primary client is a touch device, so every value shown on hover must also be
reachable by tap, and no tooltip may be the only source of a status, threshold or
identifier.

**Press.** Background steps to `--sn-elevated`, ink to primary. Node tiles
`scale(.96)` on press — the one transform in the product. Primary buttons invert on
press to `--sn-azure-deep` with `--sn-on-field` ink.

**Focus.** `0 0 0 3px rgba(34,184,240,.18)` — azure, never a status hue.

**Radii.** 5px tiles and tags, 9px controls, 14px panels and cards, 20px pills and
badges, 50% status dots only.

**Layout.** Fixed, not fluid, where fluidity can orphan: the metric row is four
columns declared, with supporting figures dropping to a two-up strip. Sidebar 248px
/ 64px collapsed, top bar 60px, content max 1640px centred, page padding 22/26px,
panel padding 16px, card gap 12px, tile gap 8px, tile minimum 132px, touch target
44px everywhere. Breakpoints: Wall ≥1600, Desktop 1200–1599, Tablet landscape
1024–1199 (the primary target, rail collapsed), Tablet portrait 768–1023 (overlay
drawer).

**Imagery.** There is none — no photographs, no illustration, no texture beyond the
hairline grid. The visual weight is carried by data: sparklines, heatmap lattices,
gauge rings, probe matrices. Charts are drawn in accent or condition colour with a
low-opacity gradient fill beneath the stroke.

**The quiet-healthy principle.** The single most consequential visual rule: at 300
systems roughly nine in ten are fine, so painting the majority bright is what hides
the minority that is not. Operational renders as a calm lattice of hairlines with a
`--sn-quiet` sparkline; amber, red, violet and glow are the exceptions the eye
lands on.

---

## Iconography

**One in-house set, no library.** The product ships its own glyph set — solid
geometric marks on a 24×24 grid, drawn in `currentColor`, with knockouts in
`--sn-on-field`. Roughly 45 glyphs covering nav and structure (`overview`,
`server`, `host`, `monitor`, `reachability`, `topology`, `alerts`, `discovery`,
`provision`, `settings`), metrics (`cpu`, `ram`, `disk`, `network`, `bandwidth`,
`activity`, `process`), UI (`search`, `command`, `grid`, `table`, `cards`, `bell`,
`sun`, `moon`, `menu`, `close`, `chevronLeft/Right`, `check`, `refresh`, `filter`,
`plus`, `enter`) and network gear (`gateway`, `netswitch`, `wifi`, `link`,
`shield`, `clock`, `pulse`, `arch`, `chip`, `survey`).

They were **copied verbatim** from the product build into
`components/core/Icon.jsx` — nothing was redrawn. Use `<Icon name="…" size={18} />`;
sizes are 16 / 18 / 20 / 24 (nav uses 19, top-bar buttons 20, table cells 15).

**No icon font, no SVG sprite, no PNG icons.** The glyphs are inline JSX paths.
The only raster asset in the sources is `assets/apple-touch-icon.png` (the PWA
touch icon, copied in).

**Status primitives are not icons.** Dot, triangle, square, diamond, ring and half
ring are solid geometric primitives on the same 24 grid, and they must never be
swapped for a library glyph. They carry condition; icons never do — an icon names
a thing, and its colour may follow selection but never state.

**Rules.** Never mix fills — all chrome icons are one style; only status primitives
are solid-with-meaning. Never icon-only without an accessible name and a focus
tooltip, especially in the collapsed rail. Badges beside icons are a single pill
shape: red only for unacknowledged criticals, slate for every other count.

**Migration flagged.** Rev 2 handoff item 14 directs replacing the set with
**Lucide** (ISC, 24 grid, 1.75px round-cap stroke, outline only, sizes 16/18/20/24)
and gives the nav mapping: Fleet → `layout-grid`, Reachability → `radio`, Topology
→ `network`, Alerts → `triangle-alert`, Analysis → `activity`, Systems → `server`,
Inventory → `package`, Discovery → `scan-search`, Provisioning → `plus-square`,
Certificates → `shield-check`. **That migration has not happened.** This system
ships what exists — the in-house solid set — so consumers match the real product
today. When Lucide lands, swap `Icon.jsx`'s path table and this section, and expect
the "solid vs outline" character of the UI to change noticeably.

**Logo.** The Rev 2 mark — a server hub with three monitor vantages on a 24-unit
grid — was extracted from the sources (it appears in both the guide and the
console rail) and is in `assets/` as `logo.svg` (horizontal lockup),
`logo-stacked.svg`, `logo-mark.svg`, `logo-mark-light.svg` and
`logo-mark-onecolour.svg`. Nothing was drawn from memory. The wordmark uses weight
contrast, not colour contrast: `Solari` at 400 against `Net` at 700.

---

## Index

### Root

| File | What it is |
| --- | --- |
| `styles.css` | The entry point consumers link. `@import` lines only. |
| `readme.md` | This file — context, content fundamentals, visual foundations, iconography, index. |
| `SKILL.md` | Agent-skill front matter, for using this system outside this project. |
| `thumbnail.html` | Homepage tile for the system. |

### `tokens/`

`fonts.css` (the two `@font-face` families) · `colors.css` (three-tier palette, dark
and light) · `typography.css` (faces, nine steps, line heights, weights, tracking,
floors) · `spacing.css` (4px scale plus layout constants) · `radii.css` ·
`effects.css` (glow, shadow, focus ring, scrim, blur, grid field, motion) ·
`semantic.css` (the aliases you should actually author against) · `base.css`
(element defaults only).

Author against the semantic aliases — `--sn-text-primary`, `--sn-surface-card`,
`--sn-border-hairline`, `--sn-accent`, `--sn-status-down` — not the raw palette.

### `assets/`

`logo.svg` · `logo-stacked.svg` · `logo-mark.svg` · `logo-mark-light.svg` ·
`logo-mark-onecolour.svg` · `apple-touch-icon.png` · `fonts/` (IBM Plex Sans
variable + IBM Plex Mono 400/500/600/700, all WOFF2, latin subset, SIL OFL 1.1).

### Components

**`components/core/`** — `Icon`, `Button`, `IconButton`, `Chip`,
`SegmentedControl`, `Tag`, `Switch`, `SearchField`

**`components/status/`** — `StatusDot`, `StatusCell`, `StatusPill`, `Heartbeat`

**`components/charts/`** — `Sparkline`, `TimeSeries`, `RadialGauge`, `HealthDonut`,
`MetricBar`, `BandwidthGauge`, `RTTBars`

**`components/data/`** — `MetricCard`, `NodeTile`, `Panel`, `DataTable`,
`PoolCard`, `MetricRow`

**`components/feedback/`** — `AlertRow`, `Toast`, `ToastStack`, `EmptyState`,
`Modal`, `CommandPalette`

**`components/navigation/`** — `BrandMark`, `SidebarNav`, `TopBar`, `PageHeader`

Every component has a sibling `.d.ts` (props contract and the rule it enforces) and
`.prompt.md` (what & when, usage, variants). Each directory has a `@dsCard` HTML
showing its states.

Also exported: `STATUS_COLOR`, `STATUS_LABEL`, `ICON_NAMES`, `metricColor(pct)`.

**Inventory provenance.** This list is exactly the component families the source
build defines — the shared components & charts module, the icon module, and the
recurring CSS component classes (`.kpi`, `.cell`, `.scard`, `.alert-row`, `.chip`,
`.seg`, `.tag`, `.switch`, `.panel`, `.metric-row`, `.grid`, `.cmdk`, `.toast`,
`.modal`). Nothing standard-but-absent was added: there is no Tabs, no Avatar, no
Accordion, no Breadcrumb, because the product has none.

**Intentional additions** (three, each with a reason):

- `StatusCell` — the source repeats "dot + word" inline in several tables and
  omits it in one. Rev 2 makes it mandatory, so it is a component.
- `Heartbeat` — the source draws heartbeat as a `Switch`. Rev 2 forbids that and
  specifies a sparkbar plus age; the replacement needed a home.
- `EmptyState` — the source ships placeholder glyphs (`—`, `V—`, `0/0`) as content.
  Rev 2 specifies three kinds of nothing with distinct copy.

### `guidelines/`

21 specimen cards feeding the Design System tab, grouped **Colors** (primary,
functional, surfaces, ink ramp, tints, hue assignment), **Type** (Sans, Mono,
nine-step scale, division of labour, floors), **Spacing** (scale, layout constants,
density in use, radii, emphasis budget) and **Brand** (lockups, mark construction,
mark misuse, the app field, iconography).

### `ui_kits/`

**`monitoring-dashboard/`** — a click-through recreation of the console: Fleet
Overview (heatmap / table / pool cards), System Detail, Alerts & Tolerances,
Reachability, Discovery, plus the command palette and toast stack. See its own
`README.md` for what was recreated and what was deliberately left out.

### `templates/`

**`standards-page/`** — the Letter-page layout of the Interface & Identity
Standards document, as a reusable starting point for further specification pages.

### `src/`

The unpacked sources of the two supplied bundles, kept for reference: extracted
templates, the seven JSX modules of the console, and the guide markup. Not compiled
into the system; delete if you do not want the reference.
