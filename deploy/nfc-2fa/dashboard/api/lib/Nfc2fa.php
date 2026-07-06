<?php
declare(strict_types=1);

/**
 * Nfc2fa — NFC-card second factor for the SolariNet dashboard.
 *
 * ADDITIVE, CONFIG-GATED (mirrors lib/Oidc.php). Completely inert unless the
 * `nfc2fa` block of the credential store has `enabled: true`. When disabled, none
 * of the login/session paths change and the dashboard behaves exactly as today.
 *
 * Threat posture (see deploy/nfc-2fa/DESIGN.md §2): the card UID is NOT a secret
 * (cloneable). We treat the card as a *possession* factor bound to an enrolled,
 * revocable credential. UIDs are stored salted+hashed. A server-issued single-use
 * challenge (bound to the pending-2FA session) stops replay. Mode B
 * (DESFire/applet challenge-response) is structured for but not yet implemented.
 *
 * STORAGE: reads/writes the same JSON credential store as Auth (Auth::authFile()).
 * The MariaDB SoR path (nfcCredential table via solariCtl) is stubbed — see
 * persist(). PHP is a read tier for telemetry, but auth-store writes are the
 * dashboard's own control-plane data, consistent with how alertRule is handled.
 *
 * This class does not overwrite any live file; it is delivered under
 * deploy/nfc-2fa/ and wired per deploy/nfc-2fa/dashboard/INTEGRATION.md.
 */
final class Nfc2fa
{
    /** Session key holding the pending-2FA transaction (challenge + principal). */
    private const SESS_TX = 'solari_nfc_tx';

    // ------------------------------------------------------------------ config

    /** The `nfc2fa` config block merged over safe defaults. */
    public static function config(): array
    {
        $store = Auth::store();
        $c = (isset($store['nfc2fa']) && is_array($store['nfc2fa'])) ? $store['nfc2fa'] : [];
        return $c + [
            'enabled'             => false,
            'enforce'             => 'enrolled',   // "enrolled" | "all"
            'maxFailures'         => 5,
            'lockoutSeconds'      => 900,
            'challengeTtlSeconds' => 60,
            'readerHmacKeyRef'    => '',
        ];
    }

    /** True only when the second factor is switched on. Half-config reads as off. */
    public static function isEnabled(): bool
    {
        $c = self::config();
        return !empty($c['enabled']);
    }

    // ------------------------------------------------------- enrolment lookup

    /** All non-revoked cards enrolled to a username. */
    public static function activeCards(string $username): array
    {
        foreach (Auth::store()['users'] ?? [] as $u) {
            if (!is_array($u) || ($u['username'] ?? null) !== $username) {
                continue;
            }
            $cards = is_array($u['nfcCards'] ?? null) ? $u['nfcCards'] : [];
            return array_values(array_filter($cards, static fn($c) =>
                is_array($c) && empty($c['revoked'])));
        }
        return [];
    }

    /**
     * Does this login require the NFC step? True when 2FA is on AND either the
     * user has an active card (enforce=enrolled) or enforce=all.
     */
    public static function isRequiredFor(string $username): bool
    {
        if (!self::isEnabled()) {
            return false;
        }
        $c = self::config();
        if (($c['enforce'] ?? 'enrolled') === 'all') {
            return true;   // caller decides how to handle a user with no card yet
        }
        return count(self::activeCards($username)) > 0;
    }

    // ------------------------------------------------------------- hashing

    /** Per-card random salt (base64). */
    public static function newSalt(): string
    {
        return base64_encode(random_bytes(16));
    }

    /**
     * Hash a raw UID with a per-card salt. Uses argon2id when available (PHP's
     * password_hash needs its own salt, so we HMAC-fold the app salt in first),
     * else a salted SHA-256. The UID is not a password; this is defense-in-depth
     * so a store leak does not hand over exact bytes to clone onto a magic card.
     */
    public static function hashUid(string $rawUidHex, string $saltB64): string
    {
        $uid  = strtoupper(preg_replace('/[^0-9A-Fa-f]/', '', $rawUidHex) ?? '');
        $salt = base64_decode($saltB64, true) ?: $saltB64;
        // Fold salt+uid deterministically, then hash. hash_hmac is constant work.
        $material = hash_hmac('sha256', $uid, $salt, true);
        return 'sha256:' . base64_encode($material);
    }

    /** Constant-time comparison of a presented UID against a stored card hash. */
    public static function uidMatches(string $rawUidHex, array $card): bool
    {
        $salt = (string) ($card['uidSalt'] ?? '');
        $want = (string) ($card['uidHash'] ?? '');
        if ($salt === '' || $want === '') {
            return false;
        }
        $got = self::hashUid($rawUidHex, $salt);
        return hash_equals($want, $got);
    }

    // --------------------------------------------------- pending transaction

    /**
     * Begin the second-factor step: called AFTER primary auth succeeds. Stashes a
     * fresh single-use challenge + the verified principal on the session, and
     * marks the session pending. Returns the client-visible challenge payload.
     */
    public static function beginVerify(array $principal): array
    {
        Auth::bootSession();
        $c = self::config();
        $challenge = base64_encode(random_bytes(32));
        $_SESSION[self::SESS_TX] = [
            'principal' => $principal,
            'challenge' => $challenge,
            'expiresAt' => time() + (int) ($c['challengeTtlSeconds'] ?? 60),
            'issuedAt'  => time(),
        ];
        // Session is NOT elevated yet: the auth gate treats a pending tx as
        // unauthenticated except for the NFC verify/logout endpoints.
        return [
            'stage'     => 'nfc_required',
            'challenge' => $challenge,
            'mode'      => 'uid',   // per-card mode is resolved on verify
            'expiresIn' => (int) ($c['challengeTtlSeconds'] ?? 60),
        ];
    }

    /** True when the current session is mid-2FA (primary done, card pending). */
    public static function isPending(): bool
    {
        Auth::bootSession();
        $tx = $_SESSION[self::SESS_TX] ?? null;
        return is_array($tx) && isset($tx['principal']);
    }

    /**
     * Complete the second factor. Validates the challenge (single-use, unexpired),
     * matches the presented UID against the principal's active cards (constant
     * time), enforces lockout, and on success elevates the session to a full
     * principal via Auth::establishSession(). Returns [ok, stage, principal|null].
     *
     * @return array{ok:bool,stage:string,principal:?array}
     */
    public static function completeVerify(string $challenge, string $rawUidHex, ?array $crypto = null): array
    {
        Auth::bootSession();
        $tx = $_SESSION[self::SESS_TX] ?? null;
        if (!is_array($tx) || !isset($tx['principal'])) {
            return ['ok' => false, 'stage' => 'no_pending', 'principal' => null];
        }
        // Single-use: consume the challenge regardless of outcome.
        $expected = (string) ($tx['challenge'] ?? '');
        $expires  = (int) ($tx['expiresAt'] ?? 0);
        $principal = $tx['principal'];
        $username  = (string) ($principal['username'] ?? '');

        // Replay / expiry / mismatch → uniform failure (no oracle).
        if ($expected === '' || !hash_equals($expected, $challenge) || time() > $expires) {
            unset($_SESSION[self::SESS_TX]);
            self::recordFailure($username);
            return ['ok' => false, 'stage' => 'nfc_failed', 'principal' => null];
        }
        // Consume the challenge now so it cannot be replayed even within TTL.
        $_SESSION[self::SESS_TX]['challenge'] = '';

        if (self::isLockedOut($username)) {
            return ['ok' => false, 'stage' => 'nfc_failed', 'principal' => null];
        }

        $matched = null;
        foreach (self::activeCards($username) as $card) {
            $mode = (string) ($card['mode'] ?? 'uid');
            if ($mode === 'crypto') {
                // Mode B stub: verify challenge-response envelope against key_ref.
                // Not implemented tonight; a crypto card cannot match yet.
                if (self::verifyCrypto($card, $challenge, $crypto)) {
                    $matched = $card;
                    break;
                }
                continue;
            }
            if (self::uidMatches($rawUidHex, $card)) {
                $matched = $card;
                break;
            }
        }

        if ($matched === null) {
            self::recordFailure($username);
            return ['ok' => false, 'stage' => 'nfc_failed', 'principal' => null];
        }

        // Success: clear the tx, reset failures, elevate the session.
        unset($_SESSION[self::SESS_TX]);
        self::resetFailures($username);
        self::touchCard($username, (string) ($matched['id'] ?? ''));
        $full = Auth::establishSession($principal);  // regenerates id, pins principal
        return ['ok' => true, 'stage' => 'authenticated', 'principal' => $full];
    }

    /** Mode B challenge-response verification — STUB (returns false). */
    private static function verifyCrypto(array $card, string $challenge, ?array $crypto): bool
    {
        error_log('[nfc2fa] crypto-mode card presented but Mode B not implemented; denying');
        return false;
    }

    // ------------------------------------------------------------- lockout

    /** Lockout counter lives on the session store keyed by user (best-effort). */
    private static function failKey(string $u): string { return 'solari_nfc_fail_' . sha1($u); }

    private static function recordFailure(string $username): void
    {
        Auth::bootSession();
        $k = self::failKey($username);
        $rec = $_SESSION[$k] ?? ['count' => 0, 'first' => time()];
        $rec['count'] = (int) ($rec['count'] ?? 0) + 1;
        $rec['last']  = time();
        $_SESSION[$k] = $rec;
    }

    private static function resetFailures(string $username): void
    {
        Auth::bootSession();
        unset($_SESSION[self::failKey($username)]);
    }

    public static function isLockedOut(string $username): bool
    {
        Auth::bootSession();
        $c = self::config();
        $rec = $_SESSION[self::failKey($username)] ?? null;
        if (!is_array($rec)) {
            return false;
        }
        $max = (int) ($c['maxFailures'] ?? 5);
        $win = (int) ($c['lockoutSeconds'] ?? 900);
        if ((int) ($rec['count'] ?? 0) < $max) {
            return false;
        }
        // Locked until the window elapses from the last failure.
        return (time() - (int) ($rec['last'] ?? 0)) < $win;
    }

    // --------------------------------------------------------- enrolment write

    /**
     * Enrol a card for a user. Rejects a UID already bound to anyone (a shared
     * factor is not a factor). Persists via persist(). Returns the new card meta.
     *
     * @return array{ok:bool,error?:string,card?:array}
     */
    public static function enroll(string $username, string $rawUidHex, string $cardType, string $label, string $enrolledBy): array
    {
        $uid = strtoupper(preg_replace('/[^0-9A-Fa-f]/', '', $rawUidHex) ?? '');
        if (strlen($uid) < 8) {
            return ['ok' => false, 'error' => 'bad_uid'];
        }
        // Reject duplicate enrolment of the same physical UID (any user).
        foreach (Auth::store()['users'] ?? [] as $u) {
            foreach (($u['nfcCards'] ?? []) as $card) {
                if (!is_array($card) || !empty($card['revoked'])) {
                    continue;
                }
                if (self::uidMatches($uid, $card)) {
                    return ['ok' => false, 'error' => 'already_enrolled'];
                }
            }
        }
        $salt = self::newSalt();
        $meta = [
            'id'         => 'card-' . bin2hex(random_bytes(4)),
            'mode'       => 'uid',
            'label'      => $label !== '' ? $label : 'NFC card',
            'uidSalt'    => $salt,
            'uidHash'    => self::hashUid($uid, $salt),
            'cardType'   => $cardType !== '' ? $cardType : 'unknown',
            'enrolledAt' => (new DateTimeImmutable('now', new DateTimeZone('UTC')))->format('Y-m-d\TH:i:s\Z'),
            'enrolledBy' => $enrolledBy,
            'lastUsedAt' => null,
            'revoked'    => false,
            'revokedAt'  => null,
            'keyRef'     => null,
        ];
        if (!self::persist($username, static function (array &$user) use ($meta): void {
            $user['nfcCards'] = array_values(array_merge(
                is_array($user['nfcCards'] ?? null) ? $user['nfcCards'] : [],
                [$meta]
            ));
        })) {
            return ['ok' => false, 'error' => 'persist_failed'];
        }
        // Return without the hash/salt.
        return ['ok' => true, 'card' => [
            'id' => $meta['id'], 'label' => $meta['label'], 'mode' => $meta['mode'],
            'cardType' => $meta['cardType'], 'enrolledAt' => $meta['enrolledAt'],
        ]];
    }

    /** Revoke one card by id for a user. */
    public static function revoke(string $username, string $cardId): bool
    {
        return self::persist($username, static function (array &$user) use ($cardId): void {
            foreach ($user['nfcCards'] ?? [] as &$card) {
                if (is_array($card) && ($card['id'] ?? '') === $cardId) {
                    $card['revoked']   = true;
                    $card['revokedAt'] = (new DateTimeImmutable('now', new DateTimeZone('UTC')))->format('Y-m-d\TH:i:s\Z');
                }
            }
            unset($card);
        });
    }

    /** List a user's cards (redacted — no hash/salt). */
    public static function listCards(string $username): array
    {
        $out = [];
        foreach (self::activeCards($username) as $c) {
            $out[] = [
                'id' => $c['id'] ?? '', 'label' => $c['label'] ?? '',
                'mode' => $c['mode'] ?? 'uid', 'cardType' => $c['cardType'] ?? '',
                'enrolledAt' => $c['enrolledAt'] ?? null, 'lastUsedAt' => $c['lastUsedAt'] ?? null,
            ];
        }
        return $out;
    }

    private static function touchCard(string $username, string $cardId): void
    {
        self::persist($username, static function (array &$user) use ($cardId): void {
            foreach ($user['nfcCards'] ?? [] as &$card) {
                if (is_array($card) && ($card['id'] ?? '') === $cardId) {
                    $card['lastUsedAt'] = (new DateTimeImmutable('now', new DateTimeZone('UTC')))->format('Y-m-d\TH:i:s\Z');
                }
            }
            unset($card);
        });
    }

    /**
     * Persist a mutation to one user's record in the credential store.
     *
     * TONIGHT: read-modify-write the JSON store atomically (tmp + rename). This is
     * the same store Auth reads; a lock avoids a lost update against a concurrent
     * login. When the MariaDB SoR lands, replace this body with a solariCtl call
     * (NFC_ENROLL / NFC_REVOKE) so the C server owns the authoritative write and
     * PHP proposes only — see DESIGN.md §3.2.
     *
     * @param callable(array): void $mutate receives the user array by reference.
     */
    private static function persist(string $username, callable $mutate): bool
    {
        $path = Auth::authFile();
        $fh = @fopen($path, 'c+');
        if ($fh === false) {
            error_log("[nfc2fa] cannot open credential store for write: $path");
            return false;
        }
        try {
            if (!flock($fh, LOCK_EX)) {
                return false;
            }
            $raw = stream_get_contents($fh) ?: '';
            $doc = json_decode($raw, true);
            if (!is_array($doc)) {
                $doc = ['version' => 1, 'users' => []];
            }
            $found = false;
            foreach ($doc['users'] ?? [] as &$user) {
                if (is_array($user) && ($user['username'] ?? null) === $username) {
                    $mutate($user);
                    $found = true;
                    break;
                }
            }
            unset($user);
            if (!$found) {
                error_log("[nfc2fa] enrol target user not found: $username");
                return false;
            }
            $out = json_encode($doc, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);
            if ($out === false) {
                return false;
            }
            ftruncate($fh, 0);
            rewind($fh);
            fwrite($fh, $out);
            fflush($fh);
            // Invalidate Auth's in-process cache so subsequent reads see the write.
            if (method_exists(Auth::class, 'forgetStore')) {
                Auth::forgetStore();
            }
            return true;
        } finally {
            flock($fh, LOCK_UN);
            fclose($fh);
        }
    }
}
