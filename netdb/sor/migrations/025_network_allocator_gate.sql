-- ============================================================================
-- 025_network_allocator_gate.sql — model source state and require approval
-- ============================================================================
-- UniFi import records controller state in `networks`; imported or seeded
-- networks are not automatically safe for static IP allocation. This migration
-- adds the source state fields and an explicit, default-off eligibility gate.
-- Apply on Cesium only; replication carries the DDL to Benzene.
-- ============================================================================

ALTER TABLE `networks`
  ADD COLUMN IF NOT EXISTS `enabled` TINYINT(1) NOT NULL DEFAULT 1
    COMMENT 'source-reported network state; disabled networks are not allocator candidates',
  ADD COLUMN IF NOT EXISTS `management` VARCHAR(32) NULL
    COMMENT 'upstream management mode, e.g. GATEWAY, SWITCH, UNMANAGED',
  ADD COLUMN IF NOT EXISTS `is_default` TINYINT(1) NOT NULL DEFAULT 0
    COMMENT 'source marks this as its default network',
  ADD COLUMN IF NOT EXISTS `allocator_eligible` TINYINT(1) NOT NULL DEFAULT 0
    COMMENT 'explicit operator approval; imports never enable address allocation automatically',
  ADD COLUMN IF NOT EXISTS `allocator_approved_by` VARCHAR(128) NULL
    COMMENT 'authenticated dashboard operator who last approved allocation',
  ADD COLUMN IF NOT EXISTS `allocator_approved_at` DATETIME NULL
    COMMENT 'time of the last operator allocation approval';

ALTER TABLE `networks`
  ADD INDEX IF NOT EXISTS `ix_networks_allocator` (`enabled`,`allocator_eligible`,`deleted_at`);
