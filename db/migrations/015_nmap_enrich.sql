-- 015_nmap_enrich.sql - active-recon nmap enrichment fields.
--
-- Companion to 007_discovery_enrichment.sql's mac/vendor/osName/sysDescr:
-- deploy/discovery/nmap_enrich.py runs an authorized `nmap -sV -O` sweep
-- against candidate `discovered` rows (mostly the bare-IPv4, no-other-data
-- ones) and stores the parsed OS guess, open-port/service summary, and
-- banner/header text it collects. Best-effort/optional at runtime, so
-- nullable and additive -- the existing UNIQUE(ip, kind) discovery identity
-- is unchanged.

ALTER TABLE discovered
  ADD COLUMN IF NOT EXISTS openPorts TEXT NULL,
  ADD COLUMN IF NOT EXISTS osGuess VARCHAR(255) NULL,
  ADD COLUMN IF NOT EXISTS banners TEXT NULL,
  ADD COLUMN IF NOT EXISTS nmapEnrichedAt DATETIME NULL;
