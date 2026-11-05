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
check(panelShortHost('10.1.0.200') === '10.1.0.200', 'an address keeps its digits');
check(panelPoolName('Core Infrastructure') === 'CORE', 'pool label is the first word');
check(panelPoolName('Monitoring') === 'MONITOR', 'pool label cut without a dot');
check(panelPoolName('') === 'NO POOL', 'unpooled label');
check(panelProbeName('10.1.0.200', null) === '1.0.200', 'probe address tail fits eight glyphs');
check(panelProbeName('chemistry', 'x') === 'CHEMISTR', 'probe name cut without a dot');
check(panelAlertTarget('2:10.0.0.20:443') === ['10.0.0.20', 443], 'alert target host and port');
check(panelAlertTarget('1:10.0.0.1') === ['10.0.0.1', null], 'icmp alert target has no port');
check(panelAlertTarget('2:fd1f:0000:0000:0000:0000:0000:0000:0001:443') === ['fd1f:0000:0000:0000:0000:0000:0000:0001', 443], 'ipv6 host keeps its colons');
check(panelAlertTarget('1:10.0.0.1:0') === ['10.0.0.1', 0], 'stored icmp target carries port 0');
check(panelAlertTarget('1:2001:db8::1:0') === ['2001:db8::1', 0], 'stored ipv6 icmp target keeps its host');
check(panelAlertTarget(null) === [null, null], 'node alert has no target');
check(panelAlertMessage('reachable', '', 0, '', 443) === 'HTTPS DOWN', 'named service text');
check(panelAlertMessage('reachable', '', 0, '', 8443) === 'PORT 8443 DOWN', 'unnamed port text');
check(panelAlertMessage(null, '', 0, '') === 'ALERT', 'unknown metric still says something');
check(panelLinkState(1) === 0 && panelLinkState(2) === 2 && panelLinkState(null) === 3, 'link state is PanelState');
check(panelStateCode('up') === 0 && panelStateCode('down') === 2 && panelStateCode('x') === 3, 'host link state');

$now = 1000000;
$probe = static fn (string $id, string $host, string $proto, ?string $outcome, ?int $rtt, ?int $ts, ?int $loss = 0): array =>
    ['targetId' => $id, 'host' => $host, 'proto' => $proto, 'displayName' => null,
     'outcome' => $outcome, 'rttMicros' => $rtt, 'lossPermille' => $loss, 'sampledTs' => $ts];
$summary = panelProbeSummary([
    $probe('icmp:10.0.0.9', '10.0.0.9', 'icmp', 'timeout', null, $now, 1000),   // dead 24 h
    $probe('icmp:10.0.0.1', '10.0.0.1', 'icmp', 'ok', 1000, $now),              // two vantages
    $probe('icmp:10.0.0.1', '10.0.0.1', 'icmp', 'timeout', null, $now, 1000),
    $probe('tcp:10.0.0.1:443', '10.0.0.1', 'tcp', 'timeout', null, $now, 1000), // service failing
    $probe('icmp:10.0.0.2', '10.0.0.2', 'icmp', 'timeout', null, $now, 1000),   // failing now
    $probe('icmp:10.0.0.3', '10.0.0.3', 'icmp', 'ok', 3000, $now - 301),        // stale sample
    $probe('tcp:10.0.0.4:22', '10.0.0.4', 'tcp', 'ok', 500, $now),              // TCP-only host
], ['icmp:10.0.0.2' => true, 'tcp:10.0.0.1:443' => true], $now);
$rows = array_column($summary['probes'], null, 'name');
check(array_column($summary['probes'], 'name') === ['10.0.0.1', '10.0.0.2', '10.0.0.4', '10.0.0.3', '10.0.0.9'], 'alive first, ICMP first, dead last');
check($rows['10.0.0.1']['state'] === 1 && $rows['10.0.0.1']['rttTenthMs'] === 10, 'answers but a service fails is degraded, any vantage counts');
check($rows['10.0.0.2']['state'] === 2 && $rows['10.0.0.2']['rttTenthMs'] === null, 'failing now is down with no RTT');
check($rows['10.0.0.3']['state'] === 3 && $rows['10.0.0.9']['state'] === 3, 'stale and dead are unknown');
check($summary['rttTenthMs'] === 8, 'aggregate RTT excludes dead and failing targets');
check($summary['lossPermille'] === 500, 'failing targets count as full loss, dead ones not at all');

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
