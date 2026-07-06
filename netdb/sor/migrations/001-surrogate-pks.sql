-- Migration 001 — every table gets an integer AUTO_INCREMENT surrogate PK, and
-- name-identity uniqueness is relaxed to plain indexes so name collisions are a
-- software concern (the dashboard), not a DB-level hard failure. Applied live
-- to `sor` on cesium 2026-07-06 (replicates to the benzene mirror).
-- Live schema = schema.sql + this migration.

-- 1. surrogate PKs on the 3 subtype/1:1 tables (were PRIMARY KEY(entity_id))
ALTER TABLE `applications`     DROP PRIMARY KEY, ADD COLUMN `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST, ADD UNIQUE KEY `uq_applications_entity` (`entity_id`);
ALTER TABLE `services`         DROP PRIMARY KEY, ADD COLUMN `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST, ADD UNIQUE KEY `uq_services_entity` (`entity_id`);
ALTER TABLE `monitoring_state` DROP PRIMARY KEY, ADD COLUMN `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST, ADD UNIQUE KEY `uq_monitoring_state_entity` (`entity_id`);

-- 2. relax name-identity UNIQUE -> plain index (collisions handled in software)
ALTER TABLE `entities`          DROP INDEX `uq_entities_name`,           ADD INDEX `idx_entities_name` (`name`,`deleted_at`);
ALTER TABLE `networks`          DROP INDEX `uq_networks_name`,           ADD INDEX `idx_networks_name` (`name`,`deleted_at`);
ALTER TABLE `dns_zones`         DROP INDEX `uq_dns_zones_name`,          ADD INDEX `idx_dns_zones_name` (`name`,`deleted_at`);
ALTER TABLE `secrets`           DROP INDEX `uq_secrets_name`,            ADD INDEX `idx_secrets_name` (`name`,`deleted_at`);
ALTER TABLE `operating_systems` DROP INDEX `uq_os_name_version`,         ADD INDEX `idx_os_name_version` (`name`,`version`);
ALTER TABLE `groups`            DROP INDEX `uq_groups_name_realm`,       ADD INDEX `idx_groups_name_realm` (`name`,`realm`,`deleted_at`);
ALTER TABLE `repositories`      DROP INDEX `uq_repositories_owner_name`, ADD INDEX `idx_repositories_owner_name` (`owner_name`,`name`,`deleted_at`);
ALTER TABLE `locations`         DROP INDEX `uq_locations_parent_name`,   ADD INDEX `idx_locations_parent_name` (`parent_id`,`name`);
ALTER TABLE `interfaces`        DROP INDEX `uq_interfaces_entity_name`,  ADD INDEX `idx_interfaces_entity_name` (`entity_id`,`name`,`deleted_at`);
-- Compound integrity keys kept intentionally (not "name" collisions): ip_addresses,
-- subnets(cidr), vlans(tag), dns_records, group_members, cert_bindings.
