<?php
declare(strict_types=1);

/**
 * LabelUnavailable — the render helper reported missing Python deps (exit 3).
 * The route layer maps this to HTTP 503 `label_renderer_unavailable`, distinct
 * from a genuine render failure (RuntimeException → 500).
 */
final class LabelUnavailable extends \RuntimeException
{
}

/**
 * Label — renders a physical inventory label (QR + Code128 + human text) to
 * raw image bytes by shelling out to tools/label_render.py, mirroring the
 * Dig.php proc_open mechanics (build argv, escapeshellarg, non-blocking
 * stdout/stderr reads, wall-clock deadline, proc_terminate).
 *
 * SCOPE: this class produces bytes only. Carrying those bytes to a physical
 * printer (MakeID EP53/E1 over BLE, Nemonic MIP-201W over Wi-Fi) is a future,
 * pluggable LabelTransport (`send(string $bytes, array $profile): void`) — the
 * first cut ships the PNG/PDF download from GET .../label. Do NOT couple
 * printer specifics into render(); keep it a pure bytes-in/bytes-out helper.
 *
 * SECURITY: every argv element is escapeshellarg()'d. --data / text lines carry
 * operator/SoR-derived strings; they are quoted as defence in depth on top of
 * the route layer's own validation, never concatenated raw into the command.
 */
final class Label
{
    /** Wall-clock ceiling for one render before we kill the child. */
    private const DEADLINE_SEC = 10.0;

    /** Named label profiles: physical stock the shop actually stocks. */
    public const PROFILES = [
        'default'      => ['width' => 406, 'height' => 203, 'dpi' => 203], // 50.8×25.4 mm — MakeID EP53 50×25 stock
        'makeid_small' => ['width' => 320, 'height' =>  96, 'dpi' => 203], // 40×12 mm — MakeID E1 12 mm tape
        'nemonic'      => ['width' => 944, 'height' => 472, 'dpi' => 300], // ~80×40 mm — Nemonic MIP-201W
    ];

    /**
     * Resolve a named profile to {width,height,dpi}; unknown name → 'default'.
     *
     * @return array{width:int,height:int,dpi:int}
     */
    public static function profile(string $name): array
    {
        return self::PROFILES[$name] ?? self::PROFILES['default'];
    }

    /**
     * Render a label to raw image bytes.
     *
     * @param array{0?:string,1?:string,2?:string} $lines line1/line2/line3
     * @param string $symbology  qr|code128|both
     * @param string $format     png|pdf
     * @throws LabelUnavailable  helper reported missing Python deps (exit 3)
     * @throws \RuntimeException  any other render failure (exit 2/4, timeout, no output)
     */
    public static function render(
        string $data,
        array $lines,
        string $symbology,
        int $widthDots,
        int $heightDots,
        int $dpi,
        string $format
    ): string {
        $symbology = in_array($symbology, ['qr', 'code128', 'both'], true) ? $symbology : 'both';
        $format    = in_array($format, ['png', 'pdf'], true) ? $format : 'png';

        $script = __DIR__ . '/../tools/label_render.py';

        // Use the combined --flag=value form for every string that can begin
        // with '-'. argparse rejects `--data -AKO` ("expected one argument")
        // because it reads the value as another option; `--data=-AKO` binds
        // unconditionally. (Each token is still escapeshellarg()'d below.)
        $argv = [
            'python3',
            $script,
            '--data=' . $data,
            '--symbology=' . $symbology,
            '--line1=' . (string) ($lines[0] ?? ''),
            '--line2=' . (string) ($lines[1] ?? ''),
            '--line3=' . (string) ($lines[2] ?? ''),
            '--width-dots', (string) $widthDots,
            '--height-dots', (string) $heightDots,
            '--dpi', (string) $dpi,
            '--format', $format,
        ];
        $cmd = implode(' ', array_map('escapeshellarg', $argv));

        $descriptors = [
            1 => ['pipe', 'w'],
            2 => ['pipe', 'w'],
        ];
        $proc = @proc_open($cmd, $descriptors, $pipes);
        if (!is_resource($proc)) {
            throw new \RuntimeException('label renderer failed to start');
        }
        stream_set_blocking($pipes[1], false);
        stream_set_blocking($pipes[2], false);

        $out      = '';
        $err      = '';
        $deadline = microtime(true) + self::DEADLINE_SEC;
        $timedOut = false;

        while (true) {
            $status = proc_get_status($proc);

            $read   = [$pipes[1], $pipes[2]];
            $write  = null;
            $except = null;
            $n = @stream_select($read, $write, $except, 0, 200000);
            if ($n !== false && $n > 0) {
                foreach ($read as $stream) {
                    $chunk = @fread($stream, 65536);
                    if (is_string($chunk) && $chunk !== '') {
                        if ($stream === $pipes[1]) {
                            $out .= $chunk;
                        } else {
                            $err .= $chunk;
                        }
                    }
                }
            }

            if ($status !== false && !$status['running']) {
                // Drain anything still buffered after the child exited.
                $out .= (string) @stream_get_contents($pipes[1]);
                $err .= (string) @stream_get_contents($pipes[2]);
                break;
            }
            if (microtime(true) >= $deadline) {
                $timedOut = true;
                @proc_terminate($proc, 9);
                break;
            }
        }

        @fclose($pipes[1]);
        @fclose($pipes[2]);
        $exit = proc_close($proc);
        if ($timedOut) {
            $exit = 124;
        }

        if ($timedOut) {
            throw new \RuntimeException('label renderer timed out after ' . self::DEADLINE_SEC . 's');
        }
        if ($exit === 3) {
            error_log('Label::render missing deps: ' . self::firstLine($err));
            throw new LabelUnavailable('install python3-qrcode python3-barcode on the API host');
        }
        if ($exit !== 0 || $out === '') {
            error_log('Label::render exit ' . $exit . ': ' . self::firstLine($err));
            throw new \RuntimeException('label render failed (exit ' . $exit . ')');
        }
        return $out;
    }

    private static function firstLine(string $s): string
    {
        $s = trim($s);
        if ($s === '') {
            return '(no stderr)';
        }
        $nl = strpos($s, "\n");
        return $nl === false ? $s : substr($s, 0, $nl);
    }
}
