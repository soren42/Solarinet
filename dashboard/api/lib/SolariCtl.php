<?php
declare(strict_types=1);

/**
 * SolariCtl — thin PHP client for the privileged operator bridge (§9.1, §11.1).
 *
 * The mutating dashboard endpoints never touch MariaDB monitoring tables and
 * never hold cert/CA material. Every state change is delegated to the C control
 * helper (src/server/solariCtl.c), which holds the CA key, enforces operator
 * RBAC + the explicit one-time decommission confirm, and emits the actual SCP
 * control frames. PHP only opens the AF_UNIX socket, frames one request line,
 * and parses the one-line reply.
 *
 * WIRE FORMAT — matched byte-for-byte to ctlHandleLine() in solariCtl.c:
 *
 *   request : "VERB k=v k=v ...\n"     (one request per line, '\n' terminated)
 *   reply   : "OK [k=v ...]\n"               on success
 *           | "ERR <code> <message>\n"       on failure  (code is a solariStatus int)
 *
 * Values may NOT contain spaces (the C parser splits args on whitespace), so any
 * value that could carry spaces (a config/payload JSON blob) is URL-encoded by
 * this client before it is placed on the wire — exactly as the C file's CONTRACT
 * GAPS note (item 1) instructs the PHP layer to do for cfg=/payload= blobs.
 *
 * The socket path comes ONLY from the environment (SOLARI_CTL_SOCK), defaulting
 * to the server's compiled-in default (/run/solari/solariCtl.sock). No secret is
 * ever read or written here.
 */
final class SolariCtl
{
    /** Default matches serverContext.c's compiled-in bind.server.ctlSocket. */
    private const DEFAULT_SOCK = '/run/solari/solariCtl.sock';

    /** Connect+read timeout (seconds). The bridge replies on a single line. */
    private const TIMEOUT_SEC = 5;

    /** Largest reply line we will read (matches CTL_REPLY_CAP in solariCtl.c). */
    private const REPLY_CAP = 16384;

    /**
     * Send one VERB request to the bridge and return the parsed reply.
     *
     * @param string                       $verb Uppercase verb (PING, APPROVE, …).
     * @param array<string,string|int|bool> $args Argument pairs; values with
     *        spaces/newlines are URL-encoded so the C whitespace parser is safe.
     * @return array{ok:bool,code:int,message:string,fields:array<string,string>}
     */
    public static function send(string $verb, array $args = []): array
    {
        $line = self::frame($verb, $args);
        $reply = self::transact($line);
        return self::parseReply($reply);
    }

    /**
     * Send a request and, on a bridge-level ERR, emit the §11.2 error envelope
     * and terminate. On success returns the parsed reply fields. This is the
     * convenience used by the mutation routes.
     *
     * @param array<string,string|int|bool> $args
     * @return array<string,string> reply fields (e.g. ['confirm' => '...'])
     */
    public static function call(string $verb, array $args = []): array
    {
        $r = self::send($verb, $args);
        if (!$r['ok']) {
            // Map the bridge's RBAC refusal to 403; everything else to 400/502.
            $status = self::statusForCode($r['code'], $r['message']);
            $code   = ($status === 403) ? 'forbidden'
                    : (($status === 502) ? 'control_unavailable' : 'control_error');
            Response::error($code,
                $r['message'] !== '' ? $r['message'] : ('control verb ' . $verb . ' failed'),
                $status);
        }
        return $r['fields'];
    }

    /** Build the single request line. */
    private static function frame(string $verb, array $args): string
    {
        $parts = [strtoupper($verb)];
        foreach ($args as $k => $v) {
            if ($v === null) {
                continue;
            }
            if (is_bool($v)) {
                $v = $v ? '1' : '0';
            }
            $sv = (string) $v;
            // The C parser forbids spaces in values; URL-encode anything that is
            // not already a bare token (digits/letters/.-_:/@). Hex/ids pass
            // through untouched; JSON blobs get rawurlencode'd.
            if (preg_match('/[^A-Za-z0-9._:\/@\-]/', $sv) === 1) {
                $sv = rawurlencode($sv);
            }
            $parts[] = $k . '=' . $sv;
        }
        return implode(' ', $parts) . "\n";
    }

    /** Open the AF_UNIX socket, write one line, read one reply line, close. */
    private static function transact(string $line): string
    {
        $sock = getenv('SOLARI_CTL_SOCK');
        if ($sock === false || $sock === '') {
            $sock = self::DEFAULT_SOCK;
        }

        $errno = 0;
        $errstr = '';
        // stream_socket_client with a unix:// transport speaks AF_UNIX SOCK_STREAM.
        $fp = @stream_socket_client('unix://' . $sock, $errno, $errstr,
                                    self::TIMEOUT_SEC);
        if ($fp === false) {
            Response::error('control_unavailable',
                'Operator bridge is unreachable (' . ($errstr ?: ('errno ' . $errno)) . ').',
                502);
        }

        stream_set_timeout($fp, self::TIMEOUT_SEC);

        $written = @fwrite($fp, $line);
        if ($written === false) {
            fclose($fp);
            Response::error('control_unavailable', 'Failed to send to operator bridge.', 502);
        }

        // The bridge replies with exactly one '\n'-terminated line then closes.
        $reply = '';
        while (!feof($fp) && strlen($reply) < self::REPLY_CAP) {
            $chunk = fread($fp, self::REPLY_CAP);
            if ($chunk === false || $chunk === '') {
                $meta = stream_get_meta_data($fp);
                if (!empty($meta['timed_out'])) {
                    break;
                }
                break;
            }
            $reply .= $chunk;
            if (strpos($reply, "\n") !== false) {
                break;
            }
        }
        fclose($fp);

        if ($reply === '') {
            Response::error('control_unavailable', 'No reply from operator bridge.', 502);
        }
        return $reply;
    }

    /**
     * Parse "OK [k=v ...]" / "ERR <code> <message>" into a structured result.
     *
     * @return array{ok:bool,code:int,message:string,fields:array<string,string>}
     */
    private static function parseReply(string $reply): array
    {
        $line = rtrim($reply, "\r\n");
        if ($line === '') {
            return ['ok' => false, 'code' => -1, 'message' => 'empty reply', 'fields' => []];
        }

        // ERR <code> <message...>
        if (strncmp($line, 'ERR', 3) === 0) {
            $rest = trim((string) substr($line, 3));
            $code = -1;
            $message = $rest;
            if (preg_match('/^(-?\d+)\s*(.*)$/s', $rest, $m) === 1) {
                $code = (int) $m[1];
                $message = $m[2];
            }
            return ['ok' => false, 'code' => $code, 'message' => $message, 'fields' => []];
        }

        // OK [k=v ...]
        if ($line === 'OK' || strncmp($line, 'OK ', 3) === 0) {
            $rest = ($line === 'OK') ? '' : trim((string) substr($line, 3));
            $fields = [];
            if ($rest !== '') {
                foreach (preg_split('/\s+/', $rest) as $tok) {
                    $eq = strpos($tok, '=');
                    if ($eq !== false) {
                        $fields[substr($tok, 0, $eq)] = rawurldecode(substr($tok, $eq + 1));
                    }
                }
            }
            return ['ok' => true, 'code' => 0, 'message' => '', 'fields' => $fields];
        }

        // Anything else is a protocol violation.
        return ['ok' => false, 'code' => -1, 'message' => 'malformed bridge reply', 'fields' => []];
    }

    /**
     * Map a solariStatus code / message onto an HTTP status. ERR_AUTH_ROLE is the
     * RBAC/confirm refusal → 403; an unreachable/transport class → 502; the rest
     * are caller-correctable validation errors → 400.
     */
    private static function statusForCode(int $code, string $message): int
    {
        // The bridge uses ERR_AUTH_ROLE for both "operator required" and a
        // confirm-token mismatch; surface those as 403 so the UI re-prompts.
        if (stripos($message, 'operator') !== false
            || stripos($message, 'confirm token') !== false
            || stripos($message, 'not authorized') !== false) {
            return 403;
        }
        return 400;
    }
}
