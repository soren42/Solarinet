<?php
declare(strict_types=1);

/**
 * NFC 2FA routes — enrolment, verify, list/revoke. ADDITIVE + CONFIG-GATED.
 *
 * Reachability contract (mirrors auth.php): the verify endpoint must be reachable
 * by a session that is mid-2FA (primary done, card pending) — the auth gate in
 * index.php lets /api/auth/* through, so these live under /api/auth/nfc/*.
 * Enrolment endpoints require a FULLY authenticated operator (Operator::require).
 *
 * When nfc2fa.enabled is false every endpoint fails closed with `nfc_disabled`,
 * so wiring these routes never changes behaviour until the feature is switched on.
 *
 * @return callable(Router):void
 */

return static function (Router $router): void {

    // Guard shared by every endpoint: 404-equivalent when the feature is off.
    $requireEnabled = static function (): void {
        if (!Nfc2fa::isEnabled()) {
            Response::error('nfc_disabled', 'NFC 2FA is not enabled.', 404);
        }
    };

    // POST /api/auth/nfc/verify  body: { challenge, uid, crypto? }
    // Reachable by a pending-2FA session ONLY. Elevates the session on success.
    $router->post('/api/auth/nfc/verify', static function () use ($requireEnabled): void {
        $requireEnabled();
        if (!Nfc2fa::isPending()) {
            Response::error('no_pending', 'No second-factor challenge is in progress.', 409);
        }
        $body = solari_json_body();
        $challenge = (string) ($body['challenge'] ?? '');
        $uid       = (string) ($body['uid'] ?? '');
        $crypto    = is_array($body['crypto'] ?? null) ? $body['crypto'] : null;

        $r = Nfc2fa::completeVerify($challenge, $uid, $crypto);
        if (!$r['ok']) {
            // Uniform failure; do not distinguish expired/replay/no-match.
            Response::error('nfc_failed', 'Card not recognised.', 401);
        }
        $p = $r['principal'];
        Response::ok([
            'stage'       => 'authenticated',
            'operator'    => $p['username'],
            'role'        => $p['role'],
            'displayName' => $p['displayName'],
            'source'      => $p['source'],
        ]);
    });

    // --- enrolment (fully-authenticated operator only) -------------------

    // POST /api/auth/nfc/enroll/begin  body: { username }
    // Issues an enrol ticket bound to the admin session. (Ticket reuses the
    // pending-tx challenge machinery; here it just returns a nonce the browser
    // passes to the reader and back, so a stray tap can't be replayed as an enrol.)
    $router->post('/api/auth/nfc/enroll/begin', static function () use ($requireEnabled): void {
        $requireEnabled();
        Operator::requireOperator();               // 403 unless operator/admin
        $body   = solari_json_body();
        $target = trim((string) ($body['username'] ?? ''));
        if ($target === '') {
            Response::error('bad_request', 'username is required.', 400);
        }
        $ticket = base64_encode(random_bytes(24));
        // Stash on the session so complete can validate it (10 min TTL).
        Auth::bootSession();
        $_SESSION['solari_nfc_enroll'] = [
            'ticket' => $ticket, 'username' => $target,
            'by' => Operator::name(), 'expiresAt' => time() + 600,
        ];
        Response::ok(['ticket' => $ticket, 'username' => $target, 'expiresIn' => 600]);
    });

    // POST /api/auth/nfc/enroll/complete  body: { ticket, uid, type?, label? }
    $router->post('/api/auth/nfc/enroll/complete', static function () use ($requireEnabled): void {
        $requireEnabled();
        Operator::requireOperator();
        $body = solari_json_body();
        Auth::bootSession();
        $tx = $_SESSION['solari_nfc_enroll'] ?? null;
        $ticket = (string) ($body['ticket'] ?? '');
        if (!is_array($tx) || !hash_equals((string) ($tx['ticket'] ?? ''), $ticket)
            || time() > (int) ($tx['expiresAt'] ?? 0)) {
            Response::error('bad_ticket', 'Enrolment ticket invalid or expired.', 409);
        }
        unset($_SESSION['solari_nfc_enroll']);     // single-use

        $r = Nfc2fa::enroll(
            (string) $tx['username'],
            (string) ($body['uid'] ?? ''),
            (string) ($body['type'] ?? ''),
            (string) ($body['label'] ?? ''),
            (string) $tx['by']
        );
        if (!$r['ok']) {
            $msg = [
                'bad_uid'          => 'The card UID was not readable.',
                'already_enrolled' => 'That card is already enrolled.',
                'persist_failed'   => 'Could not save the enrolment.',
            ][$r['error']] ?? 'Enrolment failed.';
            Response::error($r['error'], $msg, 400);
        }
        Response::ok(['enrolled' => true, 'card' => $r['card'], 'username' => $tx['username']]);
    });

    // GET  /api/auth/nfc/cards?username=…   (operator; defaults to self)
    $router->get('/api/auth/nfc/cards', static function () use ($requireEnabled): void {
        $requireEnabled();
        Operator::requireOperator();
        $u = trim((string) ($_GET['username'] ?? Operator::name()));
        Response::ok(['username' => $u, 'cards' => Nfc2fa::listCards($u)]);
    });

    // POST /api/auth/nfc/cards/{cardId}/revoke  body: { username }
    $router->post('/api/auth/nfc/cards/{cardId}/revoke', static function (array $p) use ($requireEnabled): void {
        $requireEnabled();
        Operator::requireOperator();
        $body = solari_json_body();
        $u = trim((string) ($body['username'] ?? Operator::name()));
        if (!Nfc2fa::revoke($u, (string) $p['cardId'])) {
            Response::error('revoke_failed', 'Could not revoke that card.', 400);
        }
        Response::ok(['revoked' => true, 'cardId' => $p['cardId'], 'username' => $u]);
    });
};
