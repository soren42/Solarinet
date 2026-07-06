-- ============================================================================
-- SolariNet System of Record (SoR) -- authoritative homelab inventory DB
-- ============================================================================
-- Target: MariaDB >= 10.7 (developed/validated on 12.3), InnoDB, utf8mb4.
-- Database: `sor` on cesium (10.1.0.200).
--
-- Design rules baked into every table:
--   * Provenance: every row carries (source_id, asserted_kind, asserted_at) --
--     which system asserted the fact, whether a human or a machine did, and
--     when. Nothing in this DB is ever typed in "from nowhere".
--   * Soft delete: deleted_at IS NULL = live row. Rows are retired, not lost;
--     the audit_log keeps the old values regardless.
--   * created_at / updated_at on every table.
--   * All FKs are declared and indexed. ON DELETE RESTRICT is the default
--     posture (the SoR refuses to silently orphan facts); junction rows and
--     pure-detail rows cascade from their owner.
--   * IP addresses use the native INET6 type (stores v4 and v6, sorts and
--     compares correctly).
--
-- This file is idempotent-friendly: CREATE TABLE IF NOT EXISTS throughout,
-- so re-running against an already-created schema is a no-op (it does NOT
-- migrate; schema changes get numbered migration files later).
-- ============================================================================

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- ----------------------------------------------------------------------------
-- 1. sources -- registry of systems that assert facts into the SoR
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `sources` (
  `id`            SMALLINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `slug`          VARCHAR(64)  NOT NULL COMMENT 'stable machine name, e.g. netdb, samba_ad, bind, forgejo, solarinet_monitor, unifi, pihole, dashboard, manual',
  `display_name`  VARCHAR(128) NOT NULL COMMENT 'human-readable name shown in the dashboard',
  `kind`          ENUM('seed','sync','discovery','human') NOT NULL DEFAULT 'sync'
                  COMMENT 'seed=one-time import; sync=recurring bidirectional; discovery=passive detection; human=dashboard/manual edits',
  `endpoint`      VARCHAR(255) NULL COMMENT 'where the source lives (URL/host), informational',
  `notes`         TEXT NULL COMMENT 'free-form operator notes',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_sources_slug` (`slug`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Registry of every system allowed to assert facts (provenance targets)';

-- Well-known sources (idempotent; slugs are the stable contract).
INSERT IGNORE INTO `sources` (`slug`,`display_name`,`kind`,`endpoint`) VALUES
  ('manual',            'Manual entry',                 'human',     NULL),
  ('dashboard',         'SolariNet dashboard',          'human',     'https://dashboard.akoria.net'),
  ('netdb',             'netdb YAML source-of-truth',   'seed',      'repo:netdb/akoria-hosts.yml'),
  ('samba_ad',          'Samba AD (akoria.org)',        'sync',      'ldap://radium.akoria.net'),
  ('bind',              'BIND zones (akoria.net)',      'sync',      'xenon.akoria.net'),
  ('forgejo',           'Forgejo (git.akoria.net)',     'sync',      'https://git.akoria.net'),
  ('solarinet_monitor', 'SolariNet monitoring DB',      'discovery', 'xenon.akoria.net'),
  ('unifi',             'UniFi controller',             'sync',      'https://chemistry.akoria.net'),
  ('pihole',            'Pi-hole (helium/mercury)',     'sync',      'helium.akoria.net');

-- ----------------------------------------------------------------------------
-- 2. locations -- physical hierarchy: site > room > rack > position
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `locations` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `parent_id`     INT UNSIGNED NULL COMMENT 'containing location; NULL = top-level site',
  `name`          VARCHAR(128) NOT NULL COMMENT 'name within the parent, e.g. "Office", "Minirack", "U4"',
  `loc_type`      ENUM('site','building','floor','room','rack','shelf','position','zone') NOT NULL
                  COMMENT 'what level of the physical hierarchy this row is',
  `rack_units`    SMALLINT UNSIGNED NULL COMMENT 'for racks: total U capacity; for positions: starting U',
  `notes`         TEXT NULL COMMENT 'free-form operator notes',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_locations_parent_name` (`parent_id`,`name`),
  KEY `ix_locations_type` (`loc_type`),
  CONSTRAINT `fk_locations_parent` FOREIGN KEY (`parent_id`) REFERENCES `locations`(`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_locations_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Physical location hierarchy (site/room/rack/position) via self-referencing tree';

-- ----------------------------------------------------------------------------
-- 3. hardware_models -- make/model catalog (one row per product)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `hardware_models` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `make`          VARCHAR(128) NOT NULL COMMENT 'manufacturer, e.g. Ubiquiti, Dell, Raspberry Pi',
  `model`         VARCHAR(128) NOT NULL COMMENT 'product model, e.g. U7 Pro, PowerEdge R410, CM5',
  `hw_type`       ENUM('server','workstation','sbc','nas','router','switch','ap','printer','scanner',
                       'iot','appliance','console','media','vehicle','tablet','phone','kvm','sensor',
                       'camera','ups','moca','other') NOT NULL DEFAULT 'other'
                  COMMENT 'coarse device class for filtering and dashboard icons',
  `notes`         TEXT NULL COMMENT 'free-form notes (spec links, quirks)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_hw_models_make_model` (`make`,`model`),
  KEY `ix_hw_models_type` (`hw_type`),
  CONSTRAINT `fk_hw_models_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Catalog of hardware products (make+model); one row per product, not per unit';

-- ----------------------------------------------------------------------------
-- 4. hardware_units -- individual physical units (serials) of a model
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `hardware_units` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `model_id`      INT UNSIGNED NOT NULL COMMENT 'FK: which product this unit is',
  `serial`        VARCHAR(128) NULL COMMENT 'manufacturer serial number (unique per model when known)',
  `asset_tag`     VARCHAR(64)  NULL COMMENT 'internal inventory tag, if labeled',
  `location_id`   INT UNSIGNED NULL COMMENT 'FK: where this unit physically sits',
  `acquired_on`   DATE NULL COMMENT 'purchase/acquisition date',
  `notes`         TEXT NULL COMMENT 'free-form notes (warranty, condition)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_hw_units_model_serial` (`model_id`,`serial`),
  KEY `ix_hw_units_location` (`location_id`),
  CONSTRAINT `fk_hw_units_model`    FOREIGN KEY (`model_id`)    REFERENCES `hardware_models`(`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_hw_units_location` FOREIGN KEY (`location_id`) REFERENCES `locations`(`id`)       ON DELETE SET NULL,
  CONSTRAINT `fk_hw_units_source`   FOREIGN KEY (`source_id`)   REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Individual physical hardware units (one row per serial-numbered box)';

-- ----------------------------------------------------------------------------
-- 5. operating_systems -- OS catalog (family + name + version)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `operating_systems` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `family`        ENUM('linux','bsd','windows','macos','ios','android','rtos','embedded','network_os','other')
                  NOT NULL DEFAULT 'other' COMMENT 'broad OS family for filtering',
  `name`          VARCHAR(128) NOT NULL COMMENT 'distribution/product name, e.g. Debian, UniFi OS, HomeKit firmware',
  `version`       VARCHAR(64)  NOT NULL DEFAULT '' COMMENT 'release/version string, e.g. 13, 24.04, 4.1.13; empty = unknown',
  `eol_on`        DATE NULL COMMENT 'end-of-life date if known (drives upgrade nagging)',
  `notes`         TEXT NULL COMMENT 'free-form notes',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_os_name_version` (`name`,`version`),
  KEY `ix_os_family` (`family`),
  CONSTRAINT `fk_os_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Operating system catalog; entities reference a (name,version) row';

-- ----------------------------------------------------------------------------
-- 6. entities -- the supertype: every server/app/service/device/appliance
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `entities` (
  `id`               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; referenced everywhere',
  `name`             VARCHAR(128) NOT NULL COMMENT 'canonical short name (periodic-table convention), e.g. xenon, cesium; unique among live rows',
  `entity_type`      ENUM('server','workstation','network_gear','appliance','iot_device','peripheral',
                          'vm','container','application','service','mobile_device','vehicle','other')
                     NOT NULL COMMENT 'subtype discriminator; application/service rows have detail rows in applications/services',
  `role`             VARCHAR(255) NULL COMMENT 'human description of what it does (netdb "role" field)',
  `lifecycle`        ENUM('planned','staged','active','offline','retired','decommissioned','unknown')
                     NOT NULL DEFAULT 'active' COMMENT 'lifecycle state (netdb "status"; reserved names = planned)',
  `hardware_unit_id` INT UNSIGNED NULL COMMENT 'FK: physical unit realizing this entity (NULL for apps/services/VMs)',
  `os_id`            INT UNSIGNED NULL COMMENT 'FK: OS currently installed/running (NULL if N/A or unknown)',
  `location_id`      INT UNSIGNED NULL COMMENT 'FK: physical location override; usually derived via hardware_unit',
  `owner_user_id`    BIGINT UNSIGNED NULL COMMENT 'FK (added post-create): responsible person/service account',
  `description`      TEXT NULL COMMENT 'longer free-form description',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_entities_name` (`name`,`deleted_at`) COMMENT 'name unique among live rows; retired rows keep their name',
  KEY `ix_entities_type` (`entity_type`),
  KEY `ix_entities_lifecycle` (`lifecycle`),
  KEY `ix_entities_hw_unit` (`hardware_unit_id`),
  KEY `ix_entities_os` (`os_id`),
  KEY `ix_entities_location` (`location_id`),
  CONSTRAINT `fk_entities_hw_unit`  FOREIGN KEY (`hardware_unit_id`) REFERENCES `hardware_units`(`id`)    ON DELETE SET NULL,
  CONSTRAINT `fk_entities_os`       FOREIGN KEY (`os_id`)            REFERENCES `operating_systems`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_entities_location` FOREIGN KEY (`location_id`)      REFERENCES `locations`(`id`)         ON DELETE SET NULL,
  CONSTRAINT `fk_entities_source`   FOREIGN KEY (`source_id`)        REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Supertype for everything the SoR tracks: servers, devices, appliances, apps, services';

-- ----------------------------------------------------------------------------
-- 7. applications -- subtype detail for entity_type=application
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `applications` (
  `entity_id`     BIGINT UNSIGNED NOT NULL COMMENT 'PK = FK to entities; 1:1 subtype row',
  `vendor`        VARCHAR(128) NULL COMMENT 'publisher/project, e.g. Forgejo, Pi-hole, Grafana Labs',
  `version`       VARCHAR(64)  NULL COMMENT 'currently deployed version string',
  `upstream_url`  VARCHAR(255) NULL COMMENT 'project homepage / release feed',
  `install_kind`  ENUM('package','container','binary','source','saas','firmware','other') NOT NULL DEFAULT 'other'
                  COMMENT 'how it is deployed (informs upgrade procedure)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`entity_id`),
  CONSTRAINT `fk_applications_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_applications_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Subtype detail for application entities (installed software products)';

-- ----------------------------------------------------------------------------
-- 8. services -- subtype detail for entity_type=service
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `services` (
  `entity_id`     BIGINT UNSIGNED NOT NULL COMMENT 'PK = FK to entities; 1:1 subtype row',
  `service_kind`  ENUM('dns','dhcp','ldap','kerberos','sso','http','git','db','monitoring','ntp',
                       'file','print','mqtt','media','automation','vpn','ca','other')
                  NOT NULL DEFAULT 'other' COMMENT 'functional class of the service',
  `is_critical`   TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = homelab-critical (outage pages the operator)',
  `notes`         TEXT NULL COMMENT 'free-form notes (SLAs, failover behavior)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`entity_id`),
  KEY `ix_services_kind` (`service_kind`),
  CONSTRAINT `fk_services_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_services_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Subtype detail for service entities (a function offered on the network)';

-- ----------------------------------------------------------------------------
-- 9. vlans -- layer-2 VLAN registry
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `vlans` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `vlan_tag`      SMALLINT UNSIGNED NOT NULL COMMENT '802.1Q tag, 1-4094',
  `name`          VARCHAR(64)  NOT NULL COMMENT 'VLAN name as configured on the UniFi controller',
  `purpose`       VARCHAR(255) NULL COMMENT 'what traffic belongs here (core, IoT, mgmt...)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_vlans_tag` (`vlan_tag`,`deleted_at`),
  CONSTRAINT `ck_vlans_tag_range` CHECK (`vlan_tag` BETWEEN 1 AND 4094),
  CONSTRAINT `fk_vlans_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='802.1Q VLANs (seeded from UniFi network config)';

-- ----------------------------------------------------------------------------
-- 10. networks -- named network segments (logical grouping above subnets)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `networks` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name`          VARCHAR(64)  NOT NULL COMMENT 'segment name, e.g. core, production, IoT, peripherals, personal, netmgmt, security',
  `purpose`       VARCHAR(255) NULL COMMENT 'what this segment is for; mirrors the netdb comment groupings',
  `vlan_id`       INT UNSIGNED NULL COMMENT 'FK: the VLAN carrying this segment, if VLAN-backed',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_networks_name` (`name`,`deleted_at`),
  KEY `ix_networks_vlan` (`vlan_id`),
  CONSTRAINT `fk_networks_vlan`   FOREIGN KEY (`vlan_id`)   REFERENCES `vlans`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_networks_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Logical network segments (core/production/IoT/...); groups subnets by function';

-- ----------------------------------------------------------------------------
-- 11. subnets -- IP prefixes (the IPAM containers)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `subnets` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `network_id`    INT UNSIGNED NULL COMMENT 'FK: logical segment this prefix belongs to',
  `cidr`          VARCHAR(64) NOT NULL COMMENT 'prefix in CIDR text, e.g. 10.1.0.0/24 (kept as text; INET6 has no prefix type)',
  `net_address`   INET6 NOT NULL COMMENT 'network address parsed out for range queries',
  `prefix_len`    TINYINT UNSIGNED NOT NULL COMMENT 'prefix length parsed out for range queries',
  `gateway_ip`    INET6 NULL COMMENT 'default gateway inside this prefix, if any',
  `description`   VARCHAR(255) NULL COMMENT 'human note about this prefix',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_subnets_cidr` (`cidr`,`deleted_at`),
  KEY `ix_subnets_network` (`network_id`),
  KEY `ix_subnets_netaddr` (`net_address`),
  CONSTRAINT `ck_subnets_prefix` CHECK (`prefix_len` <= 128),
  CONSTRAINT `fk_subnets_network` FOREIGN KEY (`network_id`) REFERENCES `networks`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_subnets_source`  FOREIGN KEY (`source_id`)  REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='IP prefixes (IPAM containers); seeded from UniFi networks + observed /24s';

-- ----------------------------------------------------------------------------
-- 12. dhcp_ranges -- dynamic pools and reservation blocks within a subnet
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `dhcp_ranges` (
  `id`               INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `subnet_id`        INT UNSIGNED NOT NULL COMMENT 'FK: prefix the range lives in',
  `range_start`      INET6 NOT NULL COMMENT 'first address of the range (inclusive)',
  `range_end`        INET6 NOT NULL COMMENT 'last address of the range (inclusive)',
  `kind`             ENUM('dynamic_pool','static_block','excluded') NOT NULL DEFAULT 'dynamic_pool'
                     COMMENT 'dynamic_pool = DHCP hands out; static_block = operator-managed; excluded = never assign',
  `server_entity_id` BIGINT UNSIGNED NULL COMMENT 'FK: entity running the DHCP server for this range (chemistry/UDR7)',
  `notes`            VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_dhcp_ranges_subnet` (`subnet_id`),
  KEY `ix_dhcp_ranges_server` (`server_entity_id`),
  CONSTRAINT `fk_dhcp_ranges_subnet` FOREIGN KEY (`subnet_id`)        REFERENCES `subnets`(`id`)  ON DELETE CASCADE,
  CONSTRAINT `fk_dhcp_ranges_server` FOREIGN KEY (`server_entity_id`) REFERENCES `entities`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dhcp_ranges_source` FOREIGN KEY (`source_id`)        REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='DHCP dynamic pools / static blocks / exclusions inside a subnet (from UniFi)';

-- ----------------------------------------------------------------------------
-- 13. interfaces -- network interfaces, physical and logical
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `interfaces` (
  `id`                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `entity_id`           BIGINT UNSIGNED NOT NULL COMMENT 'FK: the entity this interface belongs to',
  `name`                VARCHAR(64) NOT NULL COMMENT 'interface name on the device, e.g. eth0, wlan0, br0, Port 7; or netdb iface label (tungsten-alt)',
  `if_kind`             ENUM('physical','wifi','vlan','bridge','bond','loopback','tunnel','virtual','switch_port','other')
                        NOT NULL DEFAULT 'physical' COMMENT 'physical NIC vs logical construct',
  `mac`                 CHAR(17) NULL COMMENT 'MAC address aa:bb:cc:dd:ee:ff lowercase; netdb hw field for hosts with known MACs',
  `parent_interface_id` BIGINT UNSIGNED NULL COMMENT 'FK: underlying interface for VLAN/bridge/bond members',
  `vlan_id`             INT UNSIGNED NULL COMMENT 'FK: VLAN for vlan-kind subinterfaces or access-mode switch ports',
  `speed_mbps`          INT UNSIGNED NULL COMMENT 'negotiated or nominal link speed in Mbit/s',
  `mtu`                 SMALLINT UNSIGNED NULL COMMENT 'configured MTU',
  `is_management`       TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = the interface used to manage the device',
  `notes`               VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`           SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`       ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`          DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_interfaces_entity_name` (`entity_id`,`name`,`deleted_at`),
  KEY `ix_interfaces_mac` (`mac`),
  KEY `ix_interfaces_parent` (`parent_interface_id`),
  KEY `ix_interfaces_vlan` (`vlan_id`),
  CONSTRAINT `fk_interfaces_entity` FOREIGN KEY (`entity_id`)           REFERENCES `entities`(`id`)   ON DELETE CASCADE,
  CONSTRAINT `fk_interfaces_parent` FOREIGN KEY (`parent_interface_id`) REFERENCES `interfaces`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_interfaces_vlan`   FOREIGN KEY (`vlan_id`)             REFERENCES `vlans`(`id`)      ON DELETE SET NULL,
  CONSTRAINT `fk_interfaces_source` FOREIGN KEY (`source_id`)           REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Network interfaces (physical NICs, wifi radios, switch ports, VLAN/bridge/bond logicals)';

-- ----------------------------------------------------------------------------
-- 14. interconnects -- cabling and logical links between interfaces/entities
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `interconnects` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `a_interface_id` BIGINT UNSIGNED NULL COMMENT 'FK: endpoint A interface (preferred endpoint form)',
  `b_interface_id` BIGINT UNSIGNED NULL COMMENT 'FK: endpoint B interface (preferred endpoint form)',
  `a_entity_id`    BIGINT UNSIGNED NULL COMMENT 'FK: endpoint A entity, when the exact interface is unknown',
  `b_entity_id`    BIGINT UNSIGNED NULL COMMENT 'FK: endpoint B entity, when the exact interface is unknown',
  `link_kind`      ENUM('copper','fiber','moca','wireless','virtual','lldp_adjacency','logical')
                   NOT NULL DEFAULT 'copper' COMMENT 'physical medium, or discovered/logical adjacency',
  `label`          VARCHAR(128) NULL COMMENT 'cable label / patch id / SSID for wireless',
  `length_m`       DECIMAL(6,2) NULL COMMENT 'cable length in metres, if physical and known',
  `notes`          VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row (lldp_adjacency rows come from solarinet_monitor)',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`     DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  KEY `ix_interconnects_a_if` (`a_interface_id`),
  KEY `ix_interconnects_b_if` (`b_interface_id`),
  KEY `ix_interconnects_a_ent` (`a_entity_id`),
  KEY `ix_interconnects_b_ent` (`b_entity_id`),
  CONSTRAINT `ck_interconnects_a` CHECK (`a_interface_id` IS NOT NULL OR `a_entity_id` IS NOT NULL),
  CONSTRAINT `ck_interconnects_b` CHECK (`b_interface_id` IS NOT NULL OR `b_entity_id` IS NOT NULL),
  CONSTRAINT `fk_interconnects_a_if`  FOREIGN KEY (`a_interface_id`) REFERENCES `interfaces`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_b_if`  FOREIGN KEY (`b_interface_id`) REFERENCES `interfaces`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_a_ent` FOREIGN KEY (`a_entity_id`)    REFERENCES `entities`(`id`)   ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_b_ent` FOREIGN KEY (`b_entity_id`)    REFERENCES `entities`(`id`)   ON DELETE CASCADE,
  CONSTRAINT `fk_interconnects_source` FOREIGN KEY (`source_id`)     REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Physical cabling and logical/discovered links between interfaces (or entities as fallback)';

-- ----------------------------------------------------------------------------
-- 15. ip_addresses -- IPAM: every allocated/reserved address
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `ip_addresses` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `address`       INET6 NOT NULL COMMENT 'the IP address (v4 or v6)',
  `subnet_id`     INT UNSIGNED NULL COMMENT 'FK: containing prefix (kept consistent by application logic)',
  `interface_id`  BIGINT UNSIGNED NULL COMMENT 'FK: interface holding the address, when known',
  `entity_id`     BIGINT UNSIGNED NULL COMMENT 'FK: owning entity; redundant with interface->entity but set when the interface is unknown',
  `assignment`    ENUM('static','dhcp_reservation','dhcp_dynamic','vip','anycast','gateway','reserved','unknown')
                  NOT NULL DEFAULT 'static' COMMENT 'how the address is allocated; reserved = parked for future use (netdb reserved: block)',
  `is_primary`    TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = the entity''s identity address (netdb hosts.ip; drives the A record and PTR)',
  `notes`         VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_ip_addr` (`address`,`deleted_at`) COMMENT 'one live owner per address',
  KEY `ix_ip_subnet` (`subnet_id`),
  KEY `ix_ip_interface` (`interface_id`),
  KEY `ix_ip_entity` (`entity_id`),
  KEY `ix_ip_assignment` (`assignment`),
  CONSTRAINT `fk_ip_subnet`    FOREIGN KEY (`subnet_id`)    REFERENCES `subnets`(`id`)    ON DELETE SET NULL,
  CONSTRAINT `fk_ip_interface` FOREIGN KEY (`interface_id`) REFERENCES `interfaces`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_ip_entity`    FOREIGN KEY (`entity_id`)    REFERENCES `entities`(`id`)   ON DELETE SET NULL,
  CONSTRAINT `fk_ip_source`    FOREIGN KEY (`source_id`)    REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='IPAM: allocations, reservations and observed leases; one live row per address';

-- ----------------------------------------------------------------------------
-- 16. dns_zones -- forward and reverse zones the SoR renders/absorbs
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `dns_zones` (
  `id`                   INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name`                 VARCHAR(255) NOT NULL COMMENT 'zone origin without trailing dot, e.g. akoria.net, 0.1.10.in-addr.arpa, akoria.org',
  `kind`                 ENUM('forward','reverse') NOT NULL COMMENT 'forward or in-addr.arpa/ip6.arpa reverse',
  `authority`            ENUM('sor_rendered','external') NOT NULL DEFAULT 'sor_rendered'
                         COMMENT 'sor_rendered = the SoR is authoritative and gen-zones renders it; external = mirrored (e.g. akoria.org on Samba AD)',
  `primary_ns_entity_id` BIGINT UNSIGNED NULL COMMENT 'FK: entity running the primary nameserver (xenon for akoria.net, radium for akoria.org)',
  `default_ttl`          INT UNSIGNED NOT NULL DEFAULT 300 COMMENT 'zone default TTL in seconds',
  `hostmaster`           VARCHAR(255) NULL COMMENT 'SOA RNAME, e.g. hostmaster.akoria.net.',
  `notes`                VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`            SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`        ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`           DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`           DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`           DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_dns_zones_name` (`name`,`deleted_at`),
  CONSTRAINT `fk_dns_zones_primary_ns` FOREIGN KEY (`primary_ns_entity_id`) REFERENCES `entities`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dns_zones_source`     FOREIGN KEY (`source_id`)            REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='DNS zones; sor_rendered zones are generated from this DB, external zones are mirrored in';

-- ----------------------------------------------------------------------------
-- 17. dns_records -- resource records (both explicit and IPAM-derived)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `dns_records` (
  `id`               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `zone_id`          INT UNSIGNED NOT NULL COMMENT 'FK: owning zone',
  `name`             VARCHAR(255) NOT NULL COMMENT 'owner name relative to zone origin; @ for apex',
  `rrtype`           ENUM('A','AAAA','CNAME','PTR','SRV','TXT','MX','NS','SOA','CAA','DS','DNSKEY')
                     NOT NULL COMMENT 'record type',
  `rdata`            VARCHAR(1024) NOT NULL COMMENT 'record data as zone-file text (address, target FQDN, TXT string, SRV fields...)',
  `ttl`              INT UNSIGNED NULL COMMENT 'per-record TTL; NULL = zone default',
  `priority`         SMALLINT UNSIGNED NULL COMMENT 'MX preference / SRV priority when applicable',
  `ip_address_id`    BIGINT UNSIGNED NULL COMMENT 'FK: for A/AAAA/PTR rows generated from IPAM, the backing address row',
  `target_entity_id` BIGINT UNSIGNED NULL COMMENT 'FK: for functional CNAMEs, the entity the alias points at (netdb cnames)',
  `is_generated`     TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = derivable from ip_addresses/entities (A/PTR); 0 = explicitly asserted (CNAME/TXT/SRV...)',
  `notes`            VARCHAR(255) NULL COMMENT 'free-form note (e.g. "legacy alias family devices know")',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_dns_records` (`zone_id`,`name`,`rrtype`,`rdata`(255),`deleted_at`),
  KEY `ix_dns_records_name` (`name`),
  KEY `ix_dns_records_ip` (`ip_address_id`),
  KEY `ix_dns_records_target` (`target_entity_id`),
  CONSTRAINT `fk_dns_records_zone`   FOREIGN KEY (`zone_id`)          REFERENCES `dns_zones`(`id`)    ON DELETE CASCADE,
  CONSTRAINT `fk_dns_records_ip`     FOREIGN KEY (`ip_address_id`)    REFERENCES `ip_addresses`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_dns_records_target` FOREIGN KEY (`target_entity_id`) REFERENCES `entities`(`id`)     ON DELETE SET NULL,
  CONSTRAINT `fk_dns_records_source` FOREIGN KEY (`source_id`)        REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='DNS resource records; DNS zone files become a rendered VIEW of this table';

-- ----------------------------------------------------------------------------
-- 18. users -- people and service accounts (tied to Samba AD)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `users` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `username`      VARCHAR(128) NOT NULL COMMENT 'sAMAccountName / login name',
  `realm`         VARCHAR(128) NOT NULL DEFAULT 'akoria.org' COMMENT 'authentication realm: akoria.org (AD) or local (app-local accounts)',
  `kind`          ENUM('person','service_account','machine_account') NOT NULL DEFAULT 'person'
                  COMMENT 'humans vs service principals vs AD computer accounts',
  `display_name`  VARCHAR(255) NULL COMMENT 'full name / friendly name',
  `email`         VARCHAR(255) NULL COMMENT 'primary email address',
  `upn`           VARCHAR(255) NULL COMMENT 'userPrincipalName from AD',
  `ad_guid`       CHAR(36) NULL COMMENT 'AD objectGUID (stable identity across renames)',
  `ad_sid`        VARCHAR(64) NULL COMMENT 'AD objectSid',
  `enabled`       TINYINT(1) NOT NULL DEFAULT 1 COMMENT '0 = account disabled in the source directory',
  `notes`         TEXT NULL COMMENT 'free-form notes',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_users_username_realm` (`username`,`realm`,`deleted_at`),
  UNIQUE KEY `uq_users_ad_guid` (`ad_guid`),
  KEY `ix_users_kind` (`kind`),
  CONSTRAINT `fk_users_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='People, service accounts, and AD machine accounts; mirrored from Samba AD';

-- entities.owner_user_id FK could not be declared before users existed; add it now.
-- (Guarded so re-running the file stays idempotent.)
SET @fk_exists := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
                   WHERE CONSTRAINT_SCHEMA = DATABASE()
                     AND TABLE_NAME = 'entities' AND CONSTRAINT_NAME = 'fk_entities_owner');
SET @ddl := IF(@fk_exists = 0,
  'ALTER TABLE `entities` ADD CONSTRAINT `fk_entities_owner` FOREIGN KEY (`owner_user_id`) REFERENCES `users`(`id`) ON DELETE SET NULL',
  'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- ----------------------------------------------------------------------------
-- 19. groups -- security/role groups (tied to Samba AD)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `groups` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name`          VARCHAR(128) NOT NULL COMMENT 'group name (sAMAccountName for AD groups)',
  `realm`         VARCHAR(128) NOT NULL DEFAULT 'akoria.org' COMMENT 'akoria.org (AD) or local',
  `kind`          ENUM('security','distribution','role','local') NOT NULL DEFAULT 'security'
                  COMMENT 'AD group type, or role = SolariNet-internal RBAC group',
  `ad_guid`       CHAR(36) NULL COMMENT 'AD objectGUID',
  `ad_sid`        VARCHAR(64) NULL COMMENT 'AD objectSid',
  `description`   VARCHAR(255) NULL COMMENT 'group purpose',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_groups_name_realm` (`name`,`realm`,`deleted_at`),
  UNIQUE KEY `uq_groups_ad_guid` (`ad_guid`),
  CONSTRAINT `fk_groups_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Security/distribution/role groups; mirrored from Samba AD plus local RBAC roles';

-- ----------------------------------------------------------------------------
-- 20. group_members -- M:N users<->groups, plus nested groups
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `group_members` (
  `id`              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `group_id`        BIGINT UNSIGNED NOT NULL COMMENT 'FK: the containing group',
  `member_user_id`  BIGINT UNSIGNED NULL COMMENT 'FK: user member (exactly one of member_user_id/member_group_id set)',
  `member_group_id` BIGINT UNSIGNED NULL COMMENT 'FK: nested-group member (AD supports group nesting)',
  `source_id`       SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`   ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`      DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_group_members_user`  (`group_id`,`member_user_id`,`deleted_at`),
  UNIQUE KEY `uq_group_members_group` (`group_id`,`member_group_id`,`deleted_at`),
  KEY `ix_group_members_user` (`member_user_id`),
  KEY `ix_group_members_group` (`member_group_id`),
  CONSTRAINT `ck_group_members_one_kind`
    CHECK ((`member_user_id` IS NULL) <> (`member_group_id` IS NULL)),
  CONSTRAINT `fk_group_members_group`  FOREIGN KEY (`group_id`)        REFERENCES `groups`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_group_members_user`   FOREIGN KEY (`member_user_id`)  REFERENCES `users`(`id`)  ON DELETE CASCADE,
  CONSTRAINT `fk_group_members_nested` FOREIGN KEY (`member_group_id`) REFERENCES `groups`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_group_members_source` FOREIGN KEY (`source_id`)       REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Group membership junction: users in groups, and nested groups in groups';

-- ----------------------------------------------------------------------------
-- 21. certificates -- X.509 certificate metadata (never private keys)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `certificates` (
  `id`                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `subject_cn`         VARCHAR(255) NOT NULL COMMENT 'subject common name',
  `sans`               TEXT NULL COMMENT 'subjectAltNames, comma-separated',
  `issuer`             VARCHAR(255) NULL COMMENT 'issuer DN or friendly CA name (Let''s Encrypt, internal CA...)',
  `serial_hex`         VARCHAR(64) NULL COMMENT 'certificate serial number in hex',
  `fingerprint_sha256` CHAR(64) NOT NULL COMMENT 'SHA-256 fingerprint, lowercase hex, no colons; the identity key',
  `key_algo`           VARCHAR(32) NULL COMMENT 'public key algorithm + size, e.g. RSA-2048, ECDSA-P256',
  `not_before`         DATETIME NULL COMMENT 'validity start (UTC)',
  `not_after`          DATETIME NULL COMMENT 'validity end (UTC); drives expiry alerting',
  `is_ca`              TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = CA certificate',
  `notes`              VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`          SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`      ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`         DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_certificates_fp` (`fingerprint_sha256`),
  KEY `ix_certificates_not_after` (`not_after`),
  KEY `ix_certificates_cn` (`subject_cn`),
  CONSTRAINT `fk_certificates_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='X.509 certificate metadata; the cert/key material itself lives on the entities that use it';

-- ----------------------------------------------------------------------------
-- 22. certificate_bindings -- where each certificate is deployed (M:N)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `certificate_bindings` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `certificate_id` BIGINT UNSIGNED NOT NULL COMMENT 'FK: the certificate',
  `entity_id`      BIGINT UNSIGNED NOT NULL COMMENT 'FK: entity (host/service) where the cert is installed',
  `cert_usage`     ENUM('tls_server','tls_client','ca_trust','code_signing','other') NOT NULL DEFAULT 'tls_server'
                   COMMENT 'how the certificate is used at this deployment',
  `locator`        VARCHAR(255) NULL COMMENT 'where on the entity it lives, e.g. /etc/ssl/private/..., k8s secret name',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`     DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_cert_bindings` (`certificate_id`,`entity_id`,`cert_usage`,`deleted_at`),
  KEY `ix_cert_bindings_entity` (`entity_id`),
  CONSTRAINT `fk_cert_bindings_cert`   FOREIGN KEY (`certificate_id`) REFERENCES `certificates`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_cert_bindings_entity` FOREIGN KEY (`entity_id`)      REFERENCES `entities`(`id`)     ON DELETE CASCADE,
  CONSTRAINT `fk_cert_bindings_source` FOREIGN KEY (`source_id`)      REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='M:N deployment map: which entities serve/trust which certificates, and where the files live';

-- ----------------------------------------------------------------------------
-- 23. secrets -- metadata + pointers for keys/tokens/passwords (NO material)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `secrets` (
  `id`              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name`            VARCHAR(128) NOT NULL COMMENT 'stable secret name, e.g. forgejo-admin-token, solari-db-password',
  `kind`            ENUM('password','api_token','ssh_key','tls_private_key','shared_key','kerberos_keytab',
                         'totp_seed','webhook_secret','other') NOT NULL COMMENT 'what kind of secret this is',
  `fingerprint`     VARCHAR(128) NULL COMMENT 'non-reversible identifier: SSH key fingerprint, token prefix, hash; never the material',
  `vault_entity_id` BIGINT UNSIGNED NULL COMMENT 'FK: entity where the secret material actually lives (a host, a password manager service...)',
  `locator`         VARCHAR(255) NULL COMMENT 'path/reference inside the vault entity, e.g. /root/.mariadb-root-pw, 1Password item id',
  `owner_user_id`   BIGINT UNSIGNED NULL COMMENT 'FK: responsible user / the account the secret authenticates',
  `used_by_entity_id` BIGINT UNSIGNED NULL COMMENT 'FK: entity that consumes the secret, if a single main consumer',
  `rotated_at`      DATETIME NULL COMMENT 'when the secret was last rotated',
  `expires_at`      DATETIME NULL COMMENT 'expiry, if the secret has one (drives rotation alerting)',
  `notes`           VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`       SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`   ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`      DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_secrets_name` (`name`,`deleted_at`),
  KEY `ix_secrets_vault` (`vault_entity_id`),
  KEY `ix_secrets_owner` (`owner_user_id`),
  KEY `ix_secrets_consumer` (`used_by_entity_id`),
  KEY `ix_secrets_expires` (`expires_at`),
  CONSTRAINT `fk_secrets_vault`    FOREIGN KEY (`vault_entity_id`)   REFERENCES `entities`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_secrets_owner`    FOREIGN KEY (`owner_user_id`)     REFERENCES `users`(`id`)    ON DELETE SET NULL,
  CONSTRAINT `fk_secrets_consumer` FOREIGN KEY (`used_by_entity_id`) REFERENCES `entities`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_secrets_source`   FOREIGN KEY (`source_id`)         REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Secret INVENTORY only: what exists, where it lives, when it rotates. Material never stored here';

-- ----------------------------------------------------------------------------
-- 24. repositories -- git repositories (Forgejo on cesium)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `repositories` (
  `id`               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `owner_name`       VARCHAR(128) NOT NULL COMMENT 'Forgejo owner (user or org) as text; may also map to users via owner_user_id',
  `name`             VARCHAR(128) NOT NULL COMMENT 'repository name',
  `owner_user_id`    BIGINT UNSIGNED NULL COMMENT 'FK: owning user when the Forgejo owner maps to a directory user',
  `forge_entity_id`  BIGINT UNSIGNED NULL COMMENT 'FK: the forge service entity hosting the repo (Forgejo on cesium)',
  `clone_url`        VARCHAR(255) NULL COMMENT 'canonical clone URL',
  `default_branch`   VARCHAR(128) NULL COMMENT 'default branch name',
  `visibility`       ENUM('public','private','internal') NOT NULL DEFAULT 'private' COMMENT 'repo visibility on the forge',
  `is_archived`      TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = archived on the forge',
  `is_mirror`        TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = pull/push mirror of an external repo',
  `description`      VARCHAR(255) NULL COMMENT 'repo description from the forge',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_repositories_owner_name` (`owner_name`,`name`,`deleted_at`),
  KEY `ix_repositories_forge` (`forge_entity_id`),
  KEY `ix_repositories_owner_user` (`owner_user_id`),
  CONSTRAINT `fk_repositories_owner_user` FOREIGN KEY (`owner_user_id`)   REFERENCES `users`(`id`)    ON DELETE SET NULL,
  CONSTRAINT `fk_repositories_forge`      FOREIGN KEY (`forge_entity_id`) REFERENCES `entities`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_repositories_source`     FOREIGN KEY (`source_id`)       REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Git repositories, mirrored from Forgejo (git.akoria.net on cesium)';

-- ----------------------------------------------------------------------------
-- 25. service_endpoints -- where a service listens (proto/port/url per IP)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `service_endpoints` (
  `id`                BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `service_entity_id` BIGINT UNSIGNED NOT NULL COMMENT 'FK: the service (entities row, usually entity_type=service or application)',
  `ip_address_id`     BIGINT UNSIGNED NULL COMMENT 'FK: specific listening address; NULL = all addresses of the host',
  `protocol`          ENUM('tcp','udp','icmp','http','https','other') NOT NULL DEFAULT 'tcp' COMMENT 'transport/application protocol',
  `port`              SMALLINT UNSIGNED NULL COMMENT 'listening port; NULL for portless (icmp)',
  `url`               VARCHAR(255) NULL COMMENT 'canonical URL for web endpoints, e.g. https://git.akoria.net',
  `notes`             VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`         SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`     ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`        DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_service_endpoints` (`service_entity_id`,`ip_address_id`,`protocol`,`port`,`deleted_at`),
  KEY `ix_service_endpoints_ip` (`ip_address_id`),
  CONSTRAINT `fk_service_endpoints_service` FOREIGN KEY (`service_entity_id`) REFERENCES `entities`(`id`)     ON DELETE CASCADE,
  CONSTRAINT `fk_service_endpoints_ip`      FOREIGN KEY (`ip_address_id`)     REFERENCES `ip_addresses`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_service_endpoints_source`  FOREIGN KEY (`source_id`)         REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Listening endpoints (proto/port/url) that expose a service; feeds probe targets';

-- ----------------------------------------------------------------------------
-- 26. monitoring_state -- CURRENT health per entity (history stays in the
--     monitoring DB; the SoR keeps only the authoritative current state)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `monitoring_state` (
  `entity_id`     BIGINT UNSIGNED NOT NULL COMMENT 'PK = FK to entities; one current-state row per entity',
  `status`        ENUM('up','degraded','down','unknown','maintenance','retired') NOT NULL DEFAULT 'unknown'
                  COMMENT 'rolled-up health as last computed by SolariNet monitoring',
  `first_seen`    DATETIME NULL COMMENT 'when monitoring first observed this entity',
  `last_seen`     DATETIME NULL COMMENT 'last time the entity answered a probe / sent a report',
  `last_checked`  DATETIME NULL COMMENT 'last time monitoring evaluated the entity (even if unreachable)',
  `agent_version` VARCHAR(32) NULL COMMENT 'solariMonitor agent version, if agent-enrolled',
  `detail`        JSON NULL COMMENT 'source-specific detail blob (load, open issues, failing probes)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row (normally solarinet_monitor)',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`entity_id`),
  KEY `ix_monitoring_state_status` (`status`),
  KEY `ix_monitoring_state_last_seen` (`last_seen`),
  CONSTRAINT `fk_monitoring_state_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_monitoring_state_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Current health per entity, synced from the SolariNet monitoring DB (history stays there)';

-- ----------------------------------------------------------------------------
-- 27. config_items -- configuration key/values per entity (and per component)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `config_items` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `entity_id`     BIGINT UNSIGNED NOT NULL COMMENT 'FK: entity the setting applies to',
  `scope`         VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'component/namespace within the entity, e.g. sshd, bind, solari-agent; empty = entity-global',
  `cfg_key`       VARCHAR(191) NOT NULL COMMENT 'configuration key, dotted style, e.g. dns.forwarders, agent.interval_s',
  `cfg_value`     TEXT NULL COMMENT 'value serialized as text/JSON; NULL only when secret_id is set',
  `value_type`    ENUM('string','int','float','bool','json','secret_ref') NOT NULL DEFAULT 'string'
                  COMMENT 'how to parse cfg_value; secret_ref means the value is the secret referenced by secret_id',
  `secret_id`     BIGINT UNSIGNED NULL COMMENT 'FK: when the value is sensitive, point at the secrets inventory instead of storing it',
  `notes`         VARCHAR(255) NULL COMMENT 'free-form note (why this override exists)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_config_items` (`entity_id`,`scope`,`cfg_key`,`deleted_at`),
  KEY `ix_config_items_key` (`cfg_key`),
  KEY `ix_config_items_secret` (`secret_id`),
  CONSTRAINT `fk_config_items_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_config_items_secret` FOREIGN KEY (`secret_id`) REFERENCES `secrets`(`id`)  ON DELETE SET NULL,
  CONSTRAINT `fk_config_items_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Per-entity (and per-component) configuration key/values; sensitive values point into secrets';

-- ----------------------------------------------------------------------------
-- 28. relationships -- generic typed entity<->entity graph
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `relationships` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `from_entity_id` BIGINT UNSIGNED NOT NULL COMMENT 'FK: subject of the relationship',
  `to_entity_id`   BIGINT UNSIGNED NOT NULL COMMENT 'FK: object of the relationship',
  `rel_type`       ENUM('contains','runs_on','depends_on','member_of','connects_to','backs_up',
                        'manages','monitors','replicates_to','peers_with') NOT NULL
                   COMMENT 'read as: FROM <rel_type> TO. e.g. forgejo runs_on cesium; pihole-secondary replicates_to pihole-primary',
  `notes`          VARCHAR(255) NULL COMMENT 'free-form note qualifying the edge',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`     DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_relationships` (`from_entity_id`,`to_entity_id`,`rel_type`,`deleted_at`),
  KEY `ix_relationships_to` (`to_entity_id`),
  KEY `ix_relationships_type` (`rel_type`),
  CONSTRAINT `ck_relationships_no_self` CHECK (`from_entity_id` <> `to_entity_id`),
  CONSTRAINT `fk_relationships_from`   FOREIGN KEY (`from_entity_id`) REFERENCES `entities`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_relationships_to`     FOREIGN KEY (`to_entity_id`)   REFERENCES `entities`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_relationships_source` FOREIGN KEY (`source_id`)      REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Typed entity graph: contains / runs_on / depends_on / member_of / connects_to / ...';

-- ----------------------------------------------------------------------------
-- 29. external_refs -- map SoR rows to their IDs in source systems
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `external_refs` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'FK: which source system the external id belongs to',
  `subject_table` VARCHAR(64) NOT NULL COMMENT 'SoR table the ref points at, e.g. entities, users, repositories',
  `subject_id`    BIGINT UNSIGNED NOT NULL COMMENT 'PK value of the row in subject_table (polymorphic; no FK possible)',
  `external_id`   VARCHAR(255) NOT NULL COMMENT 'the row''s identity in the source system: UniFi device _id, Forgejo repo id, AD objectGUID, monitoring node.id',
  `external_url`  VARCHAR(255) NULL COMMENT 'deep link into the source system''s UI, if any',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_external_refs` (`source_id`,`subject_table`,`external_id`,`deleted_at`),
  KEY `ix_external_refs_subject` (`subject_table`,`subject_id`),
  CONSTRAINT `fk_external_refs_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Join table between SoR rows and their native IDs in each source system; the sync engine''s correlation key';

-- ----------------------------------------------------------------------------
-- 30. audit_log -- append-only change history for every table
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `audit_log` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK, monotonically increasing',
  `table_name`     VARCHAR(64) NOT NULL COMMENT 'SoR table that changed',
  `row_id`         BIGINT UNSIGNED NOT NULL COMMENT 'PK of the changed row in table_name',
  `action`         ENUM('insert','update','soft_delete','undelete','hard_delete') NOT NULL COMMENT 'what happened',
  `actor_user_id`  BIGINT UNSIGNED NULL COMMENT 'FK: the logged-in user, for dashboard/human changes',
  `actor_label`    VARCHAR(128) NULL COMMENT 'free-text actor for machine changes, e.g. sync:unifi, seed:netdb',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'FK: source system on whose behalf the change was made',
  `asserted_kind`  ENUM('human','machine') NOT NULL COMMENT 'human- or machine-driven change',
  `old_values`     JSON NULL COMMENT 'changed columns, previous values (NULL on insert)',
  `new_values`     JSON NULL COMMENT 'changed columns, new values (NULL on delete)',
  `note`           VARCHAR(255) NULL COMMENT 'optional operator-supplied change reason',
  `changed_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'when the change was recorded',
  PRIMARY KEY (`id`),
  KEY `ix_audit_row` (`table_name`,`row_id`,`changed_at`),
  KEY `ix_audit_changed_at` (`changed_at`),
  KEY `ix_audit_actor` (`actor_user_id`),
  CONSTRAINT `fk_audit_actor`  FOREIGN KEY (`actor_user_id`) REFERENCES `users`(`id`)   ON DELETE SET NULL,
  CONSTRAINT `fk_audit_source` FOREIGN KEY (`source_id`)     REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Append-only who/what/when/old->new change history; written by the application layer on every write';

-- ============================================================================
-- Convenience views for the DNS generator (gen-zones.py load_source() seam).
-- These reproduce exactly the (hosts, ifaces, cnames) shapes the renderer eats.
-- Note: INET6 stores IPv4 as v4-mapped (::ffff:a.b.c.d); the views normalize
-- back to dotted-quad text so zone files render correctly.
-- ============================================================================

CREATE OR REPLACE VIEW `v_dns_hosts` AS
SELECT e.`name`                              AS `host`,
       CASE WHEN CAST(ip.`address` AS CHAR) LIKE '::ffff:%.%'
            THEN SUBSTRING(CAST(ip.`address` AS CHAR), 8)
            ELSE CAST(ip.`address` AS CHAR) END AS `ip`,
       e.`role`                              AS `role`,
       e.`lifecycle`                         AS `lifecycle`
FROM `entities` e
JOIN `ip_addresses` ip ON ip.`entity_id` = e.`id`
                       AND ip.`is_primary` = 1
                       AND ip.`deleted_at` IS NULL
WHERE e.`deleted_at` IS NULL
  AND e.`lifecycle` NOT IN ('retired','decommissioned');

CREATE OR REPLACE VIEW `v_dns_ifaces` AS
SELECT CONCAT(e.`name`, '-', i.`name`)       AS `label`,
       CASE WHEN CAST(ip.`address` AS CHAR) LIKE '::ffff:%.%'
            THEN SUBSTRING(CAST(ip.`address` AS CHAR), 8)
            ELSE CAST(ip.`address` AS CHAR) END AS `ip`
FROM `ip_addresses` ip
JOIN `interfaces` i ON i.`id` = ip.`interface_id` AND i.`deleted_at` IS NULL
JOIN `entities`  e ON e.`id` = i.`entity_id`      AND e.`deleted_at` IS NULL
WHERE ip.`deleted_at` IS NULL
  AND ip.`is_primary` = 0;

CREATE OR REPLACE VIEW `v_dns_cnames` AS
SELECT r.`name`                              AS `alias`,
       e.`name`                              AS `target_host`
FROM `dns_records` r
JOIN `dns_zones` z ON z.`id` = r.`zone_id` AND z.`deleted_at` IS NULL
LEFT JOIN `entities` e ON e.`id` = r.`target_entity_id`
WHERE r.`rrtype` = 'CNAME'
  AND r.`deleted_at` IS NULL;

-- ============================================================================
-- End of schema.
-- ============================================================================
