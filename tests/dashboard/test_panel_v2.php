<?php
declare(strict_types=1);

class Response
{
    public static function error(string $code, string $message, int $status): void
    {
        throw new RuntimeException($code . ':' . $status . ':' . $message);
    }
}

require __DIR__ . '/../../dashboard/api/routes/panel.php';

function check(bool $ok, string $label): void
{
    if (!$ok) {
        fwrite(STDERR, "FAIL: {$label}\n");
        exit(1);
    }
    echo "ok: {$label}\n";
}

check(panelShortHost('Xenon.SolariNet.local') === 'XENON', 'short first DNS label');
check(panelResourcePct(7, 10) === 70, 'RAM percentage');
check(panelDiskPct('[{"mount":"/","totalKb":100,"usedKb":70},{"mount":"/data","totalKb":100,"usedKb":93}]') === 93, 'fullest filesystem percentage');
check(panelAlertMessage('reachable', '', 0, '') === 'NO RESPONSE', 'reachable text');
check(panelAlertMessage('health.fsReadonly', '', 0, '') === 'DISK READONLY', 'readonly text');
check(panelAlertMessage('health.failedUnits', '', 2, '') === '2 SVCS FAILED', 'multiple units text');
check(panelAlertMessage('health.failedUnits', '', 1, '["sshd.service"]') === 'SSHD FAILED', 'single unit text');

$state = panelStateInput(['theme'=>3, 'screen'=>3, 'brightnessPct'=>80, 'autoBright'=>1,
    'sleeping'=>0, 'alarmArmed'=>1, 'alarmAcked'=>0, 'dwellSec'=>6,
    'ackedEpisodeId'=>0, 'lastCmdId'=>0]);
check($state['screen'] === 3, 'screen 3 accepted');
check(panelCommandInput(['kind'=>2, 'arg'=>3]) === [2, 3], 'SETSCREEN 0..3 accepted');
check(panelCommandInput(['kind'=>8, 'arg'=>31]) === [8, 31], 'SCREENEN slot 15 accepted');
check(panelCommandInput(['kind'=>9, 'arg'=>125]) === [9, 125], 'SCREENWT slot 15 accepted');

$entry = ['enabled'=>1, 'weightCode'=>5];
[$cfg12] = panelConfigInput(['screenCfg'=>array_fill(0, 12, $entry), 'flags'=>1]);
[$cfg16] = panelConfigInput(['screenCfg'=>array_fill(0, 16, $entry), 'flags'=>1]);
check(count(json_decode($cfg12, true)) === 12, 'legacy 12-entry CONFIG accepted');
check(count(json_decode($cfg16, true)) === 16, 'v2 16-entry CONFIG accepted');
echo "panel v2 helpers passed\n";
