<?php
declare(strict_types=1);

/**
 * SSE bridge (§8.4, §11.2): GET /api/stream
 *
 * A deliberately thin Server-Sent-Events endpoint — no WebSockets, no daemon,
 * no long-lived PHP worker beyond this single bounded request. It bridges the
 * server's live state to the dashboard as the three event types api.jsx
 * subscribes to:
 *
 *   event: nodeStateChange   data: {"nodeId","state","hostFqdn","ts"}
 *   event: alertFired        data: {"eventId","nodeId","severity","detail","ruleId","ts"}
 *   event: probeUpdate       data: {"targetId","state","ts"}
 *
 * FRAMING (text/event-stream, per the WHATWG SSE spec):
 *   - Each message is "event: <name>\n" + "data: <json>\n" + "\n".
 *   - "id: <cursor>\n" carries our resume cursor; the browser echoes it on
 *     reconnect via the Last-Event-ID header, so we resume without dupes.
 *   - ": keepalive\n\n" comment lines are heartbeats (ignored by EventSource).
 *   - "retry: <ms>\n" tells the client the reconnect backoff.
 *
 * EVENT SOURCING: §8.4 specifies the feed is "fed from the server PUB socket
 * through solariCtl." The C bridge (solariCtl.c) speaks a one-request/one-reply
 * line protocol with no subscribe verb, and nng PUB is not consumable from PHP
 * without a persistent worker — which §8.4 forbids. So this bridge derives the
 * same events authoritatively from the server's own persisted state (the tables
 * the server writes when it processes a report/probe/alert): newly-fired
 * alertEvent rows, node lastSeen/state transitions, and probeCurrent changes.
 * It liveness-checks the operator bridge once at connect (PING) so a dead C2 is
 * surfaced. When solariCtl later grows a SUBSCRIBE verb, only emit_since() need
 * change; the framing/headers/cursor contract here is stable.
 *
 * BOUNDEDNESS: the loop runs for at most STREAM_MAX_SECONDS then closes cleanly
 * with retry set, so the browser reconnects. This keeps each PHP request short
 * (no runaway FPM worker) while the operator sees a continuous stream.
 */

return static function (Router $router): void {

    $router->get('/api/stream', static function (): void {
        // --- SSE headers; disable buffering so events flush immediately. -----
        if (!headers_sent()) {
            http_response_code(200);
            header('Content-Type: text/event-stream; charset=utf-8');
            header('Cache-Control: no-cache, no-store, must-revalidate');
            header('Connection: keep-alive');
            header('X-Accel-Buffering: no');   // nginx: do not buffer the stream
        }
        /* CRITICAL: release the PHP session-file lock before entering the
         * long-lived loop. Auth has already been checked by the router; we
         * never write the session here. Without this, the lock is held for
         * the stream's whole MAX_SECONDS lifetime and EVERY other request
         * from the same browser session (whoami, every poll, every command)
         * serializes behind it — the dashboard starves itself. Found live
         * 2026-08-05: page blank until the stream's 50 s bound expired. */
        session_write_close();
        @ini_set('zlib.output_compression', '0');
        @ini_set('output_buffering', '0');
        @set_time_limit(0);
        while (ob_get_level() > 0) {
            ob_end_flush();
        }

        $POLL_SECONDS    = 2;     // DB tail cadence
        $HEARTBEAT_EVERY = 15;    // seconds between keepalive comments
        $MAX_SECONDS     = 50;    // bound this request; client auto-reconnects

        // Resume cursor: the client echoes our last "id:" via Last-Event-ID.
        $cursor = stream_initial_cursor();

        // retry hint + one connect-time liveness probe of the operator bridge.
        echo "retry: 3000\n\n";
        stream_flush_now();
        stream_emit_bridge_health();

        $start     = time();
        $lastBeat  = time();

        while (true) {
            if (connection_aborted()) {
                break;
            }
            if ((time() - $start) >= $MAX_SECONDS) {
                // Clean close; the EventSource reconnects after `retry` ms.
                break;
            }

            $cursor = stream_emit_since($cursor);

            if ((time() - $lastBeat) >= $HEARTBEAT_EVERY) {
                echo ": keepalive\n\n";
                stream_flush_now();
                $lastBeat = time();
            }

            // Sleep between polls without busy-spinning.
            sleep($POLL_SECONDS);
        }
        exit;
    });
};

/* ===================================================================== */
/* helpers                                                                */
/* ===================================================================== */

/** Flush PHP + SAPI buffers so the event reaches the browser now. */
function stream_flush_now(): void
{
    if (ob_get_level() > 0) {
        @ob_flush();
    }
    @flush();
}

/**
 * Emit one SSE message with an explicit id (our resume cursor).
 *
 * @param array<string,mixed> $data
 */
function stream_emit(string $event, array $data, string $id): void
{
    echo 'id: ' . $id . "\n";
    echo 'event: ' . $event . "\n";
    echo 'data: ' . json_encode($data, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n\n";
    stream_flush_now();
}

/**
 * Build the resume cursor. The cursor is "<alertEventId>:<unixSeconds>": the
 * high-water alertEvent id we have already sent, plus the wall-clock from which
 * we report node/probe state changes. On reconnect the client's Last-Event-ID
 * restores both so we neither replay nor drop events across a short blip.
 */
function stream_initial_cursor(): string
{
    $last = $_SERVER['HTTP_LAST_EVENT_ID'] ?? '';
    if (is_string($last) && preg_match('/^(\d+):(\d+)$/', $last, $m) === 1) {
        return $m[1] . ':' . $m[2];
    }
    // Fresh connection: start from the current max alert id and "now" so we only
    // stream genuinely new activity (a full snapshot is the REST layer's job).
    $maxId = 0;
    try {
        $maxId = (int) (Db::scalar('SELECT COALESCE(MAX(eventId),0) FROM alertEvent') ?? 0);
    } catch (Throwable $e) {
        $maxId = 0;
    }
    return $maxId . ':' . time();
}

/** Emit a one-off bridge-health event from a connect-time PING. */
function stream_emit_bridge_health(): void
{
    $r = SolariCtl::send('PING');
    stream_emit('bridgeHealth', [
        'ok' => $r['ok'],
        'ts' => Response::nowIso8601Utc(),
    ], '0:' . time());
}

/**
 * Tail the server-written tables since the cursor, emit any new events, and
 * return the advanced cursor.
 *
 * This is the single seam to swap for a true solariCtl SUBSCRIBE later.
 */
function stream_emit_since(string $cursor): string
{
    [$lastAlertId, $sinceTs] = array_map('intval', explode(':', $cursor) + [0, time()]);
    $newAlertId = $lastAlertId;
    $sinceSql   = (new DateTimeImmutable('@' . $sinceTs))
        ->setTimezone(new DateTimeZone('UTC'))->format('Y-m-d H:i:s');

    try {
        // ---- alertFired: newly-inserted alertEvent rows -------------------
        $alerts = Db::rows(
            "SELECT e.eventId, e.nodeId, e.ruleId, e.severity, e.detail, e.firedAt
               FROM alertEvent e
              WHERE e.eventId > :last
              ORDER BY e.eventId ASC
              LIMIT 100",
            [':last' => $lastAlertId]
        );
        foreach ($alerts as $a) {
            $newAlertId = max($newAlertId, (int) $a['eventId']);
            stream_emit('alertFired', [
                'eventId'  => Coerce::id($a['eventId']),
                'nodeId'   => Coerce::id($a['nodeId']),
                'ruleId'   => Coerce::int($a['ruleId']),
                'severity' => $a['severity'],
                'detail'   => $a['detail'],
                'ts'       => Coerce::iso($a['firedAt']),
            ], $newAlertId . ':' . $sinceTs);
        }

        // ---- nodeStateChange: nodes whose lastSeenAt advanced since cursor -
        $nodes = Db::rows(
            "SELECT nodeId, hostFqdn, state, lastSeenAt
               FROM node
              WHERE lastSeenAt IS NOT NULL AND lastSeenAt > :since
              ORDER BY lastSeenAt ASC
              LIMIT 200",
            [':since' => $sinceSql]
        );
        foreach ($nodes as $n) {
            stream_emit('nodeStateChange', [
                'nodeId'   => Coerce::id($n['nodeId']),
                'hostFqdn' => $n['hostFqdn'],
                'state'    => $n['state'],
                'ts'       => Coerce::iso($n['lastSeenAt']),
            ], $newAlertId . ':' . $sinceTs);
        }

        // ---- probeUpdate: probe vantages sampled since cursor --------------
        $probes = Db::rows(
            "SELECT targetId, MAX(sampledAt) AS sampledAt,
                    SUM(outcome <> 'ok') AS bad, COUNT(*) AS n
               FROM probeCurrent
              WHERE sampledAt > :since
              GROUP BY targetId
              ORDER BY sampledAt ASC
              LIMIT 200",
            [':since' => $sinceSql]
        );
        foreach ($probes as $p) {
            $bad = (int) $p['bad'];
            $n   = (int) $p['n'];
            $state = ($bad === 0) ? 'up' : (($bad >= $n) ? 'down' : 'degraded');
            stream_emit('probeUpdate', [
                'targetId' => $p['targetId'],
                'state'    => $state,
                'ts'       => Coerce::iso($p['sampledAt']),
            ], $newAlertId . ':' . $sinceTs);
        }
    } catch (Throwable $e) {
        // A transient DB error must not kill the stream; emit a soft notice and
        // keep the cursor so the next poll retries cleanly.
        error_log('[solari-stream] ' . $e->getMessage());
    }

    // Advance the time cursor to now; keep the high-water alert id.
    return $newAlertId . ':' . time();
}
