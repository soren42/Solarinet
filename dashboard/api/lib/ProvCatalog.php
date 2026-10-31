<?php
declare(strict_types=1);

/**
 * ProvCatalog — validation + SoR access helpers for the provisioning catalog
 * (routes/provisioning.php): images, the software pool, stacks, runs, and the
 * IMAGE_REFRESH / PROV_STAGE / STACK_APPLY enqueue routes.
 *
 * Loaded by routes/provisioning.php (require_once), not by bootstrap.php.
 *
 * VALIDATION mirrors netdb/sor/migrations/029_provisioning_images_stacks.sql
 * CHECK-for-CHECK so a bad request is a field-specific 400 here rather than a
 * constraint failure in the database. A few rules are deliberately STRICTER
 * than 029 on newly supplied values (never re-applied to stored values, so a
 * seeded row can always be edited):
 *   - release_name / install_template / ansible_role: one safe path token;
 *   - column URLs (images source_url/checksum_url, software source_url,
 *     artifact url): http(s), one whitespace/control-free token;
 *   - pkg family names: one whitespace/control-free token; non-pkg family
 *     names: exactly '=' (029 also tolerates '', stackgen.py does not);
 *   - spec top-level keys (and repo family-block keys) limited to the sets
 *     deploy/fleet/stackgen.py accepts; repo spec needs >= 1 family block;
 *   - applies_* given as an empty list is refused (use null for "all");
 *   - text is valid UTF-8, counted in characters; logical_name / stack name
 *     non-blank.
 *
 * DB ERRORS: every PDOException is mapped to a clean JSON error (dup key /
 * FK -> 409, CHECK -> 400, SIGNAL -> 409, lock/deadlock -> 503, else 500).
 * No SQL text or driver message is ever returned to the client.
 *
 * Wire contract for the enqueue verbs (docs/Provisioning_UI_Design.md, P3):
 *   id    ^[1-9][0-9]{0,9}$ and <= 4294967295
 *   slug  ^[a-z0-9][a-z0-9-]{0,47}$
 *   stacks 1..32 slugs after de-duplication
 * All regexes below anchor with \z (PHP's $ would accept a trailing "\n").
 */
final class ProvCatalog
{
    public const ID_MAX     = 4294967295;
    public const MAX_STACKS = 32;
    public const ID_RE      = '/^[1-9][0-9]{0,9}\z/';
    public const SLUG_RE    = '/^[a-z0-9][a-z0-9-]{0,47}\z/';
    public const SHA256_RE  = '/^[0-9a-f]{64}\z/';
    public const PIN_RE     = '/^[A-Za-z0-9][A-Za-z0-9._+~-]{0,63}\z/';
    public const COMMIT_RE  = '/^[0-9a-f]{40}([0-9a-f]{24})?\z/';
    public const DISTRO_RE  = '/^[a-z0-9][a-z0-9-]{0,31}\z/';
    public const RELEASE_RE = '/^[A-Za-z0-9][A-Za-z0-9._-]{0,47}\z/';
    public const TEMPLATE_RE = '/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\z/';
    public const ROLE_RE    = '/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\z/';
    /** 029 ck_prov_software_name / repo fingerprint: C0, DEL, C1, U+2028/9. */
    public const CONTROL_RE = '/[\x{00}-\x{1f}\x{7f}-\x{9f}\x{2028}\x{2029}]/u';
    /** 029 repo url/key_url: one token, no space/control/C1/Unicode separator. */
    public const REPO_URL_RE = '/^[^\s\x{00}-\x{1f}\x{7f}-\x{9f}\p{Z}]+\z/u';
    /** 029 repo suite / each component. */
    public const REPO_TOKEN_RE = '/^[A-Za-z0-9._\/][A-Za-z0-9._\/-]*\z/';
    /** Column URLs (stricter than 029): http(s), one token. */
    public const URL_RE     = '/^https?:\/\/[^\s\x{00}-\x{1f}\x{7f}-\x{9f}\p{Z}]+\z/u';
    /** pkg family names (stricter than 029): one token. */
    public const PKG_NAME_RE = '/^[^\s\x{00}-\x{1f}\x{7f}-\x{9f}\p{Z}]{1,128}\z/u';

    public const FAMILIES    = ['suse', 'debian', 'fedora'];
    public const ARCHES      = ['x86_64', 'arm64', 'armhf', 'riscv64', 'i386'];
    public const ART_ARCHES  = ['any', 'x86_64', 'arm64', 'armhf', 'riscv64', 'i386'];
    public const BOOT_METHODS = ['ipxe', 'pi_netboot', 'media'];
    public const REFRESH     = ['manual', 'weekly', 'point_release'];
    public const ITEM_TYPES  = ['pkg', 'repo', 'artifact', 'script', 'build', 'role'];
    public const BUILD_SYSTEMS = ['cmake', 'make', 'autotools', 'meson', 'custom'];
    public const RUN_MODES   = ['reimage', 'apply', 'refresh'];
    public const RUN_STATUSES = ['queued', 'running', 'succeeded', 'failed', 'cancelled'];
    /** Type-default member order (029 header / stackgen TYPE_DEFAULT_ORDER). */
    public const TYPE_RANK_SQL = "FIELD(p.item_type,'repo','pkg','artifact','build','script','role')";

    /** Allowed spec keys per item type (deploy/fleet/stackgen.py *_KEYS). */
    public const SPEC_KEYS = [
        'script'   => ['body', 'url', 'sha256', 'interpreter', 'run_as', 'env', 'creates', 'check', 'version_check'],
        'build'    => ['source', 'system', 'configure_args', 'prefix', 'build', 'install', 'env', 'creates', 'check', 'version_check'],
        'artifact' => ['extract', 'creates'],
        'repo'     => ['suse', 'debian', 'fedora'],
    ];
    public const REPO_FAMILY_KEYS = ['url', 'key_url', 'fingerprint', 'suite', 'components'];

    /** Max JSON request body for catalog writes (a script body can be large). */
    public const BODY_MAX = 262144;

    private static bool $inTx = false;

    // ================================================================ errors

    /** 400 with a field-specific message; rolls back an open transaction. */
    public static function bad(string $message): never
    {
        self::fail('bad_request', $message, 400);
    }

    public static function notFound(string $message): never
    {
        self::fail('not_found', $message, 404);
    }

    public static function conflict(string $message): never
    {
        self::fail('conflict', $message, 409);
    }

    /** Emit an error envelope (exits), rolling back first if we opened a tx. */
    public static function fail(string $code, string $message, int $status): never
    {
        self::rollbackQuietly();
        Response::error($code, $message, $status);
        exit(1);   // Response::error() exits; this keeps `never` honest.
    }

    /** Map a PDOException to a clean envelope. Never leaks SQL or driver text. */
    public static function dbFail(PDOException $e): never
    {
        $info   = is_array($e->errorInfo ?? null) ? $e->errorInfo : [];
        $state  = (string) ($info[0] ?? $e->getCode());
        $driver = (int) ($info[1] ?? 0);
        error_log(sprintf('provisioning: database error sqlstate=%s driver=%d', $state, $driver));
        if ($driver === 1062) {
            self::fail('conflict', 'An entry with the same unique key already exists.', 409);
        }
        if ($driver === 1451) {
            self::fail('conflict', 'The entry is still referenced by other rows.', 409);
        }
        if ($driver === 1452) {
            self::fail('conflict', 'A referenced entry does not exist.', 409);
        }
        if ($driver === 4025 || $driver === 3819) {
            self::fail('bad_request', 'The request was rejected by a catalog constraint.', 400);
        }
        if ($driver === 1406 || $driver === 1264 || $driver === 1265 || $driver === 1366) {
            self::fail('bad_request', 'A field value is out of range or too long.', 400);
        }
        if ($driver === 1644 || $state === '45000') {
            self::fail('conflict', 'The change was refused by a catalog rule.', 409);
        }
        if ($driver === 1205 || $driver === 1213 || $state === '40001') {
            self::fail('busy', 'The catalog is busy; retry the request.', 503);
        }
        self::fail('db_error', 'Catalog database error.', 500);
    }

    // ==================================================================== DB

    /** @return list<array<string,mixed>> */
    public static function rows(string $sql, array $params = []): array
    {
        try {
            $st = Sor::db()->prepare($sql);
            $st->execute($params);
            return $st->fetchAll(PDO::FETCH_ASSOC);
        } catch (PDOException $e) {
            self::dbFail($e);
        }
    }

    /** @return array<string,mixed>|null */
    public static function row(string $sql, array $params = []): ?array
    {
        $rows = self::rows($sql, $params);
        return $rows[0] ?? null;
    }

    public static function scalar(string $sql, array $params = []): mixed
    {
        $r = self::row($sql, $params);
        return $r === null ? null : (array_values($r)[0] ?? null);
    }

    /** Execute a write; returns affected rows. */
    public static function exec(string $sql, array $params = []): int
    {
        try {
            $st = Sor::db()->prepare($sql);
            $st->execute($params);
            return $st->rowCount();
        } catch (PDOException $e) {
            self::dbFail($e);
        }
    }

    public static function insert(string $sql, array $params): int
    {
        self::exec($sql, $params);
        try {
            return (int) Sor::db()->lastInsertId();
        } catch (PDOException $e) {
            self::dbFail($e);
        }
    }

    /** Run $fn inside a transaction; any error path rolls back. */
    public static function tx(callable $fn): mixed
    {
        $db = Sor::db();
        try {
            $db->beginTransaction();
            self::$inTx = true;
            $out = $fn();
            $db->commit();
            self::$inTx = false;
            return $out;
        } catch (PDOException $e) {
            self::dbFail($e);
        } catch (Throwable $e) {
            self::rollbackQuietly();
            throw $e;
        }
    }

    private static function rollbackQuietly(): void
    {
        if (!self::$inTx) {
            return;
        }
        self::$inTx = false;
        try {
            $db = Sor::db();
            if ($db->inTransaction()) {
                $db->rollBack();
            }
        } catch (Throwable $e) {
            // Connection teardown at exit rolls back anyway.
        }
    }

    /** Provenance params for an INSERT/UPDATE (source = dashboard, human). */
    public static function prov(): array
    {
        return [':src' => Sor::dashboardSourceId()];
    }

    // ================================================================ parsing

    /** Decode a body: strict JSON object with only the allowed keys. */
    public static function body(array $allowed, ?int $max = null): array
    {
        $b = solari_json_body($max ?? self::BODY_MAX);
        if ($b !== [] && array_is_list($b)) {
            self::bad('Request body must be a JSON object.');
        }
        foreach (array_keys($b) as $k) {
            if (!in_array((string) $k, $allowed, true)) {
                self::bad('Unknown field: ' . self::clip((string) $k) . '.');
            }
        }
        return $b;
    }

    /** Clip user text echoed in an error message. */
    public static function clip(string $s): string
    {
        $s = preg_replace('/[^A-Za-z0-9._ -]/', '?', $s) ?? '';
        return strlen($s) > 48 ? substr($s, 0, 48) . '…' : $s;
    }

    /** A path/route id: ^[1-9][0-9]{0,9}$ and <= 4294967295. */
    public static function pathId(string $raw, string $field = 'id'): int
    {
        if (preg_match(self::ID_RE, $raw) !== 1 || (int) $raw > self::ID_MAX) {
            self::bad("$field must be a positive integer id.");
        }
        return (int) $raw;
    }

    /** A body id: an int, or a string of digits, under the same contract. */
    public static function idValue(mixed $v, string $field): int
    {
        if (is_int($v)) {
            $v = (string) $v;
        }
        if (!is_string($v) || preg_match(self::ID_RE, $v) !== 1 || (int) $v > self::ID_MAX) {
            self::bad("$field must be a positive integer id (1..4294967295).");
        }
        return (int) $v;
    }

    public static function requireKey(array $b, string $field): mixed
    {
        if (!array_key_exists($field, $b) || $b[$field] === null) {
            self::bad("$field is required.");
        }
        return $b[$field];
    }

    /** UTF-8 text, length in characters; null allowed when $nullable. */
    public static function text(mixed $v, string $field, int $max, bool $nullable = true): ?string
    {
        if ($v === null) {
            if (!$nullable) {
                self::bad("$field is required.");
            }
            return null;
        }
        if (!is_string($v)) {
            self::bad("$field must be a string.");
        }
        if (!mb_check_encoding($v, 'UTF-8')) {
            self::bad("$field must be valid UTF-8.");
        }
        if (mb_strlen($v, 'UTF-8') > $max) {
            self::bad("$field must be at most $max characters.");
        }
        return $v;
    }

    /** Text that must match $re (null allowed when $nullable). */
    public static function token(mixed $v, string $field, string $re, string $desc, bool $nullable = true): ?string
    {
        if ($v === null && $nullable) {
            return null;
        }
        if (!is_string($v) || preg_match($re, $v) !== 1) {
            self::bad("$field must be $desc.");
        }
        return $v;
    }

    public static function url(mixed $v, string $field, bool $nullable = true): ?string
    {
        $u = self::token($v, $field, self::URL_RE, 'an http(s) URL with no whitespace or control characters', $nullable);
        if ($u !== null && strlen($u) > 512) {
            self::bad("$field must be at most 512 characters.");
        }
        return $u;
    }

    public static function enum(mixed $v, string $field, array $allowed): string
    {
        if (!is_string($v) || !in_array($v, $allowed, true)) {
            self::bad("$field must be one of: " . implode(', ', $allowed) . '.');
        }
        return $v;
    }

    /** 0/1 flag from a JSON bool (or 0/1 integer). */
    public static function flag(mixed $v, string $field): int
    {
        if ($v === true || $v === 1) {
            return 1;
        }
        if ($v === false || $v === 0) {
            return 0;
        }
        self::bad("$field must be a boolean.");
    }

    /** SMALLINT UNSIGNED. */
    public static function smallint(mixed $v, string $field, bool $nullable): ?int
    {
        if ($v === null && $nullable) {
            return null;
        }
        if (!is_int($v) || $v < 0 || $v > 65535) {
            self::bad("$field must be an integer 0..65535" . ($nullable ? ' or null' : '') . '.');
        }
        return $v;
    }

    /** SET column from a JSON list (null = all). Canonical order, no dups. */
    public static function setValue(mixed $v, string $field, array $allowed): ?string
    {
        if ($v === null) {
            return null;
        }
        if (!is_array($v) || !array_is_list($v) || $v === []) {
            self::bad("$field must be null (all) or a non-empty list of: " . implode(', ', $allowed) . '.');
        }
        foreach ($v as $item) {
            if (!is_string($item) || !in_array($item, $allowed, true)) {
                self::bad("$field must only contain: " . implode(', ', $allowed) . '.');
            }
        }
        return implode(',', array_values(array_filter($allowed, static fn ($a) => in_array($a, $v, true))));
    }

    /** @return list<string>|null */
    public static function setOut(?string $v): ?array
    {
        return ($v === null || $v === '') ? null : explode(',', $v);
    }

    /** Stack slug list for the enqueue verbs: 1..32 distinct slugs. */
    public static function stackList(mixed $v, string $field = 'stacks'): array
    {
        if (!is_array($v) || !array_is_list($v)) {
            self::bad("$field must be a list of stack slugs.");
        }
        $out = [];
        foreach ($v as $s) {
            if (!is_string($s) || preg_match(self::SLUG_RE, $s) !== 1) {
                self::bad("$field: every entry must be a stack slug ([a-z0-9][a-z0-9-]{0,47}).");
            }
            $out[$s] = true;
        }
        $out = array_keys($out);
        if (count($out) < 1 || count($out) > self::MAX_STACKS) {
            self::bad("$field must name 1.." . self::MAX_STACKS . ' distinct stacks.');
        }
        return array_map('strval', $out);
    }

    // ===================================================== JSON object helpers

    /** A decoded JSON value that was an object (assoc array, or empty). */
    public static function isObj(mixed $v): bool
    {
        return is_array($v) && ($v === [] || !array_is_list($v));
    }

    /** JSON_VALUE(x) <> '' under a PAD SPACE collation: not empty/all-spaces. */
    public static function nonBlank(mixed $v): bool
    {
        return is_string($v) && rtrim($v, ' ') !== '';
    }

    /** Re-encode a validated spec, keeping known object positions as {}. */
    public static function specJson(?array $spec): ?string
    {
        if ($spec === null) {
            return null;
        }
        $objKeys = ['source', 'env', 'suse', 'debian', 'fedora'];
        $o = [];
        foreach ($spec as $k => $val) {
            $o[$k] = (in_array($k, $objKeys, true) && is_array($val) && self::isObj($val)) ? (object) $val : $val;
        }
        return json_encode((object) $o, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR);
    }

    // ============================================== software cross-field rules

    /**
     * Family name rule (029 ck_prov_software_<family>, stricter on new values).
     * $isNew = the value was supplied in this request.
     */
    public static function checkFamilyName(?string $name, string $type, string $field, bool $isNew): void
    {
        if ($name === null) {
            return;
        }
        if ($type === 'pkg') {
            if ($name === '=' || rtrim($name, ' ') === '' || rtrim($name, ' ') === '=') {
                self::bad("$field must be a real package name for a pkg entry (not '=' or empty).");
            }
            if ($isNew && preg_match(self::PKG_NAME_RE, $name) !== 1) {
                self::bad("$field must be one package-name token (no whitespace or control characters).");
            }
            return;
        }
        $ok = $isNew ? $name === '=' : (rtrim($name, ' ') === '=' || rtrim($name, ' ') === '');
        if (!$ok) {
            self::bad("$field must be '=' (supported) or null (unsupported) for a $type entry.");
        }
    }

    /**
     * Full spec validation for item type $type with merged version_pin $pin.
     * Mirrors 029 ck_prov_software_spec, _spec_req, _script, _build, _check,
     * _repo, plus the stackgen.py key sets.
     */
    public static function checkSpec(string $type, mixed $spec, ?string $pin): void
    {
        if ($spec === null) {
            if (in_array($type, ['repo', 'script', 'build'], true)) {
                self::bad("spec is required for a $type entry.");
            }
            return;
        }
        if (!self::isObj($spec)) {
            self::bad('spec must be a JSON object or null.');
        }
        if ($type === 'pkg' || $type === 'role') {
            self::bad("spec must be null for a $type entry.");
        }
        foreach (array_keys($spec) as $k) {
            if (!in_array((string) $k, self::SPEC_KEYS[$type], true)) {
                if ($type === 'build' && ($k === 'commands' || $k === 'build_deps')) {
                    self::bad($k === 'commands'
                        ? 'spec.commands is refused; custom builds use spec.build and spec.install.'
                        : 'spec.build_deps is refused; express build dependencies as requires edges.');
                }
                self::bad("spec.$k is not a valid key for a $type entry.");
            }
        }
        // ck_prov_software_check: if present, a non-empty array of strings whose
        // first element is non-empty.
        if (array_key_exists('check', $spec)) {
            $c = $spec['check'];
            if (!is_array($c) || !array_is_list($c) || $c === []) {
                self::bad('spec.check must be a non-empty argv list of strings.');
            }
            foreach ($c as $arg) {
                if (!is_string($arg)) {
                    self::bad('spec.check must contain only strings.');
                }
            }
            if ($c[0] === '') {
                self::bad('spec.check[0] (the program) must not be empty.');
            }
        }
        if ($type === 'script') {
            self::checkScriptSpec($spec);
        } elseif ($type === 'build') {
            self::checkBuildSpec($spec, $pin);
        } elseif ($type === 'repo') {
            self::checkRepoSpec($spec);
        }
    }

    private static function checkScriptSpec(array $spec): void
    {
        $hasBody = array_key_exists('body', $spec);
        $hasUrl  = array_key_exists('url', $spec);
        $hasSha  = array_key_exists('sha256', $spec);
        if ($hasBody && ($hasUrl || $hasSha)) {
            self::bad('spec must carry either body, or url + sha256 — not both.');
        }
        if ($hasBody) {
            if (!self::nonBlank($spec['body'])) {
                self::bad('spec.body must be a non-empty string.');
            }
            return;
        }
        if (!$hasUrl || !self::nonBlank($spec['url'])) {
            self::bad('spec needs either a non-empty body, or url + sha256 (an unpinned remote script is refused).');
        }
        if (!$hasSha || !is_string($spec['sha256']) || preg_match(self::SHA256_RE, $spec['sha256']) !== 1) {
            self::bad('spec.sha256 must be 64 lowercase hex characters.');
        }
    }

    private static function checkBuildSpec(array $spec, ?string $pin): void
    {
        $src = $spec['source'] ?? null;
        if (!array_key_exists('source', $spec) || !self::isObj($src)) {
            self::bad('spec.source must be an object ({git} or {tarball, sha256}).');
        }
        $sys = $spec['system'] ?? null;
        if (!is_string($sys) || !in_array($sys, self::BUILD_SYSTEMS, true)) {
            self::bad('spec.system must be one of: ' . implode(', ', self::BUILD_SYSTEMS) . '.');
        }
        if ($sys === 'custom') {
            foreach (['build', 'install'] as $k) {
                $v = $spec[$k] ?? null;
                if (!is_string($v) || preg_match('/[^\s]/', $v) !== 1) {
                    self::bad("spec.$k is required (a non-blank command) for a custom build.");
                }
            }
        } else {
            foreach (['build', 'install'] as $k) {
                if (array_key_exists($k, $spec)) {
                    self::bad("spec.$k is only allowed when spec.system is custom.");
                }
            }
        }
        $hasGit = array_key_exists('git', $src);
        $hasTar = array_key_exists('tarball', $src);
        if ($hasGit && $hasTar) {
            self::bad('spec.source must carry git or tarball, not both.');
        }
        if ($hasGit) {
            if (!self::nonBlank($src['git'])) {
                self::bad('spec.source.git must be a non-empty string.');
            }
            if ($pin === null || preg_match(self::COMMIT_RE, $pin) !== 1) {
                self::bad('version_pin must be the full 40/64-hex commit SHA for a git build.');
            }
            return;
        }
        if ($hasTar) {
            if (!self::nonBlank($src['tarball'])) {
                self::bad('spec.source.tarball must be a non-empty string.');
            }
            $sha = $src['sha256'] ?? null;
            if (!is_string($sha) || preg_match(self::SHA256_RE, $sha) !== 1) {
                self::bad('spec.source.sha256 must be 64 lowercase hex characters.');
            }
            return;
        }
        self::bad('spec.source must carry git or tarball.');
    }

    private static function checkRepoSpec(array $spec): void
    {
        $any = false;
        foreach (self::FAMILIES as $fam) {
            if (!array_key_exists($fam, $spec)) {
                continue;
            }
            $any = true;
            $blk = $spec[$fam];
            $w   = "spec.$fam";
            if (!self::isObj($blk)) {
                self::bad("$w must be an object.");
            }
            foreach (array_keys($blk) as $k) {
                if (!in_array((string) $k, self::REPO_FAMILY_KEYS, true)) {
                    self::bad("$w." . self::clip((string) $k) . ' is not a valid repo key.');
                }
            }
            foreach (['url', 'key_url'] as $k) {
                $v = $blk[$k] ?? null;
                if (!is_string($v) || !mb_check_encoding($v, 'UTF-8') || preg_match(self::REPO_URL_RE, $v) !== 1) {
                    self::bad("$w.$k must be one token with no whitespace or control characters.");
                }
            }
            $fp = $blk['fingerprint'] ?? null;
            if (!self::nonBlank($fp) || !mb_check_encoding($fp, 'UTF-8') || preg_match(self::CONTROL_RE, $fp) === 1) {
                self::bad("$w.fingerprint must be a non-empty string with no control characters.");
            }
            if (array_key_exists('suite', $blk)
                && (!is_string($blk['suite']) || preg_match(self::REPO_TOKEN_RE, $blk['suite']) !== 1)) {
                self::bad("$w.suite must match [A-Za-z0-9._/][A-Za-z0-9._/-]*.");
            }
            if (array_key_exists('components', $blk)) {
                $c = $blk['components'];
                if (!is_array($c) || !array_is_list($c) || $c === []) {
                    self::bad("$w.components must be a non-empty list.");
                }
                foreach ($c as $comp) {
                    if (!is_string($comp) || preg_match(self::REPO_TOKEN_RE, $comp) !== 1) {
                        self::bad("$w.components entries must match [A-Za-z0-9._/][A-Za-z0-9._/-]*.");
                    }
                }
            }
            if ($fam === 'debian' && (!array_key_exists('suite', $blk) || !array_key_exists('components', $blk))) {
                self::bad('spec.debian needs suite and components.');
            }
        }
        if (!$any) {
            self::bad('spec for a repo entry needs at least one of: suse, debian, fedora.');
        }
    }

    // ======================================================= field builders

    /** Writable prov_images fields. version/sha256/fetched_at are refresh-owned. */
    public const IMAGE_FIELDS = ['distro', 'family', 'release_name', 'release_label', 'arch', 'boot_method',
        'source_url', 'checksum_url', 'install_template', 'refresh_policy', 'netboot_verified', 'notes'];
    public const IMAGE_MANAGED = ['version', 'sha256', 'fetched_at'];
    /** Changing any of these invalidates the fetched artifact. */
    public const IMAGE_SOURCE_FIELDS = ['distro', 'release_name', 'arch', 'boot_method', 'source_url', 'checksum_url'];

    /**
     * Validate an images create ($cur null) or update body.
     * @return array<string,mixed> column => value to write
     */
    public static function imageFields(array $b, ?array $cur): array
    {
        foreach (self::IMAGE_MANAGED as $k) {
            if (array_key_exists($k, $b)) {
                self::bad("$k is managed by image refresh and cannot be set directly.");
            }
        }
        if ($cur === null) {
            foreach (['distro', 'family', 'release_name', 'arch', 'boot_method'] as $k) {
                self::requireKey($b, $k);
            }
        } elseif ($b === []) {
            self::bad('Nothing to update.');
        }
        $set = [];
        foreach ($b as $k => $v) {
            switch ($k) {
                case 'distro':
                    $set[$k] = self::token($v, $k, self::DISTRO_RE, 'a lowercase slug ([a-z0-9][a-z0-9-], max 32)', false);
                    break;
                case 'family':
                    $set[$k] = self::enum($v, $k, self::FAMILIES);
                    break;
                case 'release_name':
                    $set[$k] = self::token($v, $k, self::RELEASE_RE, 'a release token ([A-Za-z0-9][A-Za-z0-9._-], max 48)', false);
                    break;
                case 'release_label':
                    $set[$k] = self::text($v, $k, 96);
                    break;
                case 'arch':
                    $set[$k] = self::enum($v, $k, self::ARCHES);
                    break;
                case 'boot_method':
                    $set[$k] = self::enum($v, $k, self::BOOT_METHODS);
                    break;
                case 'source_url':
                case 'checksum_url':
                    $set[$k] = self::url($v, $k);
                    break;
                case 'install_template':
                    $set[$k] = self::token($v, $k, self::TEMPLATE_RE, 'a template file name ([A-Za-z0-9][A-Za-z0-9._-], max 128)');
                    break;
                case 'refresh_policy':
                    $set[$k] = self::enum($v, $k, self::REFRESH);
                    break;
                case 'netboot_verified':
                    $set[$k] = self::flag($v, $k);
                    break;
                case 'notes':
                    $set[$k] = self::text($v, $k, 255);
                    break;
                default:
                    self::bad('Unknown field: ' . self::clip((string) $k) . '.');
            }
        }
        $boot = $set['boot_method'] ?? ($cur['boot_method'] ?? null);
        $arch = $set['arch'] ?? ($cur['arch'] ?? null);
        if ($boot === 'pi_netboot' && !in_array($arch, ['arm64', 'armhf'], true)) {
            self::bad('boot_method pi_netboot requires arch arm64 or armhf.');
        }
        if ($cur !== null) {
            foreach (self::IMAGE_SOURCE_FIELDS as $k) {
                if (array_key_exists($k, $set) && $set[$k] !== $cur[$k]) {
                    // The fetched artifact no longer describes this row.
                    $set['version'] = null;
                    $set['sha256'] = null;
                    $set['fetched_at'] = null;
                    break;
                }
            }
        }
        return $set;
    }

    public const SOFTWARE_FIELDS = ['logical_name', 'item_type', 'name_suse', 'name_debian', 'name_fedora',
        'applies_families', 'applies_arches', 'ansible_role', 'source_url', 'version_pin', 'spec', 'notes'];

    /**
     * Validate a software create ($cur null) or update body against the merged
     * row. $liveArtifacts = the entry currently has live artifact rows.
     * @return array<string,mixed> column => value to write
     */
    public static function softwareFields(array $b, ?array $cur, bool $liveArtifacts = false): array
    {
        if ($cur === null) {
            self::requireKey($b, 'logical_name');
        } elseif ($b === []) {
            self::bad('Nothing to update.');
        }
        $set = [];
        $spec = null;
        foreach ($b as $k => $v) {
            switch ($k) {
                case 'logical_name':
                    $n = self::text($v, $k, 96, false);
                    if (rtrim((string) $n, ' ') === '' || preg_match(self::CONTROL_RE, (string) $n) === 1) {
                        self::bad('logical_name must be non-blank with no control characters.');
                    }
                    $set[$k] = $n;
                    break;
                case 'item_type':
                    $set[$k] = self::enum($v, $k, self::ITEM_TYPES);
                    break;
                case 'name_suse':
                case 'name_debian':
                case 'name_fedora':
                    $set[$k] = self::text($v, $k, 128);
                    break;
                case 'applies_families':
                    $set[$k] = self::setValue($v, $k, self::FAMILIES);
                    break;
                case 'applies_arches':
                    $set[$k] = self::setValue($v, $k, self::ARCHES);
                    break;
                case 'ansible_role':
                    $set[$k] = self::token($v, $k, self::ROLE_RE, 'an Ansible role name ([A-Za-z0-9][A-Za-z0-9._-], max 64)');
                    break;
                case 'source_url':
                    $set[$k] = self::url($v, $k);
                    break;
                case 'version_pin':
                    $set[$k] = self::token($v, $k, self::PIN_RE, 'one safe path component ([A-Za-z0-9][A-Za-z0-9._+~-], max 64)');
                    break;
                case 'spec':
                    if ($v !== null && !self::isObj($v)) {
                        self::bad('spec must be a JSON object or null.');
                    }
                    $spec = $v;
                    break;
                case 'notes':
                    $set[$k] = self::text($v, $k, 255);
                    break;
                default:
                    self::bad('Unknown field: ' . self::clip((string) $k) . '.');
            }
        }
        $type = $set['item_type'] ?? ($cur['item_type'] ?? 'pkg');
        if ($cur === null && !array_key_exists('item_type', $set)) {
            $set['item_type'] = 'pkg';
        }
        $role = array_key_exists('ansible_role', $set) ? $set['ansible_role'] : ($cur['ansible_role'] ?? null);
        $pin  = array_key_exists('version_pin', $set) ? $set['version_pin'] : ($cur['version_pin'] ?? null);
        $typeChanged = $cur === null || (isset($set['item_type']) && $set['item_type'] !== $cur['item_type']);

        // ck_prov_software_role
        if (($type === 'role') !== ($role !== null)) {
            self::bad('ansible_role is required for role entries and only for them.');
        }
        // ck_prov_software_pin
        if (in_array($type, ['artifact', 'build'], true) && $pin === null) {
            self::bad("version_pin is required for a $type entry.");
        }
        // ck_prov_software_<family>
        foreach (self::FAMILIES as $fam) {
            $col = "name_$fam";
            $supplied = array_key_exists($col, $set);
            if ($supplied || $typeChanged) {
                $name = $supplied ? $set[$col] : ($cur[$col] ?? null);
                self::checkFamilyName($name, $type, $col, $supplied);
            }
        }
        // ck_prov_software_spec / _spec_req / _script / _build / _check / _repo
        if (array_key_exists('spec', $b) || $typeChanged || array_key_exists('version_pin', $set)) {
            $merged = array_key_exists('spec', $b)
                ? $spec
                : (($cur['spec'] ?? null) === null ? null : json_decode((string) $cur['spec'], true));
            self::checkSpec($type, $merged, $pin);
            if (array_key_exists('spec', $b)) {
                $set['spec'] = self::specJson($spec);
                if ($set['spec'] !== null && strlen($set['spec']) > 131072) {
                    self::bad('spec must be at most 128 KiB.');
                }
            }
        }
        if ($cur !== null && $liveArtifacts && $type !== 'artifact') {
            self::conflict('This entry still has artifact rows; remove them before changing item_type.');
        }
        return $set;
    }

    public const STACK_FIELDS = ['slug', 'name', 'description', 'locked', 'sort_order'];

    /** @return array<string,mixed> column => value to write */
    public static function stackFields(array $b, bool $create): array
    {
        if ($create) {
            self::requireKey($b, 'slug');
            self::requireKey($b, 'name');
        } elseif ($b === []) {
            self::bad('Nothing to update.');
        }
        $set = [];
        foreach ($b as $k => $v) {
            switch ($k) {
                case 'slug':
                    $set[$k] = self::token($v, $k, self::SLUG_RE, 'a stack slug ([a-z0-9][a-z0-9-], max 48)', false);
                    break;
                case 'name':
                    $n = self::text($v, $k, 96, false);
                    if (rtrim((string) $n, ' ') === '') {
                        self::bad('name must not be blank.');
                    }
                    $set[$k] = $n;
                    break;
                case 'description':
                    $set[$k] = self::text($v, $k, 255);
                    break;
                case 'locked':
                    $set[$k] = self::flag($v, $k);
                    break;
                case 'sort_order':
                    $set[$k] = self::smallint($v, $k, false);
                    break;
                default:
                    self::bad('Unknown field: ' . self::clip((string) $k) . '.');
            }
        }
        return $set;
    }

    public const MEMBER_FIELDS = ['item_order', 'sort_order', 'notes'];

    /** @return array<string,mixed> column => value to write */
    public static function memberFields(array $b, bool $create): array
    {
        if (!$create && $b === []) {
            self::bad('Nothing to update.');
        }
        $set = [];
        foreach ($b as $k => $v) {
            switch ($k) {
                case 'software_id':
                    if (!$create) {
                        self::bad('software_id cannot be changed; remove and re-add the member.');
                    }
                    break;
                case 'item_order':
                    $set[$k] = self::smallint($v, $k, true);
                    break;
                case 'sort_order':
                    $set[$k] = self::smallint($v, $k, false);
                    break;
                case 'notes':
                    $set[$k] = self::text($v, $k, 255);
                    break;
                default:
                    self::bad('Unknown field: ' . self::clip((string) $k) . '.');
            }
        }
        return $set;
    }

    // ============================================================ SQL builders

    /** INSERT with provenance. Column names come only from code whitelists. */
    public static function insertRow(string $table, array $set): int
    {
        $cols = array_keys($set);
        $params = self::prov();
        foreach ($set as $c => $v) {
            $params[":$c"] = $v;
        }
        $sql = "INSERT INTO $table (" . implode(', ', $cols) . ', source_id, asserted_kind, asserted_at) VALUES ('
             . implode(', ', array_map(static fn ($c) => ":$c", $cols)) . ", :src, 'human', NOW())";
        return self::insert($sql, $params);
    }

    /** UPDATE a live row by id, re-stamping provenance. Returns affected rows. */
    public static function updateRow(string $table, int $id, array $set): int
    {
        $params = self::prov() + [':id' => $id];
        $parts = [];
        foreach ($set as $c => $v) {
            $parts[] = "$c = :$c";
            $params[":$c"] = $v;
        }
        $sql = "UPDATE $table SET " . implode(', ', $parts)
             . ", source_id = :src, asserted_kind = 'human', asserted_at = NOW() WHERE id = :id AND deleted_at IS NULL";
        return self::exec($sql, $params);
    }

    // ================================================================ mappers

    /**
     * Agent/uname arch string -> prov_images arch enum (the
     * deploy/fleet/stack-apply.py ARCH_MAP plus common aliases), or null.
     */
    /** provlib MAC_RE: the only form PROV_STAGE / prov-stage.py accept. */
    public const MAC_RE = '/^[0-9a-f]{2}(:[0-9a-f]{2}){5}\z/';

    /** Lowercase, '-' => ':' (operator/vendor spellings) — not validated. */
    public static function normMac(string $m): string
    {
        return str_replace('-', ':', strtolower(trim($m)));
    }

    /** A request MAC, normalised to MAC_RE form or 400. */
    public static function macValue(mixed $v, string $field = 'mac'): string
    {
        $m = is_string($v) ? self::normMac($v) : '';
        if (preg_match(self::MAC_RE, $m) !== 1) {
            self::bad("$field must be a MAC address (aa:bb:cc:dd:ee:ff).");
        }
        return $m;
    }

    public static function normArch(string $a): ?string
    {
        return [
            'x86_64' => 'x86_64', 'amd64' => 'x86_64',
            'aarch64' => 'arm64', 'arm64' => 'arm64',
            'armv7l' => 'armhf', 'armv6l' => 'armhf', 'armhf' => 'armhf', 'arm32' => 'armhf', 'arm' => 'armhf',
            'riscv64' => 'riscv64',
            'i386' => 'i386', 'i686' => 'i386', 'x86' => 'i386',
        ][strtolower(trim($a))] ?? null;
    }

    public static function iso(?string $dt): ?string
    {
        return Coerce::iso($dt);
    }

    public static function imageOut(array $r): array
    {
        return [
            'id'               => (int) $r['id'],
            'distro'           => $r['distro'],
            'family'           => $r['family'],
            'release_name'     => $r['release_name'],
            'release_label'    => $r['release_label'],
            'arch'             => $r['arch'],
            'boot_method'      => $r['boot_method'],
            'source_url'       => $r['source_url'],
            'checksum_url'     => $r['checksum_url'],
            'version'          => $r['version'],
            'sha256'           => $r['sha256'],
            'fetched_at'       => self::iso($r['fetched_at']),
            'fetched'          => $r['sha256'] !== null && $r['fetched_at'] !== null,
            'install_template' => $r['install_template'],
            'refresh_policy'   => $r['refresh_policy'],
            'netboot_verified' => (int) $r['netboot_verified'] === 1,
            'notes'            => $r['notes'],
            'updated_at'       => self::iso($r['updated_at'] ?? null),
        ];
    }

    public static function softwareOut(array $r): array
    {
        $spec = null;
        if ($r['spec'] !== null && $r['spec'] !== '') {
            $spec = json_decode((string) $r['spec']);   // objects stay objects
        }
        return [
            'id'               => (int) $r['id'],
            'logical_name'     => $r['logical_name'],
            'item_type'        => $r['item_type'],
            'name_suse'        => $r['name_suse'],
            'name_debian'      => $r['name_debian'],
            'name_fedora'      => $r['name_fedora'],
            'applies_families' => self::setOut($r['applies_families']),
            'applies_arches'   => self::setOut($r['applies_arches']),
            'ansible_role'     => $r['ansible_role'],
            'source_url'       => $r['source_url'],
            'version_pin'      => $r['version_pin'],
            'spec'             => $spec,
            'notes'            => $r['notes'],
            'updated_at'       => self::iso($r['updated_at'] ?? null),
        ];
    }

    public static function stackOut(array $r): array
    {
        return [
            'id'          => (int) $r['id'],
            'slug'        => $r['slug'],
            'name'        => $r['name'],
            'description' => $r['description'],
            'locked'      => (int) $r['locked'] === 1,
            'sort_order'  => (int) $r['sort_order'],
            'updated_at'  => self::iso($r['updated_at'] ?? null),
        ];
    }

    public static function runOut(array $r): array
    {
        $slugs = json_decode((string) ($r['stack_slugs'] ?? '[]'), true);
        return [
            'id'           => (string) $r['id'],
            'mode'         => $r['mode'],
            'entity_id'    => $r['entity_id'] === null ? null : (string) $r['entity_id'],   // null: refresh
            'entity_name'  => $r['entity_name'] ?? null,
            'image_id'     => $r['image_id'] === null ? null : (int) $r['image_id'],
            'image_label'  => $r['image_label'] ?? null,
            'stack_slugs'  => is_array($slugs) ? $slugs : [],
            'requested_by' => $r['requested_by'],
            'status'       => $r['status'],
            'dry_run'      => (int) $r['dry_run'] === 1,
            'detail'       => $r['detail'],
            'requested_at' => self::iso($r['requested_at']),
            'started_at'   => self::iso($r['started_at']),
            'finished_at'  => self::iso($r['finished_at']),
        ];
    }

    // ============================================================== enqueue

    /** ctrl_idem_key() semantics (routes/control.php), local copy. */
    public static function idemKey(): ?string
    {
        $k = $_SERVER['HTTP_IDEMPOTENCY_KEY'] ?? '';
        if (!is_string($k) || $k === '') {
            return null;
        }
        if (preg_match('/^[A-Za-z0-9._-]{8,128}\z/', $k) !== 1) {
            self::bad('Idempotency-Key must be 8-128 chars of [A-Za-z0-9._-].');
        }
        return $k;
    }

    /** Fleet-route reply shape: status queued + request id, or started. */
    public static function queuedOut(array $base, array $reply): array
    {
        $queued = ($reply['status'] ?? '') === 'queued' && isset($reply['request']);
        $base['status'] = $queued ? 'queued' : 'started';
        $base['log'] = isset($reply['log']) && is_string($reply['log']) ? $reply['log'] : null;
        if ($queued) {
            $base['request'] = (string) $reply['request'];
        }
        return $base;
    }
}
