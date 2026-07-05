-- 007_discovery_enrichment.sql - additive discovery asset enrichment fields.
--
-- Enrichment is best-effort and optional at runtime. These nullable columns keep
-- the original UNIQUE(ip, kind) discovery identity unchanged.

ALTER TABLE discovered
  ADD COLUMN mac VARCHAR(17) NULL,
  ADD COLUMN vendor VARCHAR(64) NULL,
  ADD COLUMN osName VARCHAR(64) NULL,
  ADD COLUMN deviceRole VARCHAR(24) NULL,
  ADD COLUMN sysDescr VARCHAR(255) NULL,
  ADD COLUMN enrichedAt DATETIME NULL;
