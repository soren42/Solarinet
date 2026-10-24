-- Persist retry scheduling for confirmed directives so a server restart or
-- failover does not lose a decommission that has not yet been acknowledged.
ALTER TABLE decommissionChallenge
  ADD COLUMN deliveryAttempts INT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN lastDeliveryAt DATETIME(6) NULL,
  ADD COLUMN nextDeliveryAt DATETIME(6) NULL,
  ADD KEY ix_decommissionChallenge_retry (state, nextDeliveryAt);
