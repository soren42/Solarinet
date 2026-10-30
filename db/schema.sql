-- GENERATED FILE: tools/export_schema.py
-- Source: monitoring migrations in tools/migrate.py execution order.
-- The migration runner, not this snapshot, is authoritative for installs.
-- Regenerate with: python3 tools/export_schema.py --database monitoring
-- Applied migration manifest:
--   001_baseline sha256=3a89d80b6596d314a68b9d85f67a4466bfa26ef3f7f5fac65489c0de36fb349c
--   002_c2_capabilities sha256=b405fcb4b665e9274f2f081c1c72067f37dce2b7d75220dab68271034a5005b8
--   003_config_engine sha256=a4c1a7bba0f37396eb9d6d6e832ab1f5d5396b706206b6e944257375d36f5643
--   004_assets_pools sha256=98f906f5a0b32bc437f8efa389df35cd3cfbbb284aee510b9fdf5d8d6308ffd9
--   005_net_history sha256=38466e24bf8c2b37a5bba07b75e56d016bc27b4e62a17d1ff4ce49dc521be7a5
--   006_snmp_gear sha256=398b9786f4bc35480ec753cbb9169fb8c4de6785ee9e4f05c1722df4e8c599ee
--   007_discovery_enrichment sha256=2bc3ca48df51d7f479ea425fa16054cd848217c99b22920b902068a510c0fef5
--   008_probe_apptype sha256=fff4bdb6e97fa4005c4a8ad3511818ec74cbfb33ce348e8f665a7a2a2176b80b
--   009_mdns_name sha256=f258a415ca920742148a59a7d48942326e8d8150db97d10f4057b4f89bc7f023
--   010_host_health sha256=afbaeea9ed97a5a65e5781de9a125050c650b49d999e9338a7b2d025f9d2630b
--   011_baseline_alert_rules sha256=85619810360fccbe32f4505149069546d0b56b475e1887cbbaa17d892a14ac46
--   012_alert_ack_and_opie sha256=b534db78ccead2b80e1de8b00d47167c839574a0b9e7610eafbcc254553df8c4
--   013_mdns_services sha256=775771b8873519eb294abd219fbc133dc75f031a244894c5a862b2c456f3f897
--   014_maintenance_windows sha256=c1f61b0a86319453725156b3c9da9ffe27ec4832584a5e2283a9adfb91437c6c
--   015_nmap_enrich sha256=fa77de451676dc4ad8c54295188c5d0dd33f08a76aab5bf0d25208d5185aa429
--   016_panel_control sha256=f42df61c64e4a25cefcc35615b0254b5f40a4477e11d95a06b7f7567bb5f0b02
--   016_push_subscriptions sha256=200b9dd03594e534304c129b8dd27ab2e43f9a4c18f04d012dfaba5627e48dda
--   017_panel_dwell sha256=d58453c8583d9c9d227e47278348506f1b1a11f86c19a6f563f2d98b8e657326
--   018_lifecycle_criticality sha256=df0f208264891fbab3d22f18075ec7b6bb3006037b37b315aef95c6cbd298be9
--   019_panel_screen_config sha256=8c5da43ff7ae37daa86fbbd48d5c5575e3481be92f1cb46c1ebaf9e4ce74787b
--   020_ctl_request_queue sha256=41b5a7e138667b33729750cf9bf7d82b31a063a929ab7886b519b5794481bc4f
--   021_report_receipts sha256=9c6dd4cd1d0b1c50a3b2dbcb7c5c9febe3b1bb3c117b2e08163a9d029a278580
--   022_enrollment_issue_commit sha256=041d056e5605539fc63ef5a9d6811e4989e77ed54c9fad356121f3eb0495029e
--   023_enrollment_tokens sha256=452f2744e295653693053b0403ee63ce93235222851b8be5290e5c7c11e28622
--   024_node_certificate_identity sha256=2ebe73ad700dfe5dd8da0c3e3b333a6db53a86a8b23f37a717f1857a6442ad0b
--   025_decommission_challenge sha256=126a2ee27507b11922c78554b99f55fdddf008b1de3165856927457e3fe4ff65
--   026_decommission_ack_generation sha256=87c41ef88e94e365f3a0c6b7c0a32606bb8d14e1060f0f3fcae65de1315bbc7c
--   027_decommission_delivery_retry sha256=fb186f43a7a98809e72cd8c68bd1cb873e6098f2e17e4334d5706dc4eac1fa4a
--   028_discovery_scan_jobs sha256=1daca39d1f272ec92331d3b028d9306f169d5bd40eeca07ef4d5768a3d898b33
--   029_certificate_revocation sha256=3e0aa33bb32b026f16ae1041058d315b553fa995e2067101a440a07c3c3b5eed
--   030_node_leaf_issuer_identity sha256=f544daeac9efc6935864dc940942d7295d62fb07aa5b65536004503979c0c95c
--   031_certificate_issuance_inventory sha256=cf0dab6d904902404512276339d839256850c2c046c736768b7e3b046ba7b26d
--   032_certificate_crl_generation sha256=c558a48754b41c9ba01f31ced46e34614733a9272995f5d8a6e4b5493036923d
--   033_redact_queued_decommission_tokens sha256=05964bd1791f558a3b2b00aa4c95767e50f03266dd71edbe1e22d2c10ff20344
--   034_pending_control_directives sha256=8db3d2344579412603ad9636d762950ecb327ab052d392b96032acaaa1e751c8
--   035_certificate_crl_acknowledgements sha256=537b56d27b9101989735e42793edfcbcecc2af30d9bb2c530ec36f15884d3c70
--   036_crl_acknowledgement_node_cascade sha256=a6c7462b422fcf39137f4c585934ac620963c7384e22b614e90d08c4f41c4ffc
--   037_node_certcn_role_prefix sha256=ef27b40ecc5d5300d43d8885fc5db5f31eccce703ebafe30382446f0df8f9eb0

/*M!999999\- enable the sandbox mode */

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*M!100616 SET @OLD_NOTE_VERBOSITY=@@NOTE_VERBOSITY, NOTE_VERBOSITY=0 */;
DROP TABLE IF EXISTS `alertEvent`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `alertEvent` (
  `eventId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `ruleId` int(11) DEFAULT NULL,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `targetId` varchar(128) DEFAULT NULL,
  `firedAt` datetime NOT NULL,
  `clearedAt` datetime DEFAULT NULL,
  `severity` enum('info','warn','crit') DEFAULT NULL,
  `detail` varchar(512) DEFAULT NULL,
  `ackedAt` datetime DEFAULT NULL,
  `ackedBy` varchar(64) DEFAULT NULL,
  `eventKind` enum('fired','cleared','audit') NOT NULL DEFAULT 'fired',
  `openedEventId` bigint(20) unsigned DEFAULT NULL,
  `assetId` bigint(20) unsigned DEFAULT NULL,
  `effectiveTier` tinyint(4) DEFAULT NULL,
  `disposition` enum('publish','suppress') DEFAULT NULL,
  PRIMARY KEY (`eventId`),
  KEY `firedAt` (`firedAt`),
  KEY `nodeId` (`nodeId`),
  KEY `ix_alert_event_asset` (`assetId`),
  KEY `ix_alert_event_opened` (`openedEventId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `alertRule`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `alertRule` (
  `ruleId` int(11) NOT NULL AUTO_INCREMENT,
  `scope` enum('host','probe') NOT NULL,
  `metric` varchar(64) NOT NULL,
  `op` enum('gt','lt','eq','transition') NOT NULL,
  `threshold` double DEFAULT NULL,
  `forSeconds` int(11) DEFAULT NULL,
  `severity` enum('info','warn','crit') DEFAULT NULL,
  `enabled` tinyint(1) NOT NULL DEFAULT 1,
  PRIMARY KEY (`ruleId`),
  UNIQUE KEY `uq_alertrule_scope_metric` (`scope`,`metric`)
) ENGINE=InnoDB AUTO_INCREMENT=7 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `asset`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `asset` (
  `assetId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `ip` varchar(64) NOT NULL,
  `host` varchar(255) DEFAULT NULL,
  `displayName` varchar(128) DEFAULT NULL,
  `class` varchar(32) NOT NULL DEFAULT 'host',
  `poolId` int(10) unsigned DEFAULT NULL,
  `tags` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`tags`)),
  `notes` varchar(512) DEFAULT NULL,
  `monitorHost` tinyint(1) NOT NULL DEFAULT 1,
  `lifecycle` enum('active','decommissioned','deleted') NOT NULL DEFAULT 'active',
  `criticality` tinyint(4) NOT NULL DEFAULT 2,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `createdAt` datetime NOT NULL,
  `updatedAt` datetime NOT NULL,
  PRIMARY KEY (`assetId`),
  UNIQUE KEY `ip` (`ip`),
  KEY `poolId` (`poolId`),
  KEY `class` (`class`),
  KEY `ix_asset_lifecycle` (`lifecycle`),
  CONSTRAINT `fk_asset_pool` FOREIGN KEY (`poolId`) REFERENCES `pool` (`poolId`) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `buildArtifact`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `buildArtifact` (
  `buildId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `arch` varchar(16) NOT NULL,
  `os` varchar(16) NOT NULL,
  `version` varchar(32) NOT NULL,
  `channel` enum('stable','beta','dev') NOT NULL DEFAULT 'stable',
  `sha256` char(64) NOT NULL,
  `sizeBytes` bigint(20) unsigned DEFAULT NULL,
  `artifactUri` varchar(255) DEFAULT NULL,
  `publishedAt` datetime DEFAULT NULL,
  PRIMARY KEY (`buildId`),
  UNIQUE KEY `arch` (`arch`,`os`,`version`,`channel`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificateCrlAcknowledgement`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificateCrlAcknowledgement` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `issuerFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `crlNumber` bigint(20) unsigned NOT NULL,
  `crlFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `acknowledgedAt` datetime(6) NOT NULL,
  PRIMARY KEY (`nodeId`,`issuerFp`),
  KEY `ix_certificateCrlAck_generation` (`issuerFp`,`crlNumber`,`crlFp`),
  CONSTRAINT `fk_certificateCrlAck_node` FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`) ON DELETE CASCADE,
  CONSTRAINT `chk_certificateCrlAck_number` CHECK (`crlNumber` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificateCrlState`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificateCrlState` (
  `issuerFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `nextNumber` bigint(20) unsigned NOT NULL DEFAULT 1,
  `publishedNumber` bigint(20) unsigned DEFAULT NULL,
  `generatedAt` datetime(6) DEFAULT NULL,
  `crlFp` char(64) CHARACTER SET ascii COLLATE ascii_bin DEFAULT NULL,
  `nextUpdate` datetime(6) DEFAULT NULL,
  PRIMARY KEY (`issuerFp`),
  CONSTRAINT `CONSTRAINT_1` CHECK (`nextNumber` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificateInventoryState`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificateInventoryState` (
  `singletonId` tinyint(3) unsigned NOT NULL,
  `complete` tinyint(1) NOT NULL DEFAULT 0,
  `completedAt` datetime(6) DEFAULT NULL,
  `completedBy` varchar(128) DEFAULT NULL,
  PRIMARY KEY (`singletonId`),
  CONSTRAINT `CONSTRAINT_1` CHECK (`singletonId` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificateIssuance`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificateIssuance` (
  `issuerFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `serialHex` varchar(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `leafCertFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `enrollmentId` bigint(20) unsigned DEFAULT NULL,
  `origin` enum('enrollment','operator','backfill') NOT NULL,
  `issuedBy` varchar(128) NOT NULL,
  `issuedAt` datetime(6) DEFAULT NULL,
  `recordedAt` datetime(6) NOT NULL DEFAULT current_timestamp(6),
  PRIMARY KEY (`issuerFp`,`serialHex`),
  UNIQUE KEY `uq_certificateIssuance_leaf` (`leafCertFp`),
  UNIQUE KEY `uq_certificateIssuance_enrollment` (`enrollmentId`),
  KEY `ix_certificateIssuance_node` (`nodeId`,`issuedAt`),
  CONSTRAINT `fk_certificateIssuance_enrollment` FOREIGN KEY (`enrollmentId`) REFERENCES `enrollment` (`enrId`),
  CONSTRAINT `fk_certificateIssuance_node` FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificateRevocation`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificateRevocation` (
  `issuerFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `serialHex` varchar(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `leafCertFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `enrollmentId` bigint(20) unsigned DEFAULT NULL,
  `reason` enum('unspecified','keyCompromise','affiliationChanged','superseded','cessationOfOperation') NOT NULL,
  `revokedBy` varchar(128) NOT NULL,
  `revokedAt` datetime(6) NOT NULL,
  PRIMARY KEY (`issuerFp`,`serialHex`),
  UNIQUE KEY `uq_certificateRevocation_leaf` (`leafCertFp`),
  KEY `ix_certificateRevocation_node` (`nodeId`,`revokedAt`),
  KEY `fk_certificateRevocation_enrollment` (`enrollmentId`),
  CONSTRAINT `fk_certificateRevocation_enrollment` FOREIGN KEY (`enrollmentId`) REFERENCES `enrollment` (`enrId`),
  CONSTRAINT `fk_certificateRevocation_issuance` FOREIGN KEY (`issuerFp`, `serialHex`) REFERENCES `certificateIssuance` (`issuerFp`, `serialHex`),
  CONSTRAINT `fk_certificateRevocation_node` FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `certificateRevocationChallenge`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `certificateRevocationChallenge` (
  `issuerFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `serialHex` varchar(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `leafCertFp` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `enrollmentId` bigint(20) unsigned DEFAULT NULL,
  `reason` enum('unspecified','keyCompromise','affiliationChanged','superseded','cessationOfOperation') NOT NULL,
  `requestedBy` varchar(128) NOT NULL,
  `tokenHash` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `state` enum('pending','consumed','cancelled') NOT NULL DEFAULT 'pending',
  `issuedAt` datetime(6) NOT NULL,
  `expiresAt` datetime(6) NOT NULL,
  `consumedAt` datetime(6) DEFAULT NULL,
  PRIMARY KEY (`issuerFp`,`serialHex`),
  KEY `ix_certificateRevocationChallenge_expiry` (`state`,`expiresAt`),
  KEY `fk_certificateRevocationChallenge_node` (`nodeId`),
  KEY `fk_certificateRevocationChallenge_enrollment` (`enrollmentId`),
  CONSTRAINT `fk_certificateRevocationChallenge_enrollment` FOREIGN KEY (`enrollmentId`) REFERENCES `enrollment` (`enrId`),
  CONSTRAINT `fk_certificateRevocationChallenge_node` FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `ctlRequest`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `ctlRequest` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `verb` varchar(48) NOT NULL,
  `argsWire` varchar(4096) NOT NULL DEFAULT '',
  `requestedBy` varchar(128) NOT NULL,
  `peerUid` int(10) unsigned NOT NULL,
  `state` enum('pending','claimed','done','failed') NOT NULL DEFAULT 'pending',
  `attempts` int(10) unsigned NOT NULL DEFAULT 0,
  `claimedBy` varchar(64) DEFAULT NULL,
  `result` mediumtext DEFAULT NULL,
  `error` varchar(512) DEFAULT NULL,
  `idempotencyKey` varchar(128) DEFAULT NULL,
  `createdAt` datetime(6) NOT NULL DEFAULT current_timestamp(6),
  `updatedAt` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  `claimedAt` datetime(6) DEFAULT NULL,
  `completedAt` datetime(6) DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_ctlRequest_idem` (`requestedBy`,`idempotencyKey`),
  KEY `ix_ctlRequest_pending` (`state`,`id`),
  KEY `ix_ctlRequest_created` (`createdAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `decommissionChallenge`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `decommissionChallenge` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `wipeScope` tinyint(3) unsigned NOT NULL,
  `requestedBy` varchar(128) NOT NULL,
  `tokenHash` char(64) NOT NULL,
  `state` enum('pending','consumed','acknowledged','cancelled') NOT NULL DEFAULT 'pending',
  `issuedAt` datetime(6) NOT NULL,
  `expiresAt` datetime(6) NOT NULL,
  `consumedAt` datetime(6) DEFAULT NULL,
  `generation` bigint(20) unsigned NOT NULL DEFAULT 1,
  `deliveryAttempts` int(10) unsigned NOT NULL DEFAULT 0,
  `lastDeliveryAt` datetime(6) DEFAULT NULL,
  `nextDeliveryAt` datetime(6) DEFAULT NULL,
  PRIMARY KEY (`nodeId`),
  KEY `ix_decommissionChallenge_expiry` (`state`,`expiresAt`),
  KEY `ix_decommissionChallenge_retry` (`state`,`nextDeliveryAt`),
  CONSTRAINT `fk_decommissionChallenge_node` FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `discovered`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `discovered` (
  `discId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `host` varchar(255) DEFAULT NULL,
  `ip` varchar(64) NOT NULL,
  `kind` enum('host','service','network','other') NOT NULL,
  `via` enum('mdns','arp','scp_advert','portscan','lldp') NOT NULL,
  `services` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`services`)),
  `segId` varchar(32) DEFAULT NULL,
  `arch` varchar(16) DEFAULT NULL,
  `seenCount` int(11) NOT NULL DEFAULT 1,
  `firstSeenAt` datetime DEFAULT NULL,
  `lastSeenAt` datetime DEFAULT NULL,
  `status` enum('new','ignored','adopting','adopted') NOT NULL DEFAULT 'new',
  `mac` varchar(17) DEFAULT NULL,
  `vendor` varchar(64) DEFAULT NULL,
  `osName` varchar(64) DEFAULT NULL,
  `deviceRole` varchar(24) DEFAULT NULL,
  `sysDescr` varchar(255) DEFAULT NULL,
  `enrichedAt` datetime DEFAULT NULL,
  `mdnsName` varchar(128) DEFAULT NULL,
  `mdnsServices` varchar(512) DEFAULT NULL,
  `openPorts` text DEFAULT NULL,
  `osGuess` varchar(255) DEFAULT NULL,
  `banners` text DEFAULT NULL,
  `nmapEnrichedAt` datetime DEFAULT NULL,
  PRIMARY KEY (`discId`),
  UNIQUE KEY `ip` (`ip`,`kind`),
  KEY `status` (`status`),
  KEY `segId` (`segId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `discoveryScanJob`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `discoveryScanJob` (
  `scanId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `requestedBy` varchar(128) NOT NULL,
  `ownerNodeId` bigint(20) unsigned NOT NULL,
  `leaseEpoch` bigint(20) unsigned NOT NULL,
  `cidr` varchar(32) NOT NULL,
  `ports` varchar(255) NOT NULL DEFAULT '',
  `status` enum('queued','running','completed','failed','cancelled') NOT NULL DEFAULT 'queued',
  `hostsTotal` int(10) unsigned NOT NULL,
  `hostsDone` int(10) unsigned NOT NULL DEFAULT 0,
  `hostsFound` int(10) unsigned NOT NULL DEFAULT 0,
  `errorCode` int(11) DEFAULT NULL,
  `errorText` varchar(255) DEFAULT NULL,
  `createdAt` datetime(3) NOT NULL,
  `startedAt` datetime(3) DEFAULT NULL,
  `updatedAt` datetime(3) NOT NULL,
  `completedAt` datetime(3) DEFAULT NULL,
  PRIMARY KEY (`scanId`),
  KEY `ix_discoveryScanJob_status` (`status`,`scanId`),
  KEY `ix_discoveryScanJob_requester` (`requestedBy`,`scanId`),
  KEY `ix_discoveryScanJob_owner` (`ownerNodeId`,`leaseEpoch`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `enrollment`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `enrollment` (
  `enrId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `host` varchar(255) DEFAULT NULL,
  `ip` varchar(64) DEFAULT NULL,
  `role` enum('client','monitor','server') NOT NULL,
  `certFp` varchar(95) DEFAULT NULL,
  `status` enum('token','pending','approved','rejected','expired') NOT NULL,
  `requestedAt` datetime DEFAULT NULL,
  `decidedAt` datetime DEFAULT NULL,
  `decidedBy` varchar(64) DEFAULT NULL,
  `csrPem` text DEFAULT NULL,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `issuedCertPem` mediumtext DEFAULT NULL,
  `enrollTokenHash` char(64) DEFAULT NULL,
  `tokenExpiresAt` datetime DEFAULT NULL,
  `certAckAt` datetime DEFAULT NULL,
  PRIMARY KEY (`enrId`),
  UNIQUE KEY `uq_enrollment_token_hash` (`enrollTokenHash`),
  KEY `status` (`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `gearInterfaceCurrent`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `gearInterfaceCurrent` (
  `gearId` varchar(48) NOT NULL,
  `ifIndex` int(11) NOT NULL,
  `ifName` varchar(96) DEFAULT NULL,
  `inOctets` bigint(20) unsigned DEFAULT NULL,
  `outOctets` bigint(20) unsigned DEFAULT NULL,
  `inRateKbps` bigint(20) unsigned DEFAULT NULL,
  `outRateKbps` bigint(20) unsigned DEFAULT NULL,
  `operStatus` tinyint(4) DEFAULT NULL,
  `sampledAt` datetime NOT NULL,
  PRIMARY KEY (`gearId`,`ifIndex`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `gearInterfaceHistory`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `gearInterfaceHistory` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `gearId` varchar(48) NOT NULL,
  `ifIndex` int(11) NOT NULL,
  `inRateKbps` bigint(20) unsigned DEFAULT NULL,
  `outRateKbps` bigint(20) unsigned DEFAULT NULL,
  `operStatus` tinyint(4) DEFAULT NULL,
  `sampledAt` datetime NOT NULL,
  PRIMARY KEY (`id`,`sampledAt`),
  KEY `gearId` (`gearId`,`ifIndex`,`sampledAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
 PARTITION BY RANGE (to_days(`sampledAt`))
(PARTITION `p_min` VALUES LESS THAN (0) ENGINE = InnoDB,
 PARTITION `p_max` VALUES LESS THAN MAXVALUE ENGINE = InnoDB);
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `globalConfig`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `globalConfig` (
  `id` tinyint(3) unsigned NOT NULL DEFAULT 1,
  `configBlob` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL CHECK (json_valid(`configBlob`)),
  `epoch` bigint(20) unsigned NOT NULL DEFAULT 1,
  `updatedAt` datetime NOT NULL,
  `updatedBy` varchar(64) DEFAULT NULL,
  PRIMARY KEY (`id`),
  CONSTRAINT `globalConfig_singleton` CHECK (`id` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `hostCurrent`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `hostCurrent` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `sampledAt` datetime NOT NULL,
  `cpuLoadMilli` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`cpuLoadMilli`)),
  `ramUsedKb` bigint(20) unsigned DEFAULT NULL,
  `ramTotalKb` bigint(20) unsigned DEFAULT NULL,
  `swapUsedKb` bigint(20) unsigned DEFAULT NULL,
  `swapTotalKb` bigint(20) unsigned DEFAULT NULL,
  `disks` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`disks`)),
  `ifaces` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`ifaces`)),
  `usbBuses` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`usbBuses`)),
  `fsReadonlyCount` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `blockDevMissing` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `smartFailCount` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `failedUnitCount` smallint(5) unsigned NOT NULL DEFAULT 0,
  `dmesgCritCount` smallint(5) unsigned NOT NULL DEFAULT 0,
  `fsReadonlyList` varchar(256) NOT NULL DEFAULT '',
  `smartFailList` varchar(256) NOT NULL DEFAULT '',
  `failedUnitList` varchar(256) NOT NULL DEFAULT '',
  `dmesgCritSample` varchar(256) NOT NULL DEFAULT '',
  PRIMARY KEY (`nodeId`),
  CONSTRAINT `FK_hc_node` FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `hostHistory`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `hostHistory` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `nodeId` bigint(20) unsigned NOT NULL,
  `sampledAt` datetime NOT NULL,
  `cpuAvgMilli` int(10) unsigned DEFAULT NULL,
  `ramUsedKb` bigint(20) unsigned DEFAULT NULL,
  `swapUsedKb` bigint(20) unsigned DEFAULT NULL,
  `diskMinFreePct` smallint(6) DEFAULT NULL,
  `netKbps` bigint(20) unsigned DEFAULT NULL,
  `fsReadonlyCount` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `blockDevMissing` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `smartFailCount` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `failedUnitCount` smallint(5) unsigned NOT NULL DEFAULT 0,
  `dmesgCritCount` smallint(5) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`,`sampledAt`),
  KEY `nodeId` (`nodeId`,`sampledAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
 PARTITION BY RANGE (to_days(`sampledAt`))
(PARTITION `p_min` VALUES LESS THAN (0) ENGINE = InnoDB,
 PARTITION `p_max` VALUES LESS THAN MAXVALUE ENGINE = InnoDB);
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `lldpEdge`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `lldpEdge` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `gearId` varchar(48) DEFAULT NULL,
  `localIf` varchar(48) DEFAULT NULL,
  `peerPort` varchar(48) DEFAULT NULL,
  `linkType` enum('wired','wireless') NOT NULL,
  `speedMbps` int(11) DEFAULT NULL,
  `rssi` smallint(6) DEFAULT NULL,
  `viaLldp` tinyint(1) NOT NULL DEFAULT 0,
  `observedAt` datetime NOT NULL,
  PRIMARY KEY (`id`),
  KEY `nodeId` (`nodeId`),
  KEY `gearId` (`gearId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `maintenanceWindow`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `maintenanceWindow` (
  `windowId` int(11) NOT NULL AUTO_INCREMENT,
  `scope` enum('node','all') NOT NULL DEFAULT 'node',
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `hostFqdn` varchar(255) DEFAULT NULL,
  `ipAddr` varchar(45) DEFAULT NULL,
  `reason` varchar(255) DEFAULT NULL,
  `startsAt` datetime NOT NULL,
  `endsAt` datetime NOT NULL,
  `status` enum('scheduled','active','completed','cancelled') NOT NULL DEFAULT 'scheduled',
  `createdBy` varchar(64) DEFAULT NULL,
  `createdAt` datetime NOT NULL DEFAULT current_timestamp(),
  `updatedAt` datetime NOT NULL DEFAULT current_timestamp() ON UPDATE current_timestamp(),
  PRIMARY KEY (`windowId`),
  KEY `idx_window_time` (`startsAt`,`endsAt`),
  KEY `idx_window_node` (`nodeId`),
  KEY `idx_window_host` (`hostFqdn`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `networkGear`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `networkGear` (
  `gearId` varchar(48) NOT NULL,
  `name` varchar(96) NOT NULL,
  `kind` enum('gateway','switch','ap','router','hub','wanBackup','other') NOT NULL,
  `model` varchar(96) DEFAULT NULL,
  `segId` varchar(32) DEFAULT NULL,
  `ports` smallint(6) DEFAULT NULL,
  `uplinkGearId` varchar(48) DEFAULT NULL,
  `wireless` tinyint(1) NOT NULL DEFAULT 0,
  `mgmtIp` varchar(64) DEFAULT NULL,
  `lastSeenAt` datetime DEFAULT NULL,
  `snmpCommunity` varchar(96) DEFAULT NULL,
  PRIMARY KEY (`gearId`),
  KEY `segId` (`segId`),
  KEY `uplinkGearId` (`uplinkGearId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `node`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `node` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `role` enum('client','monitor','server') NOT NULL,
  `hostFqdn` varchar(255) NOT NULL,
  `certCn` varchar(255) NOT NULL,
  `osName` varchar(64) DEFAULT NULL,
  `arch` varchar(16) DEFAULT NULL,
  `segId` varchar(32) DEFAULT NULL,
  `enrolledAt` datetime NOT NULL,
  `lastSeenAt` datetime DEFAULT NULL,
  `configEpoch` bigint(20) unsigned NOT NULL DEFAULT 0,
  `state` enum('up','degraded','down','unknown','retired') NOT NULL DEFAULT 'unknown',
  `criticality` tinyint(4) NOT NULL DEFAULT 2,
  `uplinkGearId` varchar(48) DEFAULT NULL,
  `leafCertFp` char(64) DEFAULT NULL,
  `leafCertSerial` varchar(40) DEFAULT NULL,
  `leafIssuerFp` char(64) CHARACTER SET ascii COLLATE ascii_bin DEFAULT NULL,
  PRIMARY KEY (`nodeId`),
  KEY `role` (`role`),
  KEY `lastSeenAt` (`lastSeenAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `nodeConfig`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `nodeConfig` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `targetEpoch` bigint(20) unsigned NOT NULL,
  `appliedEpoch` bigint(20) unsigned DEFAULT NULL,
  `configBlob` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`configBlob`)),
  `lastDirectiveAt` datetime DEFAULT NULL,
  `lastResult` varchar(128) DEFAULT NULL,
  PRIMARY KEY (`nodeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `opieReport`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `opieReport` (
  `reportId` int(11) NOT NULL AUTO_INCREMENT,
  `incidentKey` varchar(160) NOT NULL,
  `triggerKind` enum('crit','warn-storm') NOT NULL,
  `firstEventId` bigint(20) unsigned DEFAULT NULL,
  `nodeId` bigint(20) unsigned DEFAULT NULL,
  `hostFqdn` varchar(255) DEFAULT NULL,
  `severity` enum('info','warn','crit') DEFAULT NULL,
  `status` enum('investigating','done','failed') NOT NULL DEFAULT 'investigating',
  `summary` varchar(512) DEFAULT NULL,
  `analysis` mediumtext DEFAULT NULL,
  `model` varchar(48) DEFAULT NULL,
  `durationSec` int(11) DEFAULT NULL,
  `startedAt` datetime NOT NULL DEFAULT current_timestamp(),
  `finishedAt` datetime DEFAULT NULL,
  PRIMARY KEY (`reportId`),
  UNIQUE KEY `uq_incident` (`incidentKey`),
  KEY `idx_started` (`startedAt`),
  KEY `idx_node` (`nodeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `panelCommand`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `panelCommand` (
  `cmdId` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `kind` tinyint(3) unsigned NOT NULL,
  `arg` smallint(5) unsigned NOT NULL,
  `createdAt` datetime(6) NOT NULL DEFAULT current_timestamp(6),
  `createdBy` varchar(128) NOT NULL,
  `status` enum('pending','applied','expired') NOT NULL DEFAULT 'pending',
  `appliedAt` datetime(6) DEFAULT NULL,
  `expiredAt` datetime(6) DEFAULT NULL,
  PRIMARY KEY (`cmdId`),
  KEY `panelCommandPending` (`status`,`cmdId`),
  KEY `panelCommandRecent` (`cmdId` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `panelScreenConfig`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `panelScreenConfig` (
  `configId` tinyint(3) unsigned NOT NULL CHECK (`configId` = 1),
  `screenCfg` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL CHECK (json_valid(`screenCfg`)),
  `flags` tinyint(3) unsigned NOT NULL,
  `reportedAt` datetime(6) NOT NULL,
  PRIMARY KEY (`configId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `panelState`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `panelState` (
  `stateId` tinyint(3) unsigned NOT NULL,
  `theme` tinyint(3) unsigned NOT NULL,
  `screen` tinyint(3) unsigned NOT NULL,
  `brightnessPct` tinyint(3) unsigned NOT NULL,
  `autoBright` tinyint(3) unsigned NOT NULL,
  `sleeping` tinyint(3) unsigned NOT NULL,
  `alarmArmed` tinyint(3) unsigned NOT NULL,
  `alarmAcked` tinyint(3) unsigned NOT NULL,
  `dwellSec` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `ackedEpisodeId` int(10) unsigned NOT NULL,
  `lastCmdId` bigint(20) unsigned NOT NULL,
  `reportedAt` datetime(6) NOT NULL,
  PRIMARY KEY (`stateId`),
  CONSTRAINT `panelStateSingleton` CHECK (`stateId` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `pendingControlDirective`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `pendingControlDirective` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `correlationId` int(10) unsigned NOT NULL,
  `verb` tinyint(3) unsigned NOT NULL,
  `targetEpoch` bigint(20) unsigned NOT NULL DEFAULT 0,
  `issuedAt` datetime(6) NOT NULL DEFAULT current_timestamp(6),
  PRIMARY KEY (`nodeId`,`correlationId`),
  KEY `ix_pending_control_issued` (`issuedAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `pool`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `pool` (
  `poolId` int(10) unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(64) NOT NULL,
  `description` varchar(255) DEFAULT NULL,
  `color` varchar(16) DEFAULT NULL,
  `createdAt` datetime NOT NULL,
  PRIMARY KEY (`poolId`),
  UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB AUTO_INCREMENT=2 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `probeCurrent`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `probeCurrent` (
  `targetId` varchar(128) NOT NULL,
  `monitorNode` bigint(20) unsigned NOT NULL,
  `outcome` enum('ok','timeout','refused','unreachable','dns_fail','tls_fail','proto_err') DEFAULT NULL,
  `rttMicros` int(10) unsigned DEFAULT NULL,
  `jitterMicros` int(10) unsigned DEFAULT NULL,
  `lossPermille` smallint(5) unsigned DEFAULT NULL,
  `throughputKbps` int(10) unsigned DEFAULT NULL,
  `serviceMeta` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL CHECK (json_valid(`serviceMeta`)),
  `sampledAt` datetime NOT NULL,
  PRIMARY KEY (`targetId`,`monitorNode`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `probeHistory`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `probeHistory` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `targetId` varchar(128) DEFAULT NULL,
  `monitorNode` bigint(20) unsigned DEFAULT NULL,
  `outcome` varchar(16) DEFAULT NULL,
  `rttMicros` int(10) unsigned DEFAULT NULL,
  `lossPermille` smallint(5) unsigned DEFAULT NULL,
  `sampledAt` datetime NOT NULL,
  PRIMARY KEY (`id`,`sampledAt`),
  KEY `targetId` (`targetId`,`sampledAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
 PARTITION BY RANGE (to_days(`sampledAt`))
(PARTITION `p_min` VALUES LESS THAN (0) ENGINE = InnoDB,
 PARTITION `p_max` VALUES LESS THAN MAXVALUE ENGINE = InnoDB);
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `probeTarget`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `probeTarget` (
  `targetId` varchar(128) NOT NULL,
  `host` varchar(255) DEFAULT NULL,
  `port` smallint(5) unsigned DEFAULT NULL,
  `proto` enum('icmp','tcp','udp') NOT NULL,
  `replFactor` tinyint(4) NOT NULL DEFAULT 2,
  `label` varchar(128) DEFAULT NULL,
  `segId` varchar(32) DEFAULT NULL,
  `assetId` bigint(20) unsigned DEFAULT NULL,
  `probeType` varchar(16) NOT NULL DEFAULT 'tcp',
  `checkArg` varchar(255) DEFAULT NULL,
  PRIMARY KEY (`targetId`),
  KEY `idx_probeTarget_asset` (`assetId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `probeTargetTombstone`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `probeTargetTombstone` (
  `targetId` varchar(128) NOT NULL,
  `removedAt` datetime NOT NULL,
  `removedBy` varchar(64) NOT NULL,
  `assetId` bigint(20) unsigned DEFAULT NULL,
  PRIMARY KEY (`targetId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `procCurrent`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `procCurrent` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `procName` varchar(128) NOT NULL,
  `pid` int(11) DEFAULT NULL,
  `runState` char(1) DEFAULT NULL,
  `nFiles` int(11) DEFAULT NULL,
  `nSockets` int(11) DEFAULT NULL,
  `rssKb` bigint(20) unsigned DEFAULT NULL,
  `sampledAt` datetime NOT NULL,
  PRIMARY KEY (`nodeId`,`procName`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `push_subscriptions`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `push_subscriptions` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `operator` varchar(255) DEFAULT NULL,
  `endpoint` varchar(512) NOT NULL,
  `p256dh` varchar(255) NOT NULL,
  `auth` varchar(255) NOT NULL,
  `userAgent` varchar(255) DEFAULT NULL,
  `createdAt` datetime NOT NULL,
  `lastSeenAt` datetime NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_push_subscriptions_endpoint` (`endpoint`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `reportReceipt`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `reportReceipt` (
  `nodeId` bigint(20) unsigned NOT NULL,
  `sessionId` binary(16) NOT NULL,
  `seqNo` bigint(20) unsigned NOT NULL,
  `msgType` int(10) unsigned NOT NULL,
  `payloadHash` binary(32) NOT NULL,
  `receivedAt` datetime(3) NOT NULL DEFAULT current_timestamp(3),
  PRIMARY KEY (`nodeId`,`sessionId`,`seqNo`,`msgType`),
  KEY `receivedAt` (`receivedAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `segment`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `segment` (
  `segId` varchar(32) NOT NULL,
  `label` varchar(64) NOT NULL,
  `cidr` varchar(64) NOT NULL,
  `wireless` tinyint(1) NOT NULL DEFAULT 0,
  `notes` varchar(255) DEFAULT NULL,
  PRIMARY KEY (`segId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;
DROP TABLE IF EXISTS `serverLease`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `serverLease` (
  `id` tinyint(4) NOT NULL DEFAULT 1,
  `leaseHolder` bigint(20) unsigned DEFAULT NULL,
  `leaseEpoch` bigint(20) unsigned NOT NULL DEFAULT 0,
  `expiresAt` datetime(3) NOT NULL,
  PRIMARY KEY (`id`),
  CONSTRAINT `CONSTRAINT_1` CHECK (`id` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*M!100616 SET NOTE_VERBOSITY=@OLD_NOTE_VERBOSITY */;
