-- PANEL-CP1: durable command queue and latest state from the physical panel.
-- Commands are re-served while pending.  STATE lastCmdId is the only path to
-- applied; pending commands age out explicitly after 120 seconds in panel.php.

CREATE TABLE IF NOT EXISTS panelCommand (
    cmdId BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    kind TINYINT UNSIGNED NOT NULL,
    arg SMALLINT UNSIGNED NOT NULL,
    createdAt DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    createdBy VARCHAR(128) NOT NULL,
    status ENUM('pending', 'applied', 'expired') NOT NULL DEFAULT 'pending',
    appliedAt DATETIME(6) NULL,
    expiredAt DATETIME(6) NULL,
    PRIMARY KEY (cmdId),
    KEY panelCommandPending (status, cmdId),
    KEY panelCommandRecent (cmdId DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS panelState (
    stateId TINYINT UNSIGNED NOT NULL,
    theme TINYINT UNSIGNED NOT NULL,
    screen TINYINT UNSIGNED NOT NULL,
    brightnessPct TINYINT UNSIGNED NOT NULL,
    autoBright TINYINT UNSIGNED NOT NULL,
    sleeping TINYINT UNSIGNED NOT NULL,
    alarmArmed TINYINT UNSIGNED NOT NULL,
    alarmAcked TINYINT UNSIGNED NOT NULL,
    ackedEpisodeId INT UNSIGNED NOT NULL,
    lastCmdId BIGINT UNSIGNED NOT NULL,
    reportedAt DATETIME(6) NOT NULL,
    PRIMARY KEY (stateId),
    CONSTRAINT panelStateSingleton CHECK (stateId = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
