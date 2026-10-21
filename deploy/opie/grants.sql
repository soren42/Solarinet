-- Least-privilege grants required by deploy/opie/opied.py.
-- Create the account separately through the protected DB credential workflow;
-- then apply these grants for the account and host used in opie.conf.example.
-- Do not grant database-wide privileges or reuse the `solari` account.
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'solari-opie'@'127.0.0.1';

GRANT SELECT (eventId, ruleId, nodeId, severity, firedAt, detail)
  ON `solarinet`.`alertEvent` TO 'solari-opie'@'127.0.0.1';
GRANT SELECT (ruleId, metric, scope)
  ON `solarinet`.`alertRule` TO 'solari-opie'@'127.0.0.1';
GRANT SELECT (nodeId, hostFqdn)
  ON `solarinet`.`node` TO 'solari-opie'@'127.0.0.1';
GRANT SELECT (status, startsAt, endsAt, scope, nodeId)
  ON `solarinet`.`maintenanceWindow` TO 'solari-opie'@'127.0.0.1';
GRANT SELECT (reportId, incidentKey, startedAt),
      INSERT (incidentKey, triggerKind, firstEventId, nodeId, hostFqdn, severity, status),
      UPDATE (status, summary, analysis, model, durationSec, finishedAt)
  ON `solarinet`.`opieReport` TO 'solari-opie'@'127.0.0.1';
