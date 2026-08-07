<?php
// A-4 (NORMATIVE): IF-MIB operStatus -> §3.1 wire gear state. FAIL DARK.
// panel.php returns a closure and registers nothing until invoked with a
// Router, so requiring it only defines the helpers.
require __DIR__ . '/../../dashboard/api/routes/panel.php';

$cases = [
    [1,    1, 'up'],
    [3,    2, 'testing -> degraded'],
    [5,    2, 'dormant -> degraded'],
    [2,    0, 'down'],
    [7,    0, 'lowerLayerDown -> down'],
    [4,    0, 'unknown -> down (fail dark)'],
    [6,    0, 'notPresent -> down (fail dark)'],
    [null, 0, 'NULL -> down (fail dark)'],
    [99,   0, 'unrecognised -> down (fail dark)'],
];
$fail = 0;
foreach ($cases as [$in, $want, $label]) {
    $got = panelGearState($in);
    if ($got !== $want) { printf("FAIL: %s: got %d want %d\n", $label, $got, $want); $fail++; }
    else { printf("ok:   A-4 operStatus=%s -> %d (%s)\n", var_export($in, true), $got, $label); }
}
// All three wire states must be REACHABLE — the original bug made 0 unreachable.
$reach = array_unique(array_map('panelGearState', [1, 3, 2]));
sort($reach);
if ($reach !== [0, 1, 2]) { print("FAIL: not all three wire states reachable\n"); $fail++; }
else { print("ok:   A-4 all three wire states reachable\n"); }

// Role mapping (CONTRACT §3.1) and the 'other' drop.
foreach ([['gateway',0],['router',0],['switch',1],['hub',2],['ap',3],['wanBackup',4]] as [$k,$w]) {
    if (panelGearRole($k) !== $w) { printf("FAIL: role %s\n", $k); $fail++; }
}
if (panelGearRole('other') !== null) { print("FAIL: 'other' must drop\n"); $fail++; }

// Log scale: 0 ONLY for idle, saturates at 7.
if (panelGearLogScale(0) !== 0 || panelGearLogScale(1) < 1 || panelGearLogScale(999999) !== 7) {
    print("FAIL: log scale bounds\n"); $fail++;
}
exit($fail === 0 ? 0 : 1);
