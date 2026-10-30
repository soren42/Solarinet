-- Ingest now validates every frame's peer certificate CN against node.certCn,
-- and enrolled certs carry CN "<role>.<hostFqdn>". Nodes enrolled before that
-- rule stored the bare hostFqdn and are rejected as "not enrolled" until their
-- next HELLO rewrites certCn. Backfill the role prefix. Idempotent: rows that
-- already carry the prefix no longer match certCn = hostFqdn.
UPDATE node
   SET certCn = CONCAT(role, '.', hostFqdn)
 WHERE certCn = hostFqdn
   AND state <> 'retired';
