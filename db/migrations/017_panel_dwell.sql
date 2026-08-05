-- PANEL-CP12: STATE byte 7 is the panel's current dwell seconds.
ALTER TABLE panelState
    ADD COLUMN dwellSec TINYINT UNSIGNED NOT NULL DEFAULT 0
    AFTER alarmAcked;
