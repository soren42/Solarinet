-- Runtime-only grants for the three SoR sync worker identities.
-- Create each account out of band with a unique secret and no privileges
-- before applying this file. Replace the database name or account host only
-- for an isolated test. This does not alter the legacy `solari` account,
-- global grants, or accounts not named below.

GRANT SELECT ON `sor`.`dns_zones`
  TO 'solari_sor_ro'@'10.0.0.0/255.0.0.0';
GRANT SELECT ON `sor`.`v_dns_hosts`
  TO 'solari_sor_ro'@'10.0.0.0/255.0.0.0';
GRANT SELECT ON `sor`.`v_dns_ifaces`
  TO 'solari_sor_ro'@'10.0.0.0/255.0.0.0';
GRANT SELECT ON `sor`.`v_dns_cnames`
  TO 'solari_sor_ro'@'10.0.0.0/255.0.0.0';

GRANT SELECT ON `sor`.`sor_outbox`
  TO 'solari_sor_emit'@'10.0.0.0/255.0.0.0';
GRANT SELECT, INSERT, UPDATE ON `sor`.`sor_sync_state`
  TO 'solari_sor_emit'@'10.0.0.0/255.0.0.0';

GRANT SELECT ON `sor`.`sources`
  TO 'solari_sor_discovery'@'10.0.0.0/255.0.0.0';
GRANT SELECT ON `sor`.`ip_addresses`
  TO 'solari_sor_discovery'@'10.0.0.0/255.0.0.0';
GRANT INSERT ON `sor`.`entities`
  TO 'solari_sor_discovery'@'10.0.0.0/255.0.0.0';
GRANT INSERT ON `sor`.`ip_addresses`
  TO 'solari_sor_discovery'@'10.0.0.0/255.0.0.0';

GRANT SELECT ON `sor`.`sources`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
GRANT SELECT ON `sor`.`entities`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
GRANT SELECT, INSERT, UPDATE ON `sor`.`vlans`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
GRANT SELECT, INSERT, UPDATE ON `sor`.`networks`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
GRANT SELECT, INSERT, UPDATE ON `sor`.`subnets`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
GRANT SELECT, INSERT, UPDATE ON `sor`.`dhcp_ranges`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
GRANT SELECT, INSERT, UPDATE ON `sor`.`external_refs`
  TO 'solari_sor_unifi'@'10.0.0.0/255.0.0.0';
