<?php
declare(strict_types=1);

/**
 * PDO runs with native prepares (ATTR_EMULATE_PREPARES=false), which reject a
 * named placeholder used twice in one statement (HY093 -> HTTP 500). Flag any
 * SQL string literal under dashboard/api that repeats a :name.
 */
$API = dirname(__DIR__, 2) . '/dashboard/api';
$bad = [];
foreach (new RecursiveIteratorIterator(new RecursiveDirectoryIterator($API, FilesystemIterator::SKIP_DOTS)) as $file) {
    if ($file->getExtension() !== 'php') {
        continue;
    }
    foreach (token_get_all((string) file_get_contents($file->getPathname())) as $tok) {
        if (!is_array($tok) || !in_array($tok[0], [T_CONSTANT_ENCAPSED_STRING, T_ENCAPSED_AND_WHITESPACE], true)) {
            continue;
        }
        preg_match_all('/(?<![:\w]):([a-zA-Z_]\w*)/', $tok[1], $m);
        $reused = array_keys(array_filter(array_count_values($m[1]), fn(int $n): bool => $n > 1));
        if ($reused) {
            $bad[] = substr($file->getPathname(), strlen($API) + 1) . ':' . $tok[2] . ' :' . implode(', :', $reused);
        }
    }
}
if ($bad) {
    fwrite(STDERR, "FAIL: reused named placeholder (bind each once under native prepares):\n  " . implode("\n  ", $bad) . "\n");
    exit(1);
}
echo "OK: no reused named placeholders in dashboard/api\n";
