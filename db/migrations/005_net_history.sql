-- 005_net_history.sql — add aggregate network throughput to host history.
--
-- The agent already reports per-interface rxKbps/txKbps, but hostHistory had no
-- scalar network column, so the dashboard "Net" trend could never fill. Add a
-- single aggregate (sum of rx+tx across ifaces, Kbps) written per CLIENT_REPORT.
ALTER TABLE hostHistory
  ADD COLUMN netKbps BIGINT UNSIGNED NULL AFTER diskMinFreePct;
