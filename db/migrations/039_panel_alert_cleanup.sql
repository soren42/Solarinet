-- One-shot alertEvent repair for panel v2. Reviewed for operator execution;
-- intentionally not run by this migration authoring task.

-- Keep the oldest still-open event for each alert condition.
UPDATE alertEvent AS newer
JOIN alertEvent AS older
  ON older.ruleId <=> newer.ruleId
 AND older.nodeId <=> newer.nodeId
 AND older.targetId <=> newer.targetId
 AND older.eventId < newer.eventId
 AND older.eventKind = 'fired'
 AND older.clearedAt IS NULL
SET newer.clearedAt = UTC_TIMESTAMP()
WHERE newer.clearedAt IS NULL
  AND newer.ruleId IS NOT NULL
  AND newer.eventKind = 'fired';

-- Close open reachability conditions whose probe target has been removed.
UPDATE alertEvent AS e
JOIN alertRule AS r ON r.ruleId = e.ruleId
LEFT JOIN probeTarget AS pt
  ON SUBSTR(pt.targetId, INSTR(pt.targetId, ':') + 1)
   = SUBSTR(e.targetId, INSTR(e.targetId, ':') + 1)
SET e.clearedAt = UTC_TIMESTAMP()
WHERE e.clearedAt IS NULL
  AND e.eventKind = 'fired'
  AND r.metric = 'reachable'
  AND e.targetId IS NOT NULL
  AND pt.targetId IS NULL;

-- An old audit note stored as a fired event: no rule, so nothing can ever clear it.
UPDATE alertEvent
SET clearedAt = UTC_TIMESTAMP()
WHERE clearedAt IS NULL
  AND eventKind = 'fired'
  AND ruleId IS NULL
  AND detail LIKE 'asset removed by %';
