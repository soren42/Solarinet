-- 016_push_subscriptions.sql - browser Web Push subscriptions.
-- Additive and safe to apply repeatedly on MariaDB/MySQL.

CREATE TABLE IF NOT EXISTS push_subscriptions (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  operator VARCHAR(255) NULL,
  endpoint VARCHAR(512) NOT NULL,
  p256dh VARCHAR(255) NOT NULL,
  auth VARCHAR(255) NOT NULL,
  userAgent VARCHAR(255) NULL,
  createdAt DATETIME NOT NULL,
  lastSeenAt DATETIME NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_push_subscriptions_endpoint (endpoint)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
