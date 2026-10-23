<?php
declare(strict_types=1);

require_once dirname(__DIR__, 2) . '/dashboard/api/lib/bootstrap.php';

$accepted = [
    'application/json',
    'Application/JSON',
    ' application/json ',
    'application/json; charset=utf-8',
    ' APPLICATION/JSON ;charset=UTF-8',
];
$rejected = [
    '',
    'text/plain',
    'text/plain; application/json',
    'application/jsonp',
    'application/json+xml',
    'application/problem+json',
    'x-application/json',
    'application/jsonp; charset=utf-8',
];

foreach ($accepted as $value) {
    if (!solari_is_json_content_type($value)) {
        fwrite(STDERR, "FAIL: valid JSON media type rejected: $value\n");
        exit(1);
    }
}
foreach ($rejected as $value) {
    if (solari_is_json_content_type($value)) {
        fwrite(STDERR, "FAIL: non-JSON media type accepted: $value\n");
        exit(1);
    }
}

printf("JSON Content-Type exact-match tests passed (%d accepted, %d rejected)\n",
    count($accepted), count($rejected));
