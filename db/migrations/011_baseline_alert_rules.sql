-- 011_baseline_alert_rules.sql - seed the FIRST probe-scope alert rule.
--
-- Context (2026-07-07 incident review): the alertRule table was completely
-- empty -- the C alert engine (serverAlert.c) had run with zero rules, so
-- nothing ever alerted even for metrics it could see. Migration 010 added the
-- host-local health rules; this adds the highest-value, lowest-false-positive
-- probe rule: any monitored service unreachable for a sustained 2 minutes.
--
-- 'reachable' = 1.0 when a probe's outcome is PROBE_OK, else 0.0 (serverAlert.c
-- alertProbeMetric). op 'lt' threshold 1 => reachable == 0 => the target is
-- down. forSeconds=120 requires it to stay down ~2 min so a single blip does
-- not page. Idempotent via the uq_alertrule_scope_metric key from migration 010.
--
-- Resource thresholds (diskFreeMinKb, swapUsedPct, cpuAvgMilli) are intentionally
-- NOT seeded here: they need per-fleet tuning to avoid noise. Add them once you
-- know each host's normal baseline. Available host metrics: cpuAvgMilli,
-- diskFreeMinKb, swapUsedKb/swapUsedPct, plus the health.* signals from mig 010.

INSERT INTO alertRule (scope, metric, op, threshold, forSeconds, severity, enabled) VALUES
  ('probe', 'reachable', 'lt', 1, 120, 'warn', TRUE)
ON DUPLICATE KEY UPDATE
  op=VALUES(op), threshold=VALUES(threshold), forSeconds=VALUES(forSeconds),
  severity=VALUES(severity), enabled=VALUES(enabled);
