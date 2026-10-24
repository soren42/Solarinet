-- Bind node acknowledgements to a specific issued decommission challenge.
-- A consumed challenge remains pending remote acknowledgement; only a matching
-- generation can transition it to acknowledged and retire the node.
ALTER TABLE decommissionChallenge
  ADD COLUMN generation BIGINT UNSIGNED NOT NULL DEFAULT 1,
  MODIFY COLUMN state ENUM('pending','consumed','acknowledged','cancelled')
    NOT NULL DEFAULT 'pending';
