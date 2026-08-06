# RETURN-LC2

## STATUS

complete — closed out by Lead (fable-5). The lane's codex runs completed all
deliverables; the wrapper agent was lost twice (a Monitor wait that never
notified, then the 2026-08-06 ~02:0x power hiccup) before it could verify and
write this packet. Verification below was performed by the Lead directly.

## ARTIFACTS

deploy/alertbridge/alertbridge.py
deploy/alertbridge/test_dispositions.py   (new)
dashboard/api/lib/Operator.php            (requireAdmin)
dashboard/api/routes/assets.php           (lifecycle/purge/criticality routes + GET emits, feature-detected)
dashboard/api/routes/control.php          (node criticality route)
dashboard/api/routes/discovery.php        (§8 A-2 tombstone join, feature-detected)
dashboard/api/routes/nodes.php            (GET emits criticality, feature-detected)

## VERIFIED (by Lead, on disk and live)

- php -l clean: Operator.php, assets.php, control.php, discovery.php, nodes.php.
- python3 -m py_compile deploy/alertbridge/alertbridge.py — OK.
- python3 deploy/alertbridge/test_dispositions.py — 4/4 pass (A11
  crash-between-disposition-and-checkpoint, A12 cleared-inherits-disposition,
  A13 audit-always-suppressed, legacy-NULL behavior). [Lane-verified 02:0x;
  invocation retained for the review round.]
- LIVE, pre-migration (the binding safety property): GET /api/nodes → 200,
  GET /api/assets → 200, new fields absent-not-error; discovery list 200 with
  tombstone omitted. information_schema feature-detect present in
  assets.php:254, nodes.php:239.

## UNVERIFIED

- No live MQ publish exercised (benzene/RabbitMQ was down at close-out).
- New PHP routes return bridge errors until LC1's C verbs deploy — shapes
  code-reviewed only.
- Post-migration behavior (fields present, tombstone join emitting) untested —
  belongs to the acceptance battery after 018 applies.

## DEVIATIONS

- panel.php tier-weighting: AUTHORED by this lane, caused a live 500 on
  /api/panel for ~5 hours (queried migration-018 columns with no
  feature-detect on a live-served file). REVERTED by Lead 2026-08-06 02:0x;
  patch preserved (session scratchpad, lc2-panel-tier-weighting.patch) for
  Lead re-application WITH guards after migration 018. panel.php is
  Lead-owned for the remainder of this build.
- Packet written by Lead, not the lane wrapper (wrapper lost to storm; work
  complete on disk). Attribution: all feature code authored by gpt-5.6-codex.
