-- db/seed/demo.sql — DEMO / DEV fixture data for the SolariNet dashboard.
--
-- This is the SQL parallel of dashboard/public/data.jsx: a small, clearly
-- labelled set of representative rows so the dashboard and API can be exercised
-- against a live MariaDB without a running fleet. NOT production data — every
-- host/IP uses documentation ranges (RFC 5737 / RFC 3849-style) and obvious
-- demo names. Apply after migrations 001 + 002. Idempotent (INSERT IGNORE /
-- ON DUPLICATE KEY UPDATE) so it can be re-run.

-- ---- segments ----
INSERT INTO segment (segId, label, cidr, wireless, notes) VALUES
  ('core',    'Core',     '198.51.100.0/24', FALSE, 'demo core segment'),
  ('compute', 'Compute',  '198.51.101.0/24', FALSE, 'demo compute segment'),
  ('lab',     'Lab WiFi', '198.51.102.0/24', TRUE,  'demo wireless lab')
ON DUPLICATE KEY UPDATE label=VALUES(label), cidr=VALUES(cidr);

-- ---- network gear (gateway -> core switch -> lab AP) ----
INSERT INTO networkGear (gearId, name, kind, model, segId, ports, uplinkGearId, wireless, mgmtIp, lastSeenAt) VALUES
  ('gw',         'Edge Gateway', 'gateway', 'demo-gw',   'core', 8,  NULL,         FALSE, '198.51.100.1', NOW()),
  ('sw-core-01', 'Core Switch',  'switch',  'demo-sw48',  'core', 48, 'gw',         FALSE, '198.51.100.2', NOW()),
  ('ap-lab-01',  'Lab AP',       'ap',      'demo-ap',    'lab',  4,  'sw-core-01', TRUE,  '198.51.102.2', NOW())
ON DUPLICATE KEY UPDATE name=VALUES(name), uplinkGearId=VALUES(uplinkGearId);

-- ---- nodes (a server, a monitor, two clients) across segments/states ----
INSERT INTO node (nodeId, role, hostFqdn, certCn, osName, arch, segId, enrolledAt, lastSeenAt, configEpoch, state, uplinkGearId) VALUES
  (1001, 'server',  'srv-01.demo',  'server:srv-01.demo',  'linux', 'x86_64', 'core',    NOW(), NOW(), 3, 'up',       'sw-core-01'),
  (1002, 'monitor', 'mon-01.demo',  'monitor:mon-01.demo', 'linux', 'aarch64','core',    NOW(), NOW(), 3, 'up',       'sw-core-01'),
  (1003, 'client',  'web-01.demo',  'client:web-01.demo',  'linux', 'x86_64', 'compute', NOW(), NOW(), 3, 'up',       'sw-core-01'),
  (1004, 'client',  'lab-pi.demo',  'client:lab-pi.demo',  'linux', 'armv7l', 'lab',     NOW(), NOW(), 2, 'degraded', 'ap-lab-01')
ON DUPLICATE KEY UPDATE state=VALUES(state), hostFqdn=VALUES(hostFqdn), segId=VALUES(segId);

-- ---- host metrics for the clients/server (current snapshot) ----
INSERT INTO hostCurrent (nodeId, sampledAt, cpuLoadMilli, ramUsedKb, ramTotalKb, swapUsedKb, swapTotalKb)
VALUES
  (1001, NOW(), 180,  6291456, 16777216, 0, 4194304),
  (1003, NOW(), 420,  9437184, 16777216, 131072, 4194304),
  (1004, NOW(), 760,  1572864,  2097152, 524288, 1048576)
ON DUPLICATE KEY UPDATE cpuLoadMilli=VALUES(cpuLoadMilli), ramUsedKb=VALUES(ramUsedKb);

-- ---- probe targets (with §6 label / replFactor / segId) ----
INSERT INTO probeTarget (targetId, host, port, proto, replFactor, label, segId) VALUES
  ('tcp:198.51.100.1:443',  '198.51.100.1', 443, 'tcp',  2, 'Gateway HTTPS', 'core'),
  ('tcp:198.51.101.10:5432','198.51.101.10',5432,'tcp', 2, 'Primary DB',     'compute'),
  ('icmp:198.51.102.2:0',   '198.51.102.2', 0,   'icmp', 1, 'Lab AP ping',    'lab')
ON DUPLICATE KEY UPDATE label=VALUES(label), replFactor=VALUES(replFactor);

-- ---- probe current results (per monitor vantage) for rolled-up state ----
INSERT INTO probeCurrent (targetId, monitorNode, outcome, rttMicros, jitterMicros, lossPermille, throughputKbps, serviceMeta, sampledAt) VALUES
  ('tcp:198.51.100.1:443',   1002, 'ok',      1800,  120, 0,  0, NULL, NOW()),
  ('tcp:198.51.101.10:5432', 1002, 'ok',      2400,  200, 0,  0, NULL, NOW()),
  ('icmp:198.51.102.2:0',    1002, 'timeout', 9500, 1500, 80, 0, NULL, NOW())
ON DUPLICATE KEY UPDATE outcome=VALUES(outcome), rttMicros=VALUES(rttMicros);

-- ---- alert rules ----
INSERT INTO alertRule (ruleId, scope, metric, op, threshold, forSeconds, severity, enabled) VALUES
  (1, 'host',  'cpuMilliAvg', 'gt', 900,  60, 'warn', TRUE),
  (2, 'host',  'ramUsedPct',  'gt', 95,   120,'crit', TRUE),
  (3, 'probe', 'lossPermille','gt', 50,   30, 'warn', TRUE)
ON DUPLICATE KEY UPDATE threshold=VALUES(threshold), enabled=VALUES(enabled);

-- ---- a build artifact row (for /api/builds) ----
INSERT INTO buildArtifact (arch, os, version, channel, sha256, sizeBytes, artifactUri, publishedAt) VALUES
  ('x86_64', 'linux', '0.7.0', 'stable',
   '0000000000000000000000000000000000000000000000000000000000000000',
   1048576, 'file:///srv/solari/artifacts/solari-0.7.0-x86_64.tar.gz', NOW())
ON DUPLICATE KEY UPDATE sizeBytes=VALUES(sizeBytes);
