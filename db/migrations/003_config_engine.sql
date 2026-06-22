-- 003_config_engine.sql — persisted configuration engine (dashboard config push).
--
-- Per-node desired config already lives in nodeConfig (targetEpoch + configBlob,
-- written by serverProvisionNode on every push). This migration adds the
-- fleet-wide global config document so the dashboard's global tolerances/
-- schedule/retention are persisted (not just synthesized) and can be pushed to
-- the fleet. Additive; safe to re-run.

CREATE TABLE IF NOT EXISTS globalConfig (
  id         TINYINT UNSIGNED PRIMARY KEY DEFAULT 1,   -- singleton row (id=1)
  configBlob JSON NOT NULL,
  epoch      BIGINT UNSIGNED NOT NULL DEFAULT 1,        -- bumped on every save
  updatedAt  DATETIME NOT NULL,
  updatedBy  VARCHAR(64),
  CONSTRAINT globalConfig_singleton CHECK (id = 1)
) ENGINE=InnoDB;
