<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/SolariCtl.php';

function assert_true(bool $cond, string $message): void
{
    if (!$cond) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

/* Deterministic stream fixture: each stream_read() returns at most the next
 * requested fragment, so a single fread() cannot hide a split boundary. */
final class CtlChunkStream
{
    public $context;
    public static string $data = '';
    public static array $chunks = [];
    private int $offset = 0;
    public function stream_open(string $path, string $mode, int $options, ?string &$opened): bool { return true; }
    public function stream_read(int $count): string
    {
        $limit = array_shift(self::$chunks) ?? $count;
        $n = min($count, $limit, strlen(self::$data) - $this->offset);
        $part = substr(self::$data, $this->offset, $n);
        $this->offset += $n;
        return $part;
    }
    public function stream_eof(): bool { return $this->offset >= strlen(self::$data); }
    public function stream_stat(): array { return []; }
}
assert_true(stream_wrapper_register('ctlchunks', CtlChunkStream::class),
    'deterministic chunk stream wrapper should register');

$rc = new ReflectionClass(SolariCtl::class);
assert_true($rc->getConstant('REPLY_CAP') === 32768,
    'SolariCtl::REPLY_CAP must match src/server/solariCtl.c CTL_REPLY_CAP');
assert_true($rc->getConstant('REQUEST_CAP') === 16384,
    'SolariCtl::REQUEST_CAP must match src/server/solariCtl.c CTL_REQ_CAP');

$parse = $rc->getMethod('parseReply');
$parse->setAccessible(true);

$ok = $parse->invoke(null, "OK cert=" . rawurlencode("-----BEGIN CERTIFICATE-----\nabc\n-----END CERTIFICATE-----\n") . "\n");
assert_true($ok['ok'] === true, 'OK reply should parse as success');
assert_true(str_contains($ok['fields']['cert'] ?? '', 'BEGIN CERTIFICATE'),
    'URL-encoded cert field should be decoded');

$err = $parse->invoke(null, "ERR -61 operator required\n");
assert_true($err['ok'] === false, 'ERR reply should parse as failure');
assert_true($err['code'] === -61, 'ERR code should be preserved');
assert_true($err['message'] === 'operator required', 'ERR message should be preserved');

$statusForCode = $rc->getMethod('statusForCode');
$statusForCode->setAccessible(true);
assert_true($statusForCode->invoke(null, -1, 'active server required for discovery adoption') === 503,
    'standby/lease role failures should be exposed as retryable service-unavailable responses');

$readStream = $rc->getMethod('readReplyStream');
$readStream->setAccessible(true);
$reply = "OK cert=abc\n";
for ($split = 1; $split < strlen($reply); $split++) {
    CtlChunkStream::$data = $reply;
    CtlChunkStream::$chunks = [$split, strlen($reply) - $split];
    $stream = fopen('ctlchunks://reply', 'r');
    assert_true($readStream->invoke(null, $stream) === $reply,
        'reply should be read across every byte split');
    fclose($stream);
}
$byteCount = strlen($reply);
CtlChunkStream::$data = $reply;
CtlChunkStream::$chunks = array_fill(0, $byteCount, 1);
$stream = fopen('ctlchunks://reply', 'r');
assert_true($readStream->invoke(null, $stream) === $reply,
    'reply should be read when delivered one byte per read');
fclose($stream);
CtlChunkStream::$data = str_repeat('x', 32768);
CtlChunkStream::$chunks = [];
$stream = fopen('ctlchunks://reply', 'r');
assert_true(strpos($readStream->invoke(null, $stream), "\n") === false,
    'over-cap reply should remain incomplete and be rejected by transact');
fclose($stream);
CtlChunkStream::$data = 'OK truncated';
CtlChunkStream::$chunks = [];
$stream = fopen('ctlchunks://reply', 'r');
assert_true(strpos($readStream->invoke(null, $stream), "\n") === false,
    'truncated reply should be rejected');
fclose($stream);

echo "SolariCtl parser/cap tests passed\n";
