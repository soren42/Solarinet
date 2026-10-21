-- Remove confirmation tokens left by the former two-row dashboard handshake.
-- Pending rows that contain a token cannot safely be replayed after redaction.
UPDATE ctlRequest
SET state = 'failed',
    error = 'ERR 7 legacy decommission request redacted; submit a new request',
    completedAt = COALESCE(completedAt, CURRENT_TIMESTAMP(6)),
    argsWire = '[redacted legacy decommission request]'
WHERE verb = 'DECOMMISSION'
  AND state IN ('pending', 'claimed');

UPDATE ctlRequest
SET argsWire = '[redacted legacy decommission request]'
WHERE verb = 'DECOMMISSION'
  AND argsWire REGEXP '(^| )confirm=[0-9]+';

UPDATE ctlRequest
SET result = '[redacted legacy decommission confirmation]'
WHERE verb = 'DECOMMISSION'
  AND result REGEXP 'confirm=[0-9]+';

UPDATE ctlRequest
SET error = '[redacted legacy decommission confirmation]'
WHERE verb = 'DECOMMISSION'
  AND error REGEXP 'confirm=[0-9]+';
