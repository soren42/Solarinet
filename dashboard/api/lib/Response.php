<?php
declare(strict_types=1);

/**
 * Response — emits the SolariNet REST/JSON envelope (Architecture & Plan §11.2).
 *
 * Success: { "ok": true,  "data": <payload>, "ts": "<ISO-8601 UTC>" }
 * Failure: { "ok": false, "error": { "code": <string>, "message": <string> } }
 *
 * Raw units only on the wire (KB / micros / millis / permille / ISO-8601).
 * Display conversion is the dashboard adapter's job, never the server's.
 */
final class Response
{
    /**
     * Emit a success envelope and terminate the request.
     *
     * @param mixed $data   JSON-serialisable payload (array, scalar, or null).
     * @param int   $status HTTP status code (default 200).
     */
    public static function ok($data, int $status = 200): void
    {
        self::send($status, [
            'ok'   => true,
            'data' => $data,
            'ts'   => self::nowIso8601Utc(),
        ]);
    }

    /**
     * Emit an error envelope and terminate the request.
     *
     * @param string $code    Stable machine-readable error code (e.g. "not_found").
     * @param string $message Human-readable detail.
     * @param int    $status  HTTP status code (default 400).
     */
    public static function error(string $code, string $message, int $status = 400): void
    {
        self::send($status, [
            'ok'    => false,
            'error' => [
                'code'    => $code,
                'message' => $message,
            ],
        ]);
    }

    /** ISO-8601 UTC timestamp, second precision with trailing Z. */
    public static function nowIso8601Utc(): string
    {
        return (new DateTimeImmutable('now', new DateTimeZone('UTC')))
            ->format('Y-m-d\TH:i:s\Z');
    }

    /**
     * Serialise and write the body with the correct headers, then exit.
     *
     * @param array<string,mixed> $envelope
     */
    private static function send(int $status, array $envelope): void
    {
        if (!headers_sent()) {
            http_response_code($status);
            header('Content-Type: application/json; charset=utf-8');
            header('Cache-Control: no-store');
            header('X-Content-Type-Options: nosniff');
        }
        echo json_encode(
            $envelope,
            JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_PARTIAL_OUTPUT_ON_ERROR
        );
        exit;
    }
}
