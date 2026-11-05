-- Status panel v2: mark switch ports facing other switches, so a flow crossing
-- several switches counts once in the LAN total.
ALTER TABLE gearInterfaceCurrent
  ADD COLUMN isTrunk BOOLEAN NOT NULL DEFAULT FALSE;
