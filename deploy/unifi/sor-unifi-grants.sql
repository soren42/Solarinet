-- Least-privilege SoR access for the Xenon UniFi network importer.
-- Create 'solarinet_unifi_import'@'10.0.0.20' separately with a unique secret
-- kept outside git, then apply these grants on the writable SoR primary.
-- The importer soft-deletes source-owned rows; it never needs DELETE, DDL,
-- TRIGGER, global, or schema-wide privileges. CDC triggers execute with their
-- definer privileges and publish through the existing transactional outbox.

GRANT SELECT ON `sor`.`sources` TO 'solarinet_unifi_import'@'10.0.0.20';
GRANT SELECT ON `sor`.`entities` TO 'solarinet_unifi_import'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`external_refs` TO 'solarinet_unifi_import'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`vlans` TO 'solarinet_unifi_import'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`networks` TO 'solarinet_unifi_import'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`subnets` TO 'solarinet_unifi_import'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`dhcp_ranges` TO 'solarinet_unifi_import'@'10.0.0.20';
