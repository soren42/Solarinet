<?php
// D17 (NORMATIVE, CONTRACT-SW §13): server CRC alarm episodes must land in
// 0x80000000..0xBFFFFFFF — 0xC0000000+ belongs to firmware-local episodes.
// The pre-P1a mask (& 0x7fffffff) spilled: e.g. the empty vital set's seed
// 'vital:' has CRC bit 30 set and produced 0xDA973186. This suite pins the
// helper both ends: literal golden values (a mask revert fails here) and a
// range sweep over representative pool/vital seeds.
// panel.php returns a closure and registers nothing until invoked with a
// Router, so requiring it only defines the helpers.
require __DIR__ . '/../../dashboard/api/routes/panel.php';

$fail = 0;

// Golden values, computed independently of the helper. The first two have
// CRC bit 30 SET — under the old mask they landed in the firmware-local
// range (0xDA973186 and 0xD3D99E8B|high bit); reverting the mask fails them.
$golden = [
    ['vital:',              2593599878, 'empty vital set (bit30 set; old mask spilled to 0xDA973186)'],
    ['3,7',                 2564836474, 'pool set 3,7 (bit30 set; old mask spilled)'],
    ['vital:SYNTHETIC-V.',  2578426589, 'the 2026-08-11 acceptance-test episode (bit30 clear, unchanged)'],
    ['',                    2147483648, 'empty pool set = namespace floor 0x80000000'],
    ['1,2',                 2869849243, 'pool set 1,2 (bit30 clear)'],
];
foreach ($golden as [$seed, $want, $label]) {
    $got = panelCrcEpisode($seed);
    if ($got !== $want) { printf("FAIL: %s: got %u want %u\n", $label, $got, $want); $fail++; }
    else { printf("ok:   D17 %s -> %u\n", $label, $got); }
}

// Range sweep: every episode must sit inside the server CRC namespace.
$seeds = ['vital:', ''];
foreach (range(0, 500) as $i) {
    $seeds[] = (string) $i;                    // pool-set shapes
    $seeds[] = "$i," . ($i + 1);
    $seeds[] = "vital:HOST$i.,HOST" . ($i + 1) . '.';
}
$bad = 0;
foreach ($seeds as $seed) {
    $id = panelCrcEpisode($seed);
    if ($id < 0x80000000 || $id > 0xBFFFFFFF) {
        printf("FAIL: seed %s -> 0x%08X outside 0x80000000..0xBFFFFFFF\n", var_export($seed, true), $id);
        $bad++;
    }
}
if ($bad === 0) { printf("ok:   D17 range sweep: %d seeds all inside 0x80000000..0xBFFFFFFF\n", count($seeds)); }
$fail += $bad;

exit($fail === 0 ? 0 : 1);
