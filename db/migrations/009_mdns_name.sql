-- 009_mdns_name.sql - additive discovery mDNS friendly-name field.
--
-- mDNS enrichment is best-effort and optional at runtime. This nullable column
-- keeps the original UNIQUE(ip, kind) discovery identity unchanged.

ALTER TABLE discovered
  ADD COLUMN mdnsName VARCHAR(128) NULL;
