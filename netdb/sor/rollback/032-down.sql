-- Manual rollback for 032_node_os_hinfo.sql.
-- HINFO records must be deliberately removed first; this guarded preflight
-- refuses to discard them. MariaDB DDL autocommits.
DROP PROCEDURE IF EXISTS `_solarinet_rollback_m032_assert_reversible`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m032_assert_reversible`()
BEGIN
  IF EXISTS (SELECT 1 FROM dns_records WHERE rrtype='HINFO') THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='refusing rollback: dns_records still contains HINFO rows';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_rollback_m032_assert_reversible`();
DROP PROCEDURE `_solarinet_rollback_m032_assert_reversible`;

ALTER TABLE `dns_records`
  MODIFY COLUMN `rrtype` ENUM('A','AAAA','CNAME','PTR','SRV','TXT','MX','NS','SOA','CAA','DS','DNSKEY')
    NOT NULL COMMENT 'record type';
