-- 032_node_os_hinfo.sql
-- Permit RFC 1035 HINFO records in the SoR DNS catalog. Node OS and eligible
-- hardware-model architecture values are reconciled by sor_reconcile_discovery.
-- This migration is intentionally isolated at 032; inventory work owns 033+.
-- Rollback: after removing all HINFO rows, ALTER dns_records MODIFY rrtype back
-- to the pre-032 ENUM listed below. MariaDB DDL autocommits.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

ALTER TABLE `dns_records`
  MODIFY COLUMN `rrtype` ENUM('A','AAAA','CNAME','PTR','SRV','TXT','MX','NS','SOA','CAA','DS','DNSKEY','HINFO')
    NOT NULL COMMENT 'record type';
