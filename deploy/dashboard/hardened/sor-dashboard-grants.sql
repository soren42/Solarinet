-- Dashboard-only privileges for the SoR on Cesium.
--
-- Create 'solari_dashboard'@'10.0.0.20' separately with a unique secret kept
-- outside git, then apply this file on the writable SoR primary. This role is
-- limited to the tables and views directly used by dashboard SoR routes. It is
-- not the SCP server account and has no global/schema-level privileges.

GRANT SELECT ON `sor`.`sources` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`entities` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`entity_hardware_unit_links` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`ip_addresses` TO 'solari_dashboard'@'10.0.0.20';

GRANT SELECT, INSERT ON `sor`.`hardware_models` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`hardware_units` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`locations` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`projects` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`project_allocations` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`receipts` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`barcodes` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT ON `sor`.`barcode_scans` TO 'solari_dashboard'@'10.0.0.20';

GRANT SELECT, UPDATE ON `sor`.`networks` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`interconnects` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`rw_plans` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, DELETE ON `sor`.`rw_plan_versions` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT, INSERT, UPDATE ON `sor`.`rw_model_groups` TO 'solari_dashboard'@'10.0.0.20';

-- Entity-link edits record provenance in the same transaction; append-only.
-- external_refs remains denied; the one-time scanner import is retired.
GRANT INSERT ON `sor`.`audit_log` TO 'solari_dashboard'@'10.0.0.20';

GRANT SELECT ON `sor`.`interfaces` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`vlans` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`subnets` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`dhcp_ranges` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`dns_zones` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`dns_records` TO 'solari_dashboard'@'10.0.0.20';

GRANT SELECT ON `sor`.`v_dns_hosts` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`v_dns_ifaces` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`v_dns_cnames` TO 'solari_dashboard'@'10.0.0.20';
GRANT SELECT ON `sor`.`v_project_shopping` TO 'solari_dashboard'@'10.0.0.20';
