-- ============================================================================
-- 026_unifi_network_cdc.sql — publish imported network fact changes
-- ============================================================================
-- Networks and subnets already have core CDC triggers. UniFi import also
-- mutates VLANs, DHCP ranges, and external_refs; publish those changes through
-- the same transactional outbox so downstream views can converge.
-- Apply on Cesium only; replication carries trigger metadata to Benzene.
-- ============================================================================

CREATE OR REPLACE TRIGGER trg_vlans_ai
AFTER INSERT ON `vlans` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('vlans',NEW.id,'insert');
CREATE OR REPLACE TRIGGER trg_vlans_au
AFTER UPDATE ON `vlans` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('vlans',NEW.id,'update');
CREATE OR REPLACE TRIGGER trg_vlans_ad
AFTER DELETE ON `vlans` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('vlans',OLD.id,'delete');

CREATE OR REPLACE TRIGGER trg_dhcp_ranges_ai
AFTER INSERT ON `dhcp_ranges` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dhcp_ranges',NEW.id,'insert');
CREATE OR REPLACE TRIGGER trg_dhcp_ranges_au
AFTER UPDATE ON `dhcp_ranges` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dhcp_ranges',NEW.id,'update');
CREATE OR REPLACE TRIGGER trg_dhcp_ranges_ad
AFTER DELETE ON `dhcp_ranges` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('dhcp_ranges',OLD.id,'delete');

CREATE OR REPLACE TRIGGER trg_external_refs_ai
AFTER INSERT ON `external_refs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('external_refs',NEW.id,'insert');
CREATE OR REPLACE TRIGGER trg_external_refs_au
AFTER UPDATE ON `external_refs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('external_refs',NEW.id,'update');
CREATE OR REPLACE TRIGGER trg_external_refs_ad
AFTER DELETE ON `external_refs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name,row_id,action) VALUES ('external_refs',OLD.id,'delete');
