-- 006_snmp_gear.sql — SNMP polling tier: managed-gear interface metrics.
--
-- The server polls each networkGear's IF-MIB counters (ifHCIn/OutOctets,
-- ifOperStatus) by numeric OID and stores per-interface throughput. Rates are
-- computed server-side from the octet deltas between samples.

-- per-gear SNMP read community (NULL -> use the server.conf default).
ALTER TABLE networkGear
  ADD COLUMN snmpCommunity VARCHAR(96) DEFAULT NULL;

-- current per-interface snapshot (one row per gear+ifIndex).
CREATE TABLE IF NOT EXISTS gearInterfaceCurrent (
  gearId      VARCHAR(48) NOT NULL,
  ifIndex     INT NOT NULL,
  ifName      VARCHAR(96),
  inOctets    BIGINT UNSIGNED,   outOctets    BIGINT UNSIGNED,
  inRateKbps  BIGINT UNSIGNED,   outRateKbps  BIGINT UNSIGNED,
  operStatus  TINYINT,                        -- IF-MIB ifOperStatus: 1=up,2=down,...
  sampledAt   DATETIME NOT NULL,
  PRIMARY KEY (gearId, ifIndex)
) ENGINE=InnoDB;

-- append-only per-interface rate history, partitioned like hostHistory.
CREATE TABLE IF NOT EXISTS gearInterfaceHistory (
  id          BIGINT UNSIGNED AUTO_INCREMENT,
  gearId      VARCHAR(48) NOT NULL,
  ifIndex     INT NOT NULL,
  inRateKbps  BIGINT UNSIGNED,   outRateKbps BIGINT UNSIGNED,
  operStatus  TINYINT,
  sampledAt   DATETIME NOT NULL,
  PRIMARY KEY (id, sampledAt),
  INDEX (gearId, ifIndex, sampledAt)
) ENGINE=InnoDB
  PARTITION BY RANGE (TO_DAYS(sampledAt)) (
    PARTITION p_min VALUES LESS THAN (0),
    PARTITION p_max VALUES LESS THAN MAXVALUE
  );
