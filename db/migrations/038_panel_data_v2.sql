-- Status panel v2: retain UniFi MAC identity and mark measured port roles.
ALTER TABLE networkGear
  ADD COLUMN macAddress CHAR(17) NULL,
  ADD INDEX idx_networkGear_macAddress (macAddress);

ALTER TABLE gearInterfaceCurrent
  ADD COLUMN isUplink BOOLEAN NOT NULL DEFAULT FALSE;

CREATE INDEX idx_alertEvent_current_condition
  ON alertEvent (clearedAt, ruleId, nodeId, targetId, eventId);
