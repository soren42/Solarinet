-- GENERATED FILE: tools/export_schema.py
-- Source: sor migrations in tools/migrate.py execution order.
-- The migration runner, not this snapshot, is authoritative for installs.
-- Regenerate with: python3 tools/export_schema.py --database sor
-- Applied migration manifest:
--   schema sha256=342099f0561de203a312a2483bd84c000632e1f9ffc87e3c2d13992a62f8e6b1
--   001-surrogate-pks sha256=ec3411c35b863b300c5117782c45f6161d9e123333be010373bc7282bd86d77d
--   014_inventory sha256=036759e95e5550b6c7ec369a6e780bf81739c964debf19262dfd7bdecad2456f
--   015_barcodes sha256=f65fb022e7806c835cac2deb003bb65ad30a30de634ca9a6314bc973c2d5b0a4
--   020_rackwire sha256=e425533919188e4cedddabc88a3e9c8c08d5cb9cbb6d7f46c53f6d58b925df9d
--   021_provisioning_taxonomy sha256=9ee25baa436bd80406d3cbf261c611cdfe4c044b5df14640e5e1899a00f8b106
--   022_transactional_outbox sha256=86239cb34615f50e48bc7b7629df496ea399d78196d647b4d4ea72dbc67cdc7a
--   023_sor_cdc_triggers sha256=50577d58e2735bc332df7565e4fd3ccffd9aba71791fa7f73499043e16c6cf5e
--   024_provisioning_cdc sha256=89880eeb87e758fcee54ebd11d1b380e9d1e883306bce552374e7a3abcbbbf49
--   025_network_allocator_gate sha256=34d2c5fb57984dbadc9cdaddc4979fb31f69b647c2c45f44ce04f4c3f3c9d2a0
--   026_unifi_network_cdc sha256=1c6c092d08783729c506edb8ae41dd22f0bb21b19d299b124ce4822b174901dc
--   027_interconnect_cable_details sha256=0ed1ed961fe13366195191a2038df5468f8ca75f5865a6b8963e0b72d07bb0e3
--   028_entity_unit_association sha256=35f30382d3bede4cb98165b2bd61d77c96da5bea873a6a984f7faf497b4760db

/*M!999999\- enable the sandbox mode */

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*M!100616 SET @OLD_NOTE_VERBOSITY=@@NOTE_VERBOSITY, NOTE_VERBOSITY=0 */;
DROP TABLE IF EXISTS `applications`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `applications` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'PK = FK to entities; 1:1 subtype row',
  `vendor` varchar(128) DEFAULT NULL COMMENT 'publisher/project, e.g. Forgejo, Pi-hole, Grafana Labs',
  `version` varchar(64) DEFAULT NULL COMMENT 'currently deployed version string',
  `upstream_url` varchar(255) DEFAULT NULL COMMENT 'project homepage / release feed',
  `install_kind` enum('package','container','binary','source','saas','firmware','other') NOT NULL DEFAULT 'other' COMMENT 'how it is deployed (informs upgrade procedure)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_applications_entity` (`entity_id`),
  KEY `fk_applications_source` (`source_id`),
  CONSTRAINT `fk_applications_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_applications_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Subtype detail for application entities (installed software products)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `audit_log`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `audit_log` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK, monotonically increasing',
  `table_name` varchar(64) NOT NULL COMMENT 'SoR table that changed',
  `row_id` bigint(20) unsigned NOT NULL COMMENT 'PK of the changed row in table_name',
  `action` enum('insert','update','soft_delete','undelete','hard_delete') NOT NULL COMMENT 'what happened',
  `actor_user_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: the logged-in user, for dashboard/human changes',
  `actor_label` varchar(128) DEFAULT NULL COMMENT 'free-text actor for machine changes, e.g. sync:unifi, seed:netdb',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'FK: source system on whose behalf the change was made',
  `asserted_kind` enum('human','machine') NOT NULL COMMENT 'human- or machine-driven change',
  `old_values` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL COMMENT 'changed columns, previous values (NULL on insert)' CHECK (json_valid(`old_values`)),
  `new_values` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL COMMENT 'changed columns, new values (NULL on delete)' CHECK (json_valid(`new_values`)),
  `note` varchar(255) DEFAULT NULL COMMENT 'optional operator-supplied change reason',
  `changed_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'when the change was recorded',
  PRIMARY KEY (`id`),
  KEY `ix_audit_row` (`table_name`,`row_id`,`changed_at`),
  KEY `ix_audit_changed_at` (`changed_at`),
  KEY `ix_audit_actor` (`actor_user_id`),
  KEY `fk_audit_source` (`source_id`),
  CONSTRAINT `fk_audit_actor` FOREIGN KEY (`actor_user_id`) REFERENCES `users` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_audit_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Append-only who/what/when/old->new change history; written by the application layer on every write';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `barcode_scans`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `barcode_scans` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `code` varchar(128) NOT NULL COMMENT 'raw payload as scanned',
  `barcode_id` bigint(20) unsigned DEFAULT NULL COMMENT 'resolved barcode row, if any',
  `subject_table` varchar(64) DEFAULT NULL COMMENT 'resolved subject snapshot',
  `subject_id` bigint(20) unsigned DEFAULT NULL,
  `action` enum('lookup','create','receive','move','associate','allocate','print','delete') NOT NULL,
  `operator` varchar(64) NOT NULL COMMENT 'authenticated operator (Operator::requireOperator())',
  `detail` text DEFAULT NULL COMMENT 'JSON context for history/undo, e.g. {"fromLocationId":3,"toLocationId":9}',
  `source_id` smallint(5) unsigned NOT NULL,
  `created_at` datetime NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`id`),
  KEY `ix_bscans_code` (`code`),
  KEY `ix_bscans_subject` (`subject_table`,`subject_id`),
  KEY `ix_bscans_ts` (`created_at`),
  KEY `fk_bscans_barcode` (`barcode_id`),
  KEY `fk_bscans_source` (`source_id`),
  CONSTRAINT `fk_bscans_barcode` FOREIGN KEY (`barcode_id`) REFERENCES `barcodes` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_bscans_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Append-only audit log: one row per scan-driven action';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `barcodes`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `barcodes` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `code` varchar(128) NOT NULL COMMENT 'scanned payload: AKO-0042, LOC-0007, PRJ-0003, factory serial, UPC…',
  `code_type` enum('asset_tag','serial','upc','location','project','port','custom') NOT NULL DEFAULT 'custom',
  `symbology` enum('qr','code128','both') NOT NULL DEFAULT 'both',
  `subject_table` varchar(64) NOT NULL COMMENT 'hardware_units|locations|projects|interfaces (polymorphic, mirrors external_refs; enforced in the API)',
  `subject_id` bigint(20) unsigned NOT NULL COMMENT 'PK value in subject_table (no FK possible)',
  `label_printed_at` datetime DEFAULT NULL COMMENT 'when a physical label was last generated+confirmed printed',
  `notes` varchar(255) DEFAULT NULL,
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp(),
  `created_at` datetime NOT NULL DEFAULT current_timestamp(),
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_barcodes_code_live` (`code`,`live_flag`),
  KEY `ix_barcodes_subject` (`subject_table`,`subject_id`),
  KEY `ix_barcodes_code` (`code`),
  KEY `fk_barcodes_source` (`source_id`),
  CONSTRAINT `fk_barcodes_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Physical barcode/QR codes bound to SoR subjects (units, locations, projects, interfaces)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificate_bindings`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificate_bindings` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `certificate_id` bigint(20) unsigned NOT NULL COMMENT 'FK: the certificate',
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: entity (host/service) where the cert is installed',
  `cert_usage` enum('tls_server','tls_client','ca_trust','code_signing','other') NOT NULL DEFAULT 'tls_server' COMMENT 'how the certificate is used at this deployment',
  `locator` varchar(255) DEFAULT NULL COMMENT 'where on the entity it lives, e.g. /etc/ssl/private/..., k8s secret name',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_cert_bindings` (`certificate_id`,`entity_id`,`cert_usage`,`deleted_at`),
  KEY `ix_cert_bindings_entity` (`entity_id`),
  KEY `fk_cert_bindings_source` (`source_id`),
  CONSTRAINT `fk_cert_bindings_cert` FOREIGN KEY (`certificate_id`) REFERENCES `certificates` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_cert_bindings_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_cert_bindings_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='M:N deployment map: which entities serve/trust which certificates, and where the files live';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificates`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificates` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `subject_cn` varchar(255) NOT NULL COMMENT 'subject common name',
  `sans` text DEFAULT NULL COMMENT 'subjectAltNames, comma-separated',
  `issuer` varchar(255) DEFAULT NULL COMMENT 'issuer DN or friendly CA name (Let''s Encrypt, internal CA...)',
  `serial_hex` varchar(64) DEFAULT NULL COMMENT 'certificate serial number in hex',
  `fingerprint_sha256` char(64) NOT NULL COMMENT 'SHA-256 fingerprint, lowercase hex, no colons; the identity key',
  `key_algo` varchar(32) DEFAULT NULL COMMENT 'public key algorithm + size, e.g. RSA-2048, ECDSA-P256',
  `not_before` datetime DEFAULT NULL COMMENT 'validity start (UTC)',
  `not_after` datetime DEFAULT NULL COMMENT 'validity end (UTC); drives expiry alerting',
  `is_ca` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = CA certificate',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_certificates_fp` (`fingerprint_sha256`),
  KEY `ix_certificates_not_after` (`not_after`),
  KEY `ix_certificates_cn` (`subject_cn`),
  KEY `fk_certificates_source` (`source_id`),
  CONSTRAINT `fk_certificates_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='X.509 certificate metadata; the cert/key material itself lives on the entities that use it';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `config_items`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `config_items` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: entity the setting applies to',
  `scope` varchar(128) NOT NULL DEFAULT '' COMMENT 'component/namespace within the entity, e.g. sshd, bind, solari-agent; empty = entity-global',
  `cfg_key` varchar(191) NOT NULL COMMENT 'configuration key, dotted style, e.g. dns.forwarders, agent.interval_s',
  `cfg_value` text DEFAULT NULL COMMENT 'value serialized as text/JSON; NULL only when secret_id is set',
  `value_type` enum('string','int','float','bool','json','secret_ref') NOT NULL DEFAULT 'string' COMMENT 'how to parse cfg_value; secret_ref means the value is the secret referenced by secret_id',
  `secret_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: when the value is sensitive, point at the secrets inventory instead of storing it',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note (why this override exists)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_config_items` (`entity_id`,`scope`,`cfg_key`,`deleted_at`),
  KEY `ix_config_items_key` (`cfg_key`),
  KEY `ix_config_items_secret` (`secret_id`),
  KEY `fk_config_items_source` (`source_id`),
  CONSTRAINT `fk_config_items_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_config_items_secret` FOREIGN KEY (`secret_id`) REFERENCES `secrets` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_config_items_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Per-entity (and per-component) configuration key/values; sensitive values point into secrets';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `dhcp_ranges`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `dhcp_ranges` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `subnet_id` int(10) unsigned NOT NULL COMMENT 'FK: prefix the range lives in',
  `range_start` inet6 NOT NULL COMMENT 'first address of the range (inclusive)',
  `range_end` inet6 NOT NULL COMMENT 'last address of the range (inclusive)',
  `kind` enum('dynamic_pool','static_block','excluded') NOT NULL DEFAULT 'dynamic_pool' COMMENT 'dynamic_pool = DHCP hands out; static_block = operator-managed; excluded = never assign',
  `server_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: entity running the DHCP server for this range (chemistry/UDR7)',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_dhcp_ranges_subnet` (`subnet_id`),
  KEY `ix_dhcp_ranges_server` (`server_entity_id`),
  KEY `fk_dhcp_ranges_source` (`source_id`),
  CONSTRAINT `fk_dhcp_ranges_server` FOREIGN KEY (`server_entity_id`) REFERENCES `entities` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dhcp_ranges_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_dhcp_ranges_subnet` FOREIGN KEY (`subnet_id`) REFERENCES `subnets` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='DHCP dynamic pools / static blocks / exclusions inside a subnet (from UniFi)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dhcp_ranges_ai
AFTER INSERT ON `dhcp_ranges` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dhcp_ranges',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dhcp_ranges_au
AFTER UPDATE ON `dhcp_ranges` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dhcp_ranges',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dhcp_ranges_ad
AFTER DELETE ON `dhcp_ranges` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dhcp_ranges',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `dns_records`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `dns_records` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `zone_id` int(10) unsigned NOT NULL COMMENT 'FK: owning zone',
  `name` varchar(255) NOT NULL COMMENT 'owner name relative to zone origin; @ for apex',
  `rrtype` enum('A','AAAA','CNAME','PTR','SRV','TXT','MX','NS','SOA','CAA','DS','DNSKEY') NOT NULL COMMENT 'record type',
  `rdata` varchar(1024) NOT NULL COMMENT 'record data as zone-file text (address, target FQDN, TXT string, SRV fields...)',
  `ttl` int(10) unsigned DEFAULT NULL COMMENT 'per-record TTL; NULL = zone default',
  `priority` smallint(5) unsigned DEFAULT NULL COMMENT 'MX preference / SRV priority when applicable',
  `ip_address_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: for A/AAAA/PTR rows generated from IPAM, the backing address row',
  `target_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: for functional CNAMEs, the entity the alias points at (netdb cnames)',
  `is_generated` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = derivable from ip_addresses/entities (A/PTR); 0 = explicitly asserted (CNAME/TXT/SRV...)',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note (e.g. "legacy alias family devices know")',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_dns_records` (`zone_id`,`name`,`rrtype`,`rdata`(255),`deleted_at`),
  KEY `ix_dns_records_name` (`name`),
  KEY `ix_dns_records_ip` (`ip_address_id`),
  KEY `ix_dns_records_target` (`target_entity_id`),
  KEY `fk_dns_records_source` (`source_id`),
  CONSTRAINT `fk_dns_records_ip` FOREIGN KEY (`ip_address_id`) REFERENCES `ip_addresses` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dns_records_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_dns_records_target` FOREIGN KEY (`target_entity_id`) REFERENCES `entities` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dns_records_zone` FOREIGN KEY (`zone_id`) REFERENCES `dns_zones` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='DNS resource records; DNS zone files become a rendered VIEW of this table';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dns_records_ai AFTER INSERT ON `dns_records` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dns_records',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dns_records_au AFTER UPDATE ON `dns_records` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dns_records',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dns_records_ad AFTER DELETE ON `dns_records` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dns_records',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `dns_zones`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `dns_zones` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name` varchar(255) NOT NULL COMMENT 'zone origin without trailing dot, e.g. akoria.net, 0.1.10.in-addr.arpa, akoria.org',
  `kind` enum('forward','reverse') NOT NULL COMMENT 'forward or in-addr.arpa/ip6.arpa reverse',
  `authority` enum('sor_rendered','external') NOT NULL DEFAULT 'sor_rendered' COMMENT 'sor_rendered = the SoR is authoritative and gen-zones renders it; external = mirrored (e.g. akoria.org on Samba AD)',
  `primary_ns_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: entity running the primary nameserver (xenon for akoria.net, radium for akoria.org)',
  `default_ttl` int(10) unsigned NOT NULL DEFAULT 300 COMMENT 'zone default TTL in seconds',
  `hostmaster` varchar(255) DEFAULT NULL COMMENT 'SOA RNAME, e.g. hostmaster.akoria.net.',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `fk_dns_zones_primary_ns` (`primary_ns_entity_id`),
  KEY `fk_dns_zones_source` (`source_id`),
  KEY `idx_dns_zones_name` (`name`,`deleted_at`),
  CONSTRAINT `fk_dns_zones_primary_ns` FOREIGN KEY (`primary_ns_entity_id`) REFERENCES `entities` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dns_zones_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='DNS zones; sor_rendered zones are generated from this DB, external zones are mirrored in';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dns_zones_ai AFTER INSERT ON `dns_zones` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dns_zones',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dns_zones_au AFTER UPDATE ON `dns_zones` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dns_zones',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_dns_zones_ad AFTER DELETE ON `dns_zones` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dns_zones',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `entities`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `entities` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; referenced everywhere',
  `name` varchar(128) NOT NULL COMMENT 'canonical short name (periodic-table convention), e.g. xenon, cesium; unique among live rows',
  `entity_type` enum('server','workstation','network_gear','appliance','iot_device','peripheral','vm','container','application','service','mobile_device','vehicle','other') NOT NULL COMMENT 'subtype discriminator; application/service rows have detail rows in applications/services',
  `role` varchar(255) DEFAULT NULL COMMENT 'human description of what it does (netdb "role" field)',
  `lifecycle` enum('planned','staged','active','offline','retired','decommissioned','unknown') NOT NULL DEFAULT 'active' COMMENT 'lifecycle state (netdb "status"; reserved names = planned)',
  `hardware_unit_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: physical unit realizing this entity (NULL for apps/services/VMs)',
  `os_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: OS currently installed/running (NULL if N/A or unknown)',
  `location_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: physical location override; usually derived via hardware_unit',
  `owner_user_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK (added post-create): responsible person/service account',
  `description` text DEFAULT NULL COMMENT 'longer free-form description',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_entities_type` (`entity_type`),
  KEY `ix_entities_lifecycle` (`lifecycle`),
  KEY `ix_entities_hw_unit` (`hardware_unit_id`),
  KEY `ix_entities_os` (`os_id`),
  KEY `ix_entities_location` (`location_id`),
  KEY `fk_entities_source` (`source_id`),
  KEY `fk_entities_owner` (`owner_user_id`),
  KEY `idx_entities_name` (`name`,`deleted_at`),
  CONSTRAINT `fk_entities_hw_unit` FOREIGN KEY (`hardware_unit_id`) REFERENCES `hardware_units` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_entities_location` FOREIGN KEY (`location_id`) REFERENCES `locations` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_entities_os` FOREIGN KEY (`os_id`) REFERENCES `operating_systems` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_entities_owner` FOREIGN KEY (`owner_user_id`) REFERENCES `users` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_entities_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Supertype for everything the SoR tracks: servers, devices, appliances, apps, services';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_entities_ai AFTER INSERT ON `entities` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('entities',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER `trg_entities_hu_link_ai`
AFTER INSERT ON `entities` FOR EACH ROW
BEGIN
  IF NEW.hardware_unit_id IS NOT NULL AND NEW.deleted_at IS NULL THEN
    IF EXISTS (
      SELECT 1 FROM entity_hardware_unit_links
       WHERE hardware_unit_id=NEW.hardware_unit_id AND entity_id<>NEW.id
    ) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'physical unit is already linked to another live entity';
    END IF;
    INSERT INTO entity_hardware_unit_links (hardware_unit_id, entity_id)
    VALUES (NEW.hardware_unit_id, NEW.id);
  END IF;
END */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_entities_au AFTER UPDATE ON `entities` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('entities',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER `trg_entities_hu_link_au`
AFTER UPDATE ON `entities` FOR EACH ROW
BEGIN
  IF NOT (OLD.hardware_unit_id <=> NEW.hardware_unit_id)
     OR ((OLD.deleted_at IS NULL) <> (NEW.deleted_at IS NULL)) THEN
    IF NEW.hardware_unit_id IS NOT NULL AND NEW.deleted_at IS NULL
       AND EXISTS (
         SELECT 1 FROM entity_hardware_unit_links
          WHERE hardware_unit_id=NEW.hardware_unit_id AND entity_id<>NEW.id
       ) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'physical unit is already linked to another live entity';
    END IF;
    DELETE FROM entity_hardware_unit_links WHERE entity_id=OLD.id;
    IF NEW.hardware_unit_id IS NOT NULL AND NEW.deleted_at IS NULL THEN
      INSERT INTO entity_hardware_unit_links (hardware_unit_id, entity_id)
      VALUES (NEW.hardware_unit_id, NEW.id);
    END IF;
  END IF;
END */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_entities_ad AFTER DELETE ON `entities` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('entities',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `entity_functions`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `entity_functions` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: the entity carrying the tag',
  `function_id` int(10) unsigned NOT NULL COMMENT 'FK: the function tag',
  `is_primary` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = the entity''s primary tag (drives home network + identity address)',
  `tag_order` smallint(5) unsigned NOT NULL DEFAULT 0 COMMENT 'deterministic layering order for bundle/config resolution (lower first)',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper',
  `primary_flag` bigint(20) unsigned GENERATED ALWAYS AS (if(`is_primary` = 1 and `deleted_at` is null,`entity_id`,NULL)) STORED COMMENT 'holds entity_id for the single live primary tag; UNIQUE enforces one-primary-per-entity',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_entity_functions_live` (`entity_id`,`function_id`,`live_flag`),
  UNIQUE KEY `uq_entity_functions_order` (`entity_id`,`tag_order`,`live_flag`),
  UNIQUE KEY `uq_entity_functions_primary` (`primary_flag`),
  KEY `ix_entity_functions_function` (`function_id`),
  KEY `fk_entity_functions_source` (`source_id`),
  CONSTRAINT `fk_entity_functions_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_entity_functions_function` FOREIGN KEY (`function_id`) REFERENCES `functions` (`id`),
  CONSTRAINT `fk_entity_functions_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_entity_functions_primary` CHECK (`is_primary` in (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Ordered multi-tag membership of entities in the function taxonomy; one live primary tag per entity (plan §5/§10)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_entity_functions_ai
AFTER INSERT ON `entity_functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('entity_functions', NEW.id, 'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_entity_functions_au
AFTER UPDATE ON `entity_functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('entity_functions', NEW.id, 'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_entity_functions_ad
AFTER DELETE ON `entity_functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('entity_functions', OLD.id, 'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `entity_hardware_unit_links`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `entity_hardware_unit_links` (
  `hardware_unit_id` int(10) unsigned NOT NULL,
  `entity_id` bigint(20) unsigned NOT NULL,
  `linked_at` datetime NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`hardware_unit_id`),
  UNIQUE KEY `uq_entity_hardware_unit_entity` (`entity_id`),
  CONSTRAINT `fk_entity_hardware_unit_link_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_entity_hardware_unit_link_unit` FOREIGN KEY (`hardware_unit_id`) REFERENCES `hardware_units` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Current one-to-one association of physical units to SoR entities';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `external_refs`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `external_refs` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'FK: which source system the external id belongs to',
  `subject_table` varchar(64) NOT NULL COMMENT 'SoR table the ref points at, e.g. entities, users, repositories',
  `subject_id` bigint(20) unsigned NOT NULL COMMENT 'PK value of the row in subject_table (polymorphic; no FK possible)',
  `external_id` varchar(255) NOT NULL COMMENT 'the row''s identity in the source system: UniFi device _id, Forgejo repo id, AD objectGUID, monitoring node.id',
  `external_url` varchar(255) DEFAULT NULL COMMENT 'deep link into the source system''s UI, if any',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_external_refs` (`source_id`,`subject_table`,`external_id`,`deleted_at`),
  KEY `ix_external_refs_subject` (`subject_table`,`subject_id`),
  CONSTRAINT `fk_external_refs_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Join table between SoR rows and their native IDs in each source system; the sync engine''s correlation key';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_external_refs_ai
AFTER INSERT ON `external_refs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('external_refs',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_external_refs_au
AFTER UPDATE ON `external_refs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('external_refs',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_external_refs_ad
AFTER DELETE ON `external_refs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('external_refs',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `function_incompatibility`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `function_incompatibility` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `function_lo_id` int(10) unsigned NOT NULL COMMENT 'FK: lower function id of the incompatible pair (canonical: lo < hi)',
  `function_hi_id` int(10) unsigned NOT NULL COMMENT 'FK: higher function id of the incompatible pair',
  `reason` varchar(255) DEFAULT NULL COMMENT 'why the two tags may not coexist',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_function_incompat_live` (`function_lo_id`,`function_hi_id`,`live_flag`),
  KEY `ix_function_incompat_hi` (`function_hi_id`),
  KEY `fk_function_incompat_source` (`source_id`),
  CONSTRAINT `fk_function_incompat_hi` FOREIGN KEY (`function_hi_id`) REFERENCES `functions` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_function_incompat_lo` FOREIGN KEY (`function_lo_id`) REFERENCES `functions` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_function_incompat_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_function_incompat_order` CHECK (`function_lo_id` < `function_hi_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Symmetric incompatible function-tag pairs, stored canonically lo<hi (plan §5)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_function_incompatibility_ai
AFTER INSERT ON `function_incompatibility` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('function_incompatibility', NEW.id, 'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_function_incompatibility_au
AFTER UPDATE ON `function_incompatibility` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('function_incompatibility', NEW.id, 'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_function_incompatibility_ad
AFTER DELETE ON `function_incompatibility` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('function_incompatibility', OLD.id, 'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `functions`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `functions` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `slug` varchar(48) NOT NULL COMMENT 'stable tag handle, e.g. database, auth, web, base; unique among live rows',
  `display_name` varchar(96) NOT NULL COMMENT 'operator-facing label',
  `network_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: home segment a host with this primary tag allocates from; NULL = segment-agnostic (base)',
  `criticality` tinyint(3) unsigned NOT NULL DEFAULT 3 COMMENT '5-tier criticality (1=highest .. 5=lowest); mirrors the criticality subsystem',
  `package_bundle` varchar(64) DEFAULT NULL COMMENT 'package-bundle handle the LSV subsystem resolves; NULL = same as slug',
  `config_profile` varchar(64) DEFAULT NULL COMMENT 'config-profile handle the configure subsystem resolves; NULL = same as slug',
  `description` varchar(255) DEFAULT NULL COMMENT 'human note on what the tag means',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_functions_slug_live` (`slug`,`live_flag`),
  KEY `ix_functions_network` (`network_id`),
  KEY `fk_functions_source` (`source_id`),
  CONSTRAINT `fk_functions_network` FOREIGN KEY (`network_id`) REFERENCES `networks` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_functions_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_functions_criticality` CHECK (`criticality` between 1 and 5)
) ENGINE=InnoDB AUTO_INCREMENT=8 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Closed function-tag taxonomy: drives IP allocation, package bundle, DNS, config, criticality (plan §5)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_functions_ai
AFTER INSERT ON `functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('functions', NEW.id, 'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_functions_au
AFTER UPDATE ON `functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('functions', NEW.id, 'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_functions_ad
AFTER DELETE ON `functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('functions', OLD.id, 'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `group_members`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `group_members` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `group_id` bigint(20) unsigned NOT NULL COMMENT 'FK: the containing group',
  `member_user_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: user member (exactly one of member_user_id/member_group_id set)',
  `member_group_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: nested-group member (AD supports group nesting)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_group_members_user` (`group_id`,`member_user_id`,`deleted_at`),
  UNIQUE KEY `uq_group_members_group` (`group_id`,`member_group_id`,`deleted_at`),
  KEY `ix_group_members_user` (`member_user_id`),
  KEY `ix_group_members_group` (`member_group_id`),
  KEY `fk_group_members_source` (`source_id`),
  CONSTRAINT `fk_group_members_group` FOREIGN KEY (`group_id`) REFERENCES `groups` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_group_members_nested` FOREIGN KEY (`member_group_id`) REFERENCES `groups` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_group_members_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_group_members_user` FOREIGN KEY (`member_user_id`) REFERENCES `users` (`id`) ON DELETE CASCADE,
  CONSTRAINT `ck_group_members_one_kind` CHECK (`member_user_id` is null <> (`member_group_id` is null))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Group membership junction: users in groups, and nested groups in groups';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `groups`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `groups` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name` varchar(128) NOT NULL COMMENT 'group name (sAMAccountName for AD groups)',
  `realm` varchar(128) NOT NULL DEFAULT 'akoria.org' COMMENT 'akoria.org (AD) or local',
  `kind` enum('security','distribution','role','local') NOT NULL DEFAULT 'security' COMMENT 'AD group type, or role = SolariNet-internal RBAC group',
  `ad_guid` char(36) DEFAULT NULL COMMENT 'AD objectGUID',
  `ad_sid` varchar(64) DEFAULT NULL COMMENT 'AD objectSid',
  `description` varchar(255) DEFAULT NULL COMMENT 'group purpose',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_groups_ad_guid` (`ad_guid`),
  KEY `fk_groups_source` (`source_id`),
  KEY `idx_groups_name_realm` (`name`,`realm`,`deleted_at`),
  CONSTRAINT `fk_groups_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Security/distribution/role groups; mirrored from Samba AD plus local RBAC roles';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `hardware_models`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `hardware_models` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `make` varchar(128) NOT NULL COMMENT 'manufacturer, e.g. Ubiquiti, Dell, Raspberry Pi',
  `model` varchar(128) NOT NULL COMMENT 'product model, e.g. U7 Pro, PowerEdge R410, CM5',
  `hw_type` enum('server','workstation','sbc','nas','router','switch','ap','printer','scanner','iot','appliance','console','media','vehicle','tablet','phone','kvm','sensor','camera','ups','moca','other') NOT NULL DEFAULT 'other' COMMENT 'coarse device class for filtering and dashboard icons',
  `notes` text DEFAULT NULL COMMENT 'free-form notes (spec links, quirks)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_hw_models_make_model` (`make`,`model`),
  KEY `ix_hw_models_type` (`hw_type`),
  KEY `fk_hw_models_source` (`source_id`),
  CONSTRAINT `fk_hw_models_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Catalog of hardware products (make+model); one row per product, not per unit';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `hardware_units`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `hardware_units` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `model_id` int(10) unsigned NOT NULL COMMENT 'FK: which product this unit is',
  `serial` varchar(128) DEFAULT NULL COMMENT 'manufacturer serial number (unique per model when known)',
  `asset_tag` varchar(64) DEFAULT NULL COMMENT 'internal inventory tag, if labeled',
  `location_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: where this unit physically sits',
  `acquired_on` date DEFAULT NULL COMMENT 'purchase/acquisition date',
  `notes` text DEFAULT NULL COMMENT 'free-form notes (warranty, condition)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `quantity` int(11) NOT NULL DEFAULT 1,
  `unit_cost_cents` int(11) DEFAULT NULL,
  `stock_status` enum('in_stock','allocated','deployed','consumed','retired') NOT NULL DEFAULT 'in_stock',
  `receipt_id` int(10) unsigned DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_hw_units_model_serial` (`model_id`,`serial`),
  KEY `ix_hw_units_location` (`location_id`),
  KEY `fk_hw_units_source` (`source_id`),
  KEY `fk_hwunit_receipt` (`receipt_id`),
  CONSTRAINT `fk_hw_units_location` FOREIGN KEY (`location_id`) REFERENCES `locations` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_hw_units_model` FOREIGN KEY (`model_id`) REFERENCES `hardware_models` (`id`),
  CONSTRAINT `fk_hw_units_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_hwunit_receipt` FOREIGN KEY (`receipt_id`) REFERENCES `receipts` (`id`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Individual physical hardware units (one row per serial-numbered box)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `interconnects`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `interconnects` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `a_interface_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: endpoint A interface (preferred endpoint form)',
  `b_interface_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: endpoint B interface (preferred endpoint form)',
  `a_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: endpoint A entity, when the exact interface is unknown',
  `b_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: endpoint B entity, when the exact interface is unknown',
  `link_kind` enum('copper','fiber','moca','wireless','virtual','lldp_adjacency','logical','power') NOT NULL DEFAULT 'copper' COMMENT 'physical medium, or discovered/logical adjacency; power = AC/DC cord (RackWire)',
  `label` varchar(128) DEFAULT NULL COMMENT 'cable label / patch id / SSID for wireless',
  `length_m` decimal(6,2) DEFAULT NULL COMMENT 'cable length in metres, if physical and known',
  `media_standard` varchar(32) DEFAULT NULL COMMENT 'cable or power-cord standard identifier, e.g. cat6, om3, c13c14',
  `jacket_color` varchar(64) DEFAULT NULL COMMENT 'physical cable jacket color, when known',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row (lldp_adjacency rows come from solarinet_monitor)',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_interconnects_a_if` (`a_interface_id`),
  KEY `ix_interconnects_b_if` (`b_interface_id`),
  KEY `ix_interconnects_a_ent` (`a_entity_id`),
  KEY `ix_interconnects_b_ent` (`b_entity_id`),
  KEY `fk_interconnects_source` (`source_id`),
  CONSTRAINT `fk_interconnects_a_ent` FOREIGN KEY (`a_entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_a_if` FOREIGN KEY (`a_interface_id`) REFERENCES `interfaces` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_b_ent` FOREIGN KEY (`b_entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_b_if` FOREIGN KEY (`b_interface_id`) REFERENCES `interfaces` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_interconnects_a` CHECK (`a_interface_id` is not null or `a_entity_id` is not null),
  CONSTRAINT `ck_interconnects_b` CHECK (`b_interface_id` is not null or `b_entity_id` is not null)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Physical cabling and logical/discovered links between interfaces (or entities as fallback)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `interfaces`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `interfaces` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: the entity this interface belongs to',
  `name` varchar(64) NOT NULL COMMENT 'interface name on the device, e.g. eth0, wlan0, br0, Port 7; or netdb iface label (tungsten-alt)',
  `if_kind` enum('physical','wifi','vlan','bridge','bond','loopback','tunnel','virtual','switch_port','other','power_inlet','power_outlet') NOT NULL DEFAULT 'physical' COMMENT 'physical NIC vs logical construct; power_inlet/power_outlet are electrical ports (RackWire)',
  `mac` char(17) DEFAULT NULL COMMENT 'MAC address aa:bb:cc:dd:ee:ff lowercase; netdb hw field for hosts with known MACs',
  `parent_interface_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: underlying interface for VLAN/bridge/bond members',
  `vlan_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: VLAN for vlan-kind subinterfaces or access-mode switch ports',
  `circuit_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: power circuit this port draws from (inlet) or feeds (outlet); NULL for non-power ports',
  `watts_rated` decimal(8,2) DEFAULT NULL COMMENT 'rated draw (inlet) or rated output (outlet) in watts; NULL = unknown/not applicable',
  `poe_class` enum('af','at','bt3','bt4','passive24','passive48') DEFAULT NULL COMMENT 'PoE standard this port delivers or draws; NULL = no PoE',
  `speed_mbps` int(10) unsigned DEFAULT NULL COMMENT 'negotiated or nominal link speed in Mbit/s',
  `mtu` smallint(5) unsigned DEFAULT NULL COMMENT 'configured MTU',
  `is_management` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = the interface used to manage the device',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_interfaces_mac` (`mac`),
  KEY `ix_interfaces_parent` (`parent_interface_id`),
  KEY `ix_interfaces_vlan` (`vlan_id`),
  KEY `fk_interfaces_source` (`source_id`),
  KEY `idx_interfaces_entity_name` (`entity_id`,`name`,`deleted_at`),
  KEY `ix_interfaces_circuit` (`circuit_id`),
  CONSTRAINT `fk_interfaces_circuit` FOREIGN KEY (`circuit_id`) REFERENCES `power_circuits` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_interfaces_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interfaces_parent` FOREIGN KEY (`parent_interface_id`) REFERENCES `interfaces` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_interfaces_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_interfaces_vlan` FOREIGN KEY (`vlan_id`) REFERENCES `vlans` (`id`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Network interfaces (physical NICs, wifi radios, switch ports, VLAN/bridge/bond logicals)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_interfaces_ai AFTER INSERT ON `interfaces` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('interfaces',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_interfaces_au AFTER UPDATE ON `interfaces` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('interfaces',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_interfaces_ad AFTER DELETE ON `interfaces` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('interfaces',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `inventory_snapshots`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `inventory_snapshots` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; the snapshot id a run pins',
  `content` longtext NOT NULL COMMENT 'rendered inventory payload (JSON) at snapshot time',
  `content_hash` char(64) GENERATED ALWAYS AS (sha2(`content`,256)) STORED COMMENT 'sha256 hex of content; generated so it cannot diverge',
  `entity_count` int(10) unsigned NOT NULL DEFAULT 0 COMMENT 'denormalized host count for the list endpoint (no blob read)',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note, e.g. what triggered the snapshot',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  PRIMARY KEY (`id`),
  KEY `ix_inventory_snapshots_hash` (`content_hash`),
  KEY `ix_inventory_snapshots_created` (`created_at`),
  KEY `fk_inventory_snapshots_source` (`source_id`),
  CONSTRAINT `fk_inventory_snapshots_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_inventory_snapshots_json` CHECK (json_valid(`content`))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Append-only immutable snapshots of rendered inventory; a run pins one (plan §11)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `ip_addresses`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `ip_addresses` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `address` inet6 NOT NULL COMMENT 'the IP address (v4 or v6)',
  `subnet_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: containing prefix (kept consistent by application logic)',
  `interface_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: interface holding the address, when known',
  `entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: owning entity; redundant with interface->entity but set when the interface is unknown',
  `assignment` enum('static','dhcp_reservation','dhcp_dynamic','vip','anycast','gateway','reserved','unknown') NOT NULL DEFAULT 'static' COMMENT 'how the address is allocated; reserved = parked for future use (netdb reserved: block)',
  `is_primary` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = the entity''s identity address (netdb hosts.ip; drives the A record and PTR)',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper (added 021)',
  `primary_entity_flag` bigint(20) unsigned GENERATED ALWAYS AS (if(`is_primary` = 1 and `deleted_at` is null,`entity_id`,NULL)) VIRTUAL COMMENT 'entity_id for the single live primary address; UNIQUE = one identity IP/entity (added 021)',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_ip_addr` (`address`,`deleted_at`) COMMENT 'one live owner per address',
  UNIQUE KEY `uq_ip_addr_live` (`address`,`live_flag`),
  UNIQUE KEY `uq_ip_addr_primary_entity` (`primary_entity_flag`),
  KEY `ix_ip_subnet` (`subnet_id`),
  KEY `ix_ip_interface` (`interface_id`),
  KEY `ix_ip_entity` (`entity_id`),
  KEY `ix_ip_assignment` (`assignment`),
  KEY `fk_ip_source` (`source_id`),
  CONSTRAINT `fk_ip_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`),
  CONSTRAINT `fk_ip_interface` FOREIGN KEY (`interface_id`) REFERENCES `interfaces` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_ip_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_ip_subnet` FOREIGN KEY (`subnet_id`) REFERENCES `subnets` (`id`) ON DELETE SET NULL,
  CONSTRAINT `ck_ip_addr_is_primary` CHECK (`is_primary` in (0,1)),
  CONSTRAINT `ck_ip_addr_primary_has_entity` CHECK (`is_primary` = 0 or `deleted_at` is not null or `entity_id` is not null)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='IPAM: allocations, reservations and observed leases; one live row per address';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_ip_addresses_ai AFTER INSERT ON `ip_addresses` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('ip_addresses',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_ip_addresses_au AFTER UPDATE ON `ip_addresses` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('ip_addresses',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_ip_addresses_ad AFTER DELETE ON `ip_addresses` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('ip_addresses',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `locations`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `locations` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `parent_id` int(10) unsigned DEFAULT NULL COMMENT 'containing location; NULL = top-level site',
  `name` varchar(128) NOT NULL COMMENT 'name within the parent, e.g. "Office", "Minirack", "U4"',
  `code` varchar(32) DEFAULT NULL,
  `loc_type` enum('site','building','floor','room','rack','shelf','position','zone','cabinet','drawer','tray','bin','container') NOT NULL,
  `rack_units` smallint(5) unsigned DEFAULT NULL COMMENT 'for racks: total U capacity; for positions: starting U',
  `notes` text DEFAULT NULL COMMENT 'free-form operator notes',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_locations_type` (`loc_type`),
  KEY `fk_locations_source` (`source_id`),
  KEY `idx_locations_parent_name` (`parent_id`,`name`),
  CONSTRAINT `fk_locations_parent` FOREIGN KEY (`parent_id`) REFERENCES `locations` (`id`),
  CONSTRAINT `fk_locations_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Physical location hierarchy (site/room/rack/position) via self-referencing tree';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `monitoring_state`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `monitoring_state` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'PK = FK to entities; one current-state row per entity',
  `status` enum('up','degraded','down','unknown','maintenance','retired') NOT NULL DEFAULT 'unknown' COMMENT 'rolled-up health as last computed by SolariNet monitoring',
  `first_seen` datetime DEFAULT NULL COMMENT 'when monitoring first observed this entity',
  `last_seen` datetime DEFAULT NULL COMMENT 'last time the entity answered a probe / sent a report',
  `last_checked` datetime DEFAULT NULL COMMENT 'last time monitoring evaluated the entity (even if unreachable)',
  `agent_version` varchar(32) DEFAULT NULL COMMENT 'solariMonitor agent version, if agent-enrolled',
  `detail` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL COMMENT 'source-specific detail blob (load, open issues, failing probes)' CHECK (json_valid(`detail`)),
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row (normally solarinet_monitor)',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_monitoring_state_entity` (`entity_id`),
  KEY `ix_monitoring_state_status` (`status`),
  KEY `ix_monitoring_state_last_seen` (`last_seen`),
  KEY `fk_monitoring_state_source` (`source_id`),
  CONSTRAINT `fk_monitoring_state_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_monitoring_state_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Current health per entity, synced from the SolariNet monitoring DB (history stays there)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `networks`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `networks` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name` varchar(64) NOT NULL COMMENT 'segment name, e.g. core, production, IoT, peripherals, personal, netmgmt, security',
  `purpose` varchar(255) DEFAULT NULL COMMENT 'what this segment is for; mirrors the netdb comment groupings',
  `vlan_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: the VLAN carrying this segment, if VLAN-backed',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `enabled` tinyint(1) NOT NULL DEFAULT 1 COMMENT 'source-reported network state; disabled networks are not allocator candidates',
  `management` varchar(32) DEFAULT NULL COMMENT 'upstream management mode, e.g. GATEWAY, SWITCH, UNMANAGED',
  `is_default` tinyint(1) NOT NULL DEFAULT 0 COMMENT 'source marks this as its default network',
  `allocator_eligible` tinyint(1) NOT NULL DEFAULT 0 COMMENT 'explicit operator approval; imports never enable address allocation automatically',
  `allocator_approved_by` varchar(128) DEFAULT NULL COMMENT 'authenticated dashboard operator who last approved allocation',
  `allocator_approved_at` datetime DEFAULT NULL COMMENT 'time of the last operator allocation approval',
  PRIMARY KEY (`id`),
  KEY `ix_networks_vlan` (`vlan_id`),
  KEY `fk_networks_source` (`source_id`),
  KEY `idx_networks_name` (`name`,`deleted_at`),
  KEY `ix_networks_allocator` (`enabled`,`allocator_eligible`,`deleted_at`),
  CONSTRAINT `fk_networks_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_networks_vlan` FOREIGN KEY (`vlan_id`) REFERENCES `vlans` (`id`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Logical network segments (core/production/IoT/...); groups subnets by function';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_networks_ai AFTER INSERT ON `networks` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('networks',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_networks_au AFTER UPDATE ON `networks` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('networks',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_networks_ad AFTER DELETE ON `networks` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('networks',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `operating_systems`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `operating_systems` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `family` enum('linux','bsd','windows','macos','ios','android','rtos','embedded','network_os','other') NOT NULL DEFAULT 'other' COMMENT 'broad OS family for filtering',
  `name` varchar(128) NOT NULL COMMENT 'distribution/product name, e.g. Debian, UniFi OS, HomeKit firmware',
  `version` varchar(64) NOT NULL DEFAULT '' COMMENT 'release/version string, e.g. 13, 24.04, 4.1.13; empty = unknown',
  `eol_on` date DEFAULT NULL COMMENT 'end-of-life date if known (drives upgrade nagging)',
  `notes` text DEFAULT NULL COMMENT 'free-form notes',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_os_family` (`family`),
  KEY `fk_os_source` (`source_id`),
  KEY `idx_os_name_version` (`name`,`version`),
  CONSTRAINT `fk_os_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Operating system catalog; entities reference a (name,version) row';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `power_circuits`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `power_circuits` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `parent_circuit_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: upstream circuit (wall > UPS rail > PDU bank); NULL = utility feed',
  `location_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: where the circuit lands (room/rack); NULL = unplaced',
  `name` varchar(128) NOT NULL COMMENT 'operator label, panel-qualified, e.g. "MAIN-12", "UPS-A/bank1"; unique among live rows',
  `breaker_label` varchar(64) DEFAULT NULL COMMENT 'breaker/fuse designation as printed on the panel',
  `volts` decimal(6,2) DEFAULT NULL COMMENT 'nominal circuit voltage, e.g. 120.00, 240.00',
  `amps` decimal(6,2) DEFAULT NULL COMMENT 'breaker rating in amps; usable continuous load is 80% of this (NEC derate)',
  `phase` enum('single','split','three','dc') NOT NULL DEFAULT 'single' COMMENT 'supply topology',
  `notes` text DEFAULT NULL COMMENT 'free-form operator notes',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_power_circuits_name_live` (`name`,`live_flag`),
  KEY `ix_power_circuits_parent` (`parent_circuit_id`),
  KEY `ix_power_circuits_location` (`location_id`),
  KEY `fk_power_circuits_source` (`source_id`),
  CONSTRAINT `fk_power_circuits_location` FOREIGN KEY (`location_id`) REFERENCES `locations` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_power_circuits_parent` FOREIGN KEY (`parent_circuit_id`) REFERENCES `power_circuits` (`id`),
  CONSTRAINT `fk_power_circuits_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AC/DC branch circuits and PDU/UPS rails as a self-referencing tree (D5)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `project_allocations`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `project_allocations` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT,
  `project_id` int(10) unsigned NOT NULL,
  `hardware_unit_id` int(10) unsigned DEFAULT NULL,
  `hardware_model_id` int(10) unsigned DEFAULT NULL,
  `allocation` enum('needed','committed','installed','returned') NOT NULL DEFAULT 'needed',
  `quantity` int(11) NOT NULL DEFAULT 1,
  `notes` text DEFAULT NULL,
  `source_id` int(11) DEFAULT NULL,
  `created_at` datetime NOT NULL DEFAULT current_timestamp(),
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  `deleted_at` datetime DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_alloc_unit` (`hardware_unit_id`,`allocation`),
  KEY `idx_alloc_proj` (`project_id`),
  KEY `fk_alloc_model` (`hardware_model_id`),
  CONSTRAINT `fk_alloc_model` FOREIGN KEY (`hardware_model_id`) REFERENCES `hardware_models` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_alloc_project` FOREIGN KEY (`project_id`) REFERENCES `projects` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_alloc_unit` FOREIGN KEY (`hardware_unit_id`) REFERENCES `hardware_units` (`id`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_alloc_no_double_commit_ins BEFORE INSERT ON project_allocations
FOR EACH ROW BEGIN
  IF NEW.allocation IN ('committed','installed') AND NEW.hardware_unit_id IS NOT NULL THEN
    IF EXISTS (SELECT 1 FROM project_allocations pa
                WHERE pa.hardware_unit_id = NEW.hardware_unit_id
                  AND pa.allocation IN ('committed','installed')
                  AND pa.deleted_at IS NULL AND pa.project_id <> NEW.project_id) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'hardware_unit already committed to another project';
    END IF;
  END IF;
END */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_alloc_tray_move AFTER INSERT ON project_allocations
FOR EACH ROW BEGIN
  IF NEW.allocation IN ('committed','installed') AND NEW.hardware_unit_id IS NOT NULL THEN
    UPDATE hardware_units hu
      JOIN projects p ON p.id = NEW.project_id
      SET hu.location_id  = COALESCE(p.tray_location_id, hu.location_id),
          hu.stock_status = IF(NEW.allocation='installed','deployed','allocated')
    WHERE hu.id = NEW.hardware_unit_id;
  END IF;
END */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_alloc_no_double_commit_upd BEFORE UPDATE ON project_allocations
FOR EACH ROW BEGIN
  IF NEW.allocation IN ('committed','installed') AND NEW.hardware_unit_id IS NOT NULL THEN
    IF EXISTS (SELECT 1 FROM project_allocations pa
                WHERE pa.hardware_unit_id = NEW.hardware_unit_id
                  AND pa.allocation IN ('committed','installed')
                  AND pa.deleted_at IS NULL AND pa.id <> NEW.id
                  AND pa.project_id <> NEW.project_id) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'hardware_unit already committed to another project';
    END IF;
  END IF;
END */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `projects`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `projects` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(160) NOT NULL,
  `code` varchar(32) DEFAULT NULL,
  `status` enum('planning','active','staged','on_hold','complete','archived') NOT NULL DEFAULT 'planning',
  `tray_location_id` int(10) unsigned DEFAULT NULL,
  `owner_user_id` bigint(20) unsigned DEFAULT NULL,
  `description` text DEFAULT NULL,
  `started_on` date DEFAULT NULL,
  `target_on` date DEFAULT NULL,
  `completed_on` date DEFAULT NULL,
  `source_id` int(11) DEFAULT NULL,
  `asserted_kind` varchar(32) DEFAULT NULL,
  `asserted_at` datetime DEFAULT NULL,
  `created_at` datetime NOT NULL DEFAULT current_timestamp(),
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  `deleted_at` datetime DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_project_code` (`code`),
  KEY `fk_project_tray` (`tray_location_id`),
  KEY `fk_project_owner` (`owner_user_id`),
  CONSTRAINT `fk_project_owner` FOREIGN KEY (`owner_user_id`) REFERENCES `users` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_project_tray` FOREIGN KEY (`tray_location_id`) REFERENCES `locations` (`id`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `receipts`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `receipts` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT,
  `vendor` varchar(128) DEFAULT NULL,
  `purchased_on` date DEFAULT NULL,
  `subtotal_cents` int(11) DEFAULT NULL,
  `currency` char(3) NOT NULL DEFAULT 'USD',
  `order_ref` varchar(96) DEFAULT NULL,
  `file_path` varchar(512) DEFAULT NULL,
  `file_mime` varchar(64) DEFAULT NULL,
  `notes` text DEFAULT NULL,
  `source_id` int(11) DEFAULT NULL,
  `asserted_kind` varchar(32) DEFAULT NULL,
  `asserted_at` datetime DEFAULT NULL,
  `created_at` datetime NOT NULL DEFAULT current_timestamp(),
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  `deleted_at` datetime DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_receipt_purchased` (`purchased_on`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `relationships`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `relationships` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `from_entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: subject of the relationship',
  `to_entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: object of the relationship',
  `rel_type` enum('contains','runs_on','depends_on','member_of','connects_to','backs_up','manages','monitors','replicates_to','peers_with') NOT NULL COMMENT 'read as: FROM <rel_type> TO. e.g. forgejo runs_on cesium; pihole-secondary replicates_to pihole-primary',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note qualifying the edge',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_relationships` (`from_entity_id`,`to_entity_id`,`rel_type`,`deleted_at`),
  KEY `ix_relationships_to` (`to_entity_id`),
  KEY `ix_relationships_type` (`rel_type`),
  KEY `fk_relationships_source` (`source_id`),
  CONSTRAINT `fk_relationships_from` FOREIGN KEY (`from_entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_relationships_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_relationships_to` FOREIGN KEY (`to_entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `ck_relationships_no_self` CHECK (`from_entity_id` <> `to_entity_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Typed entity graph: contains / runs_on / depends_on / member_of / connects_to / ...';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `repositories`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `repositories` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `owner_name` varchar(128) NOT NULL COMMENT 'Forgejo owner (user or org) as text; may also map to users via owner_user_id',
  `name` varchar(128) NOT NULL COMMENT 'repository name',
  `owner_user_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: owning user when the Forgejo owner maps to a directory user',
  `forge_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: the forge service entity hosting the repo (Forgejo on cesium)',
  `clone_url` varchar(255) DEFAULT NULL COMMENT 'canonical clone URL',
  `default_branch` varchar(128) DEFAULT NULL COMMENT 'default branch name',
  `visibility` enum('public','private','internal') NOT NULL DEFAULT 'private' COMMENT 'repo visibility on the forge',
  `is_archived` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = archived on the forge',
  `is_mirror` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = pull/push mirror of an external repo',
  `description` varchar(255) DEFAULT NULL COMMENT 'repo description from the forge',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_repositories_forge` (`forge_entity_id`),
  KEY `ix_repositories_owner_user` (`owner_user_id`),
  KEY `fk_repositories_source` (`source_id`),
  KEY `idx_repositories_owner_name` (`owner_name`,`name`,`deleted_at`),
  CONSTRAINT `fk_repositories_forge` FOREIGN KEY (`forge_entity_id`) REFERENCES `entities` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_repositories_owner_user` FOREIGN KEY (`owner_user_id`) REFERENCES `users` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_repositories_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Git repositories, mirrored from Forgejo (git.akoria.net on cesium)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `rw_model_groups`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `rw_model_groups` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `hardware_model_id` int(10) unsigned NOT NULL COMMENT 'FK: the product this geometry describes',
  `library_id` varchar(64) DEFAULT NULL COMMENT 'RW.DEVICES entry id (e.g. udm-pro-max) so the client merges by id, server wins',
  `groups_json` longtext NOT NULL COMMENT 'HANDOFF §6 groups[] verbatim; RW.layout() input',
  `device_json` longtext DEFAULT NULL COMMENT 'device-level library extras as a JSON object (drawW, budgetW, poeBudgetW, eff, isInternet, isBattery, …)',
  `notes` text DEFAULT NULL COMMENT 'free-form operator notes',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_rw_model_groups_model_live` (`hardware_model_id`,`live_flag`),
  UNIQUE KEY `uq_rw_model_groups_library_live` (`library_id`,`live_flag`),
  KEY `fk_rw_model_groups_source` (`source_id`),
  CONSTRAINT `fk_rw_model_groups_model` FOREIGN KEY (`hardware_model_id`) REFERENCES `hardware_models` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_rw_model_groups_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_rw_model_groups_groups` CHECK (json_valid(`groups_json`)),
  CONSTRAINT `ck_rw_model_groups_device` CHECK (`device_json` is null or json_valid(`device_json`))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='RackWire port geometry + electrical extras per hardware model (D8); one live row per model';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `rw_plan_versions`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `rw_plan_versions` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; also the snapshot id the host API returns',
  `plan_id` bigint(20) unsigned NOT NULL COMMENT 'FK: the plan this snapshot belongs to',
  `version_name` varchar(160) DEFAULT NULL COMMENT 'operator label passed to saveVersion(name); NULL = auto-named by timestamp',
  `plan_json` longtext NOT NULL COMMENT 'full plan payload at snapshot time (not a diff)',
  `created_by` varchar(64) DEFAULT NULL COMMENT 'authenticated operator that took the snapshot',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  PRIMARY KEY (`id`),
  KEY `ix_rw_plan_versions_plan` (`plan_id`,`created_at`),
  KEY `fk_rw_plan_versions_source` (`source_id`),
  CONSTRAINT `fk_rw_plan_versions_plan` FOREIGN KEY (`plan_id`) REFERENCES `rw_plans` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_rw_plan_versions_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_rw_plan_versions_json` CHECK (json_valid(`plan_json`))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Append-only RackWire plan snapshots; cap enforced in the API, not the schema';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `rw_plans`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `rw_plans` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name` varchar(160) NOT NULL COMMENT 'operator-facing plan name; unique among live rows',
  `slug` varchar(96) NOT NULL COMMENT 'url-safe stable handle derived from name; unique among live rows',
  `plan_json` longtext NOT NULL COMMENT 'RackWire.getPlan() payload verbatim: {devices,cables,racks,notes,codeMap}',
  `thumbnail` longtext DEFAULT NULL COMMENT 'optional data: URI preview of the map view, for the plan picker',
  `device_count` int(10) unsigned NOT NULL DEFAULT 0 COMMENT 'denormalized from plan_json for the list endpoint (no blob read)',
  `cable_count` int(10) unsigned NOT NULL DEFAULT 0 COMMENT 'denormalized from plan_json for the list endpoint (no blob read)',
  `updated_by` varchar(64) DEFAULT NULL COMMENT 'authenticated operator that last wrote this plan (Operator::requireOperator())',
  `notes` text DEFAULT NULL COMMENT 'free-form operator notes',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row (normally rackwire)',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag` tinyint(4) GENERATED ALWAYS AS (if(`deleted_at` is null,1,NULL)) STORED COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_rw_plans_name_live` (`name`,`live_flag`),
  UNIQUE KEY `uq_rw_plans_slug_live` (`slug`,`live_flag`),
  KEY `ix_rw_plans_updated` (`updated_at`),
  KEY `fk_rw_plans_source` (`source_id`),
  CONSTRAINT `fk_rw_plans_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_rw_plans_json` CHECK (json_valid(`plan_json`))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='RackWire plans: current state of each named cabling plan (D4)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `secrets`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `secrets` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name` varchar(128) NOT NULL COMMENT 'stable secret name, e.g. forgejo-admin-token, solari-db-password',
  `kind` enum('password','api_token','ssh_key','tls_private_key','shared_key','kerberos_keytab','totp_seed','webhook_secret','other') NOT NULL COMMENT 'what kind of secret this is',
  `fingerprint` varchar(128) DEFAULT NULL COMMENT 'non-reversible identifier: SSH key fingerprint, token prefix, hash; never the material',
  `vault_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: entity where the secret material actually lives (a host, a password manager service...)',
  `locator` varchar(255) DEFAULT NULL COMMENT 'path/reference inside the vault entity, e.g. /root/.mariadb-root-pw, 1Password item id',
  `owner_user_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: responsible user / the account the secret authenticates',
  `used_by_entity_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: entity that consumes the secret, if a single main consumer',
  `rotated_at` datetime DEFAULT NULL COMMENT 'when the secret was last rotated',
  `expires_at` datetime DEFAULT NULL COMMENT 'expiry, if the secret has one (drives rotation alerting)',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_secrets_vault` (`vault_entity_id`),
  KEY `ix_secrets_owner` (`owner_user_id`),
  KEY `ix_secrets_consumer` (`used_by_entity_id`),
  KEY `ix_secrets_expires` (`expires_at`),
  KEY `fk_secrets_source` (`source_id`),
  KEY `idx_secrets_name` (`name`,`deleted_at`),
  CONSTRAINT `fk_secrets_consumer` FOREIGN KEY (`used_by_entity_id`) REFERENCES `entities` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_secrets_owner` FOREIGN KEY (`owner_user_id`) REFERENCES `users` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_secrets_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `fk_secrets_vault` FOREIGN KEY (`vault_entity_id`) REFERENCES `entities` (`id`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Secret INVENTORY only: what exists, where it lives, when it rotates. Material never stored here';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `service_endpoints`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `service_endpoints` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `service_entity_id` bigint(20) unsigned NOT NULL COMMENT 'FK: the service (entities row, usually entity_type=service or application)',
  `ip_address_id` bigint(20) unsigned DEFAULT NULL COMMENT 'FK: specific listening address; NULL = all addresses of the host',
  `protocol` enum('tcp','udp','icmp','http','https','other') NOT NULL DEFAULT 'tcp' COMMENT 'transport/application protocol',
  `port` smallint(5) unsigned DEFAULT NULL COMMENT 'listening port; NULL for portless (icmp)',
  `url` varchar(255) DEFAULT NULL COMMENT 'canonical URL for web endpoints, e.g. https://git.akoria.net',
  `notes` varchar(255) DEFAULT NULL COMMENT 'free-form note',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_service_endpoints` (`service_entity_id`,`ip_address_id`,`protocol`,`port`,`deleted_at`),
  KEY `ix_service_endpoints_ip` (`ip_address_id`),
  KEY `fk_service_endpoints_source` (`source_id`),
  CONSTRAINT `fk_service_endpoints_ip` FOREIGN KEY (`ip_address_id`) REFERENCES `ip_addresses` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_service_endpoints_service` FOREIGN KEY (`service_entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_service_endpoints_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Listening endpoints (proto/port/url) that expose a service; feeds probe targets';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `services`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `services` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `entity_id` bigint(20) unsigned NOT NULL COMMENT 'PK = FK to entities; 1:1 subtype row',
  `service_kind` enum('dns','dhcp','ldap','kerberos','sso','http','git','db','monitoring','ntp','file','print','mqtt','media','automation','vpn','ca','other') NOT NULL DEFAULT 'other' COMMENT 'functional class of the service',
  `is_critical` tinyint(1) NOT NULL DEFAULT 0 COMMENT '1 = homelab-critical (outage pages the operator)',
  `notes` text DEFAULT NULL COMMENT 'free-form notes (SLAs, failover behavior)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_services_entity` (`entity_id`),
  KEY `ix_services_kind` (`service_kind`),
  KEY `fk_services_source` (`source_id`),
  CONSTRAINT `fk_services_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_services_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Subtype detail for service entities (a function offered on the network)';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `sor_outbox`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `sor_outbox` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `table_name` varchar(64) NOT NULL,
  `row_id` bigint(20) unsigned DEFAULT NULL,
  `action` enum('insert','update','delete') NOT NULL,
  `changed_at` timestamp NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `sor_sync_state`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `sor_sync_state` (
  `k` varchar(64) NOT NULL,
  `v` bigint(20) unsigned NOT NULL DEFAULT 0,
  `updated_at` timestamp NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  PRIMARY KEY (`k`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `sources`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `sources` (
  `id` smallint(5) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `slug` varchar(64) NOT NULL COMMENT 'stable machine name, e.g. netdb, samba_ad, bind, forgejo, solarinet_monitor, unifi, pihole, dashboard, manual',
  `display_name` varchar(128) NOT NULL COMMENT 'human-readable name shown in the dashboard',
  `kind` enum('seed','sync','discovery','human') NOT NULL DEFAULT 'sync' COMMENT 'seed=one-time import; sync=recurring bidirectional; discovery=passive detection; human=dashboard/manual edits',
  `endpoint` varchar(255) DEFAULT NULL COMMENT 'where the source lives (URL/host), informational',
  `notes` text DEFAULT NULL COMMENT 'free-form operator notes',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_sources_slug` (`slug`)
) ENGINE=InnoDB AUTO_INCREMENT=12 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Registry of every system allowed to assert facts (provenance targets)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_sources_ai
AFTER INSERT ON `sources` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('sources', NEW.id, 'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_sources_au
AFTER UPDATE ON `sources` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('sources', NEW.id, 'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_sources_ad
AFTER DELETE ON `sources` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('sources', OLD.id, 'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `subnets`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `subnets` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `network_id` int(10) unsigned DEFAULT NULL COMMENT 'FK: logical segment this prefix belongs to',
  `cidr` varchar(64) NOT NULL COMMENT 'prefix in CIDR text, e.g. 10.1.0.0/24 (kept as text; INET6 has no prefix type)',
  `net_address` inet6 NOT NULL COMMENT 'network address parsed out for range queries',
  `prefix_len` tinyint(3) unsigned NOT NULL COMMENT 'prefix length parsed out for range queries',
  `gateway_ip` inet6 DEFAULT NULL COMMENT 'default gateway inside this prefix, if any',
  `description` varchar(255) DEFAULT NULL COMMENT 'human note about this prefix',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_subnets_cidr` (`cidr`,`deleted_at`),
  KEY `ix_subnets_network` (`network_id`),
  KEY `ix_subnets_netaddr` (`net_address`),
  KEY `fk_subnets_source` (`source_id`),
  CONSTRAINT `fk_subnets_network` FOREIGN KEY (`network_id`) REFERENCES `networks` (`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_subnets_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_subnets_prefix` CHECK (`prefix_len` <= 128)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='IP prefixes (IPAM containers); seeded from UniFi networks + observed /24s';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_subnets_ai AFTER INSERT ON `subnets` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('subnets',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_subnets_au AFTER UPDATE ON `subnets` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('subnets',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_subnets_ad AFTER DELETE ON `subnets` FOR EACH ROW INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('subnets',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
DROP TABLE IF EXISTS `users`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `users` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `username` varchar(128) NOT NULL COMMENT 'sAMAccountName / login name',
  `realm` varchar(128) NOT NULL DEFAULT 'akoria.org' COMMENT 'authentication realm: akoria.org (AD) or local (app-local accounts)',
  `kind` enum('person','service_account','machine_account') NOT NULL DEFAULT 'person' COMMENT 'humans vs service principals vs AD computer accounts',
  `display_name` varchar(255) DEFAULT NULL COMMENT 'full name / friendly name',
  `email` varchar(255) DEFAULT NULL COMMENT 'primary email address',
  `upn` varchar(255) DEFAULT NULL COMMENT 'userPrincipalName from AD',
  `ad_guid` char(36) DEFAULT NULL COMMENT 'AD objectGUID (stable identity across renames)',
  `ad_sid` varchar(64) DEFAULT NULL COMMENT 'AD objectSid',
  `enabled` tinyint(1) NOT NULL DEFAULT 1 COMMENT '0 = account disabled in the source directory',
  `notes` text DEFAULT NULL COMMENT 'free-form notes',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_users_username_realm` (`username`,`realm`,`deleted_at`),
  UNIQUE KEY `uq_users_ad_guid` (`ad_guid`),
  KEY `ix_users_kind` (`kind`),
  KEY `fk_users_source` (`source_id`),
  CONSTRAINT `fk_users_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='People, service accounts, and AD machine accounts; mirrored from Samba AD';
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `v_dns_cnames`;
/*!50001 DROP VIEW IF EXISTS `v_dns_cnames`*/;
SET @saved_cs_client     = @@character_set_client;
SET character_set_client = utf8mb4;
/*!50001 CREATE VIEW `v_dns_cnames` AS SELECT
 1 AS `alias`,
  1 AS `target_host` */;
SET character_set_client = @saved_cs_client;
DROP TABLE IF EXISTS `v_dns_hosts`;
/*!50001 DROP VIEW IF EXISTS `v_dns_hosts`*/;
SET @saved_cs_client     = @@character_set_client;
SET character_set_client = utf8mb4;
/*!50001 CREATE VIEW `v_dns_hosts` AS SELECT
 1 AS `host`,
  1 AS `ip`,
  1 AS `role`,
  1 AS `lifecycle` */;
SET character_set_client = @saved_cs_client;
DROP TABLE IF EXISTS `v_dns_ifaces`;
/*!50001 DROP VIEW IF EXISTS `v_dns_ifaces`*/;
SET @saved_cs_client     = @@character_set_client;
SET character_set_client = utf8mb4;
/*!50001 CREATE VIEW `v_dns_ifaces` AS SELECT
 1 AS `label`,
  1 AS `ip` */;
SET character_set_client = @saved_cs_client;
DROP TABLE IF EXISTS `v_project_shopping`;
/*!50001 DROP VIEW IF EXISTS `v_project_shopping`*/;
SET @saved_cs_client     = @@character_set_client;
SET character_set_client = utf8mb4;
/*!50001 CREATE VIEW `v_project_shopping` AS SELECT
 1 AS `project_id`,
  1 AS `project`,
  1 AS `hardware_model_id`,
  1 AS `make`,
  1 AS `model`,
  1 AS `hw_type`,
  1 AS `qty_needed` */;
SET character_set_client = @saved_cs_client;
DROP TABLE IF EXISTS `vlans`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `vlans` (
  `id` int(10) unsigned NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `vlan_tag` smallint(5) unsigned NOT NULL COMMENT '802.1Q tag, 1-4094',
  `name` varchar(64) NOT NULL COMMENT 'VLAN name as configured on the UniFi controller',
  `purpose` varchar(255) DEFAULT NULL COMMENT 'what traffic belongs here (core, IoT, mgmt...)',
  `source_id` smallint(5) unsigned NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` enum('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'provenance: when the source last asserted this',
  `created_at` datetime NOT NULL DEFAULT current_timestamp() COMMENT 'row creation time',
  `updated_at` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp() COMMENT 'last row modification',
  `deleted_at` datetime DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_vlans_tag` (`vlan_tag`,`deleted_at`),
  KEY `fk_vlans_source` (`source_id`),
  CONSTRAINT `fk_vlans_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`),
  CONSTRAINT `ck_vlans_tag_range` CHECK (`vlan_tag` between 1 and 4094)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='802.1Q VLANs (seeded from UniFi network config)';
/*!40101 SET character_set_client = @saved_cs_client */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_vlans_ai
AFTER INSERT ON `vlans` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('vlans',NEW.id,'insert') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_vlans_au
AFTER UPDATE ON `vlans` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('vlans',NEW.id,'update') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_uca1400_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
/*!50003 CREATE*/ /*!50003 TRIGGER trg_vlans_ad
AFTER DELETE ON `vlans` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('vlans',OLD.id,'delete') */;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50001 DROP VIEW IF EXISTS `v_dns_cnames`*/;
/*!50001 SET @saved_cs_client          = @@character_set_client */;
/*!50001 SET @saved_cs_results         = @@character_set_results */;
/*!50001 SET @saved_col_connection     = @@collation_connection */;
/*!50001 SET character_set_client      = utf8mb4 */;
/*!50001 SET character_set_results     = utf8mb4 */;
/*!50001 SET collation_connection      = utf8mb4_uca1400_ai_ci */;
/*!50001 CREATE ALGORITHM=UNDEFINED */
/*!50013 SQL SECURITY DEFINER */
/*!50001 VIEW `v_dns_cnames` AS select `r`.`name` AS `alias`,`e`.`name` AS `target_host` from ((`dns_records` `r` join `dns_zones` `z` on(`z`.`id` = `r`.`zone_id` and `z`.`deleted_at` is null)) left join `entities` `e` on(`e`.`id` = `r`.`target_entity_id`)) where `r`.`rrtype` = 'CNAME' and `r`.`deleted_at` is null */;
/*!50001 SET character_set_client      = @saved_cs_client */;
/*!50001 SET character_set_results     = @saved_cs_results */;
/*!50001 SET collation_connection      = @saved_col_connection */;
/*!50001 DROP VIEW IF EXISTS `v_dns_hosts`*/;
/*!50001 SET @saved_cs_client          = @@character_set_client */;
/*!50001 SET @saved_cs_results         = @@character_set_results */;
/*!50001 SET @saved_col_connection     = @@collation_connection */;
/*!50001 SET character_set_client      = utf8mb4 */;
/*!50001 SET character_set_results     = utf8mb4 */;
/*!50001 SET collation_connection      = utf8mb4_uca1400_ai_ci */;
/*!50001 CREATE ALGORITHM=UNDEFINED */
/*!50013 SQL SECURITY DEFINER */
/*!50001 VIEW `v_dns_hosts` AS select `e`.`name` AS `host`,case when cast(`ip`.`address` as char charset utf8mb4) like '::ffff:%.%' then substr(cast(`ip`.`address` as char charset utf8mb4),8) else cast(`ip`.`address` as char charset utf8mb4) end AS `ip`,`e`.`role` AS `role`,`e`.`lifecycle` AS `lifecycle` from (`entities` `e` join `ip_addresses` `ip` on(`ip`.`entity_id` = `e`.`id` and `ip`.`is_primary` = 1 and `ip`.`deleted_at` is null)) where `e`.`deleted_at` is null and `e`.`lifecycle` not in ('retired','decommissioned') */;
/*!50001 SET character_set_client      = @saved_cs_client */;
/*!50001 SET character_set_results     = @saved_cs_results */;
/*!50001 SET collation_connection      = @saved_col_connection */;
/*!50001 DROP VIEW IF EXISTS `v_dns_ifaces`*/;
/*!50001 SET @saved_cs_client          = @@character_set_client */;
/*!50001 SET @saved_cs_results         = @@character_set_results */;
/*!50001 SET @saved_col_connection     = @@collation_connection */;
/*!50001 SET character_set_client      = utf8mb4 */;
/*!50001 SET character_set_results     = utf8mb4 */;
/*!50001 SET collation_connection      = utf8mb4_uca1400_ai_ci */;
/*!50001 CREATE ALGORITHM=UNDEFINED */
/*!50013 SQL SECURITY DEFINER */
/*!50001 VIEW `v_dns_ifaces` AS select concat(`e`.`name`,'-',`i`.`name`) AS `label`,case when cast(`ip`.`address` as char charset utf8mb4) like '::ffff:%.%' then substr(cast(`ip`.`address` as char charset utf8mb4),8) else cast(`ip`.`address` as char charset utf8mb4) end AS `ip` from ((`ip_addresses` `ip` join `interfaces` `i` on(`i`.`id` = `ip`.`interface_id` and `i`.`deleted_at` is null)) join `entities` `e` on(`e`.`id` = `i`.`entity_id` and `e`.`deleted_at` is null)) where `ip`.`deleted_at` is null and `ip`.`is_primary` = 0 */;
/*!50001 SET character_set_client      = @saved_cs_client */;
/*!50001 SET character_set_results     = @saved_cs_results */;
/*!50001 SET collation_connection      = @saved_col_connection */;
/*!50001 DROP VIEW IF EXISTS `v_project_shopping`*/;
/*!50001 SET @saved_cs_client          = @@character_set_client */;
/*!50001 SET @saved_cs_results         = @@character_set_results */;
/*!50001 SET @saved_col_connection     = @@collation_connection */;
/*!50001 SET character_set_client      = utf8mb4 */;
/*!50001 SET character_set_results     = utf8mb4 */;
/*!50001 SET collation_connection      = utf8mb4_uca1400_ai_ci */;
/*!50001 CREATE ALGORITHM=UNDEFINED */
/*!50013 SQL SECURITY DEFINER */
/*!50001 VIEW `v_project_shopping` AS select `pa`.`project_id` AS `project_id`,`p`.`name` AS `project`,`pa`.`hardware_model_id` AS `hardware_model_id`,`hm`.`make` AS `make`,`hm`.`model` AS `model`,`hm`.`hw_type` AS `hw_type`,sum(`pa`.`quantity`) AS `qty_needed` from ((`project_allocations` `pa` join `projects` `p` on(`p`.`id` = `pa`.`project_id` and `p`.`deleted_at` is null)) left join `hardware_models` `hm` on(`hm`.`id` = `pa`.`hardware_model_id`)) where `pa`.`allocation` = 'needed' and `pa`.`hardware_unit_id` is null and `pa`.`deleted_at` is null group by `pa`.`project_id`,`p`.`name`,`pa`.`hardware_model_id`,`hm`.`make`,`hm`.`model`,`hm`.`hw_type` */;
/*!50001 SET character_set_client      = @saved_cs_client */;
/*!50001 SET character_set_results     = @saved_cs_results */;
/*!50001 SET collation_connection      = @saved_col_connection */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*M!100616 SET NOTE_VERBOSITY=@OLD_NOTE_VERBOSITY */;
