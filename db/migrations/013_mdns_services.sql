-- 013_mdns_services.sql - compact mDNS advertised-service summary.
--
-- Companion to 009_mdns_name.sql's mdnsName: a comma-joined list of the
-- distinct Avahi/mDNS service types seen advertised by a host, e.g.
-- "_spotify-connect._tcp,_airplay._tcp". Best-effort/optional at runtime
-- (populated by deploy/discovery/avahi_import.py), so nullable and additive —
-- the existing UNIQUE(ip, kind) discovery identity is unchanged.

ALTER TABLE discovered
  ADD COLUMN IF NOT EXISTS mdnsServices VARCHAR(512) NULL;
