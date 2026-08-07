-- AW panel CONFIG report singleton and UniFi role additions.
CREATE TABLE IF NOT EXISTS panelScreenConfig (
  configId TINYINT UNSIGNED PRIMARY KEY CHECK (configId = 1),
  screenCfg JSON NOT NULL,
  flags TINYINT UNSIGNED NOT NULL,
  reportedAt DATETIME(6) NOT NULL
) ENGINE=InnoDB;

ALTER TABLE networkGear MODIFY kind ENUM('gateway','switch','ap','router',
  'hub','wanBackup','other') NOT NULL;
