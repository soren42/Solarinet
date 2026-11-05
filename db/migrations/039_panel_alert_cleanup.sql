-- One-shot alertEvent repair for panel v2. Reviewed for operator execution;
-- intentionally not run by this migration authoring task.

-- Before August the engine wrote a clear as another 'fired' row, so the condition's
-- rows stayed open after it cleared. A condition whose newest row is a clear is over.
UPDATE alertEvent AS e
JOIN (
  SELECT ruleId, nodeId, targetId, MAX(eventId) AS lastId
  FROM alertEvent
  WHERE ruleId IS NOT NULL AND eventKind IN ('fired', 'cleared')
  GROUP BY ruleId, nodeId, targetId
) AS cond
  ON cond.ruleId = e.ruleId
 AND cond.nodeId <=> e.nodeId
 AND cond.targetId <=> e.targetId
JOIN alertEvent AS newest ON newest.eventId = cond.lastId
SET e.clearedAt = UTC_TIMESTAMP()
WHERE e.clearedAt IS NULL
  AND e.eventKind = 'fired'
  AND newest.detail LIKE 'CLEARED %';

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

-- Close open reachability conditions whose probe target has been removed. An event
-- names a target <ordinal>:<host>:<port> (1 ICMP, 2 TCP); probeTarget also holds
-- icmp:<host> and tcp:<host>:<port>.
UPDATE alertEvent AS e
JOIN alertRule AS r ON r.ruleId = e.ruleId
LEFT JOIN probeTarget AS pt
  ON pt.targetId IN (
       e.targetId,
       CONCAT('icmp:', SUBSTRING_INDEX(SUBSTRING_INDEX(e.targetId, ':', 2), ':', -1)),
       CONCAT('tcp:', SUBSTRING_INDEX(e.targetId, ':', -2)))
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
