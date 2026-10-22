-- 027_interconnect_cable_details.sql — preserve RackWire cable metadata
--
-- RackWire plans distinguish cable/power-cord standards and jacket colors.
-- Keep these details on the shared interconnect row alongside label, length,
-- and notes so planned topology survives a SoR round trip.

ALTER TABLE `interconnects`
  ADD COLUMN IF NOT EXISTS `media_standard` VARCHAR(32) NULL
      COMMENT 'cable or power-cord standard identifier, e.g. cat6, om3, c13c14'
      AFTER `length_m`,
  ADD COLUMN IF NOT EXISTS `jacket_color` VARCHAR(64) NULL
      COMMENT 'physical cable jacket color, when known'
      AFTER `media_standard`;
