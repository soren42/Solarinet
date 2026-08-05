<?php
declare(strict_types=1);

/**
 * Web Push subscription endpoints.
 *
 * POST   /api/push/subscribe  stores the current browser endpoint for the
 *                              authenticated operator.
 * DELETE /api/push/subscribe  removes an endpoint after browser unsubscribe.
 * GET    /api/push/vapid      exposes the public application-server key.
 */

return static function (Router $router): void {
    $router->get('/api/push/vapid', static function (): void {
        Auth::requireSession();
        Response::ok(['publicKey' => 'BK9zeslyJEedhIEoLnuY1OoKI0SIPPodZn6udVOnEmvLHOEVevAmHeJm-zeFZQ6L5anH8bEQ-9x8G_18KOyhSD0']);
    });

    $router->post('/api/push/subscribe', static function (): void {
        $principal = Auth::requireSession();
        $body = solari_json_body();
        $endpoint = trim((string) ($body['endpoint'] ?? ''));
        $keys = $body['keys'] ?? null;
        $p256dh = is_array($keys) ? trim((string) ($keys['p256dh'] ?? '')) : '';
        $auth = is_array($keys) ? trim((string) ($keys['auth'] ?? '')) : '';
        if ($endpoint === '' || strlen($endpoint) > 512 || !filter_var($endpoint, FILTER_VALIDATE_URL)
            || $p256dh === '' || strlen($p256dh) > 255 || $auth === '' || strlen($auth) > 255) {
            Response::error('bad_request', 'A valid push subscription endpoint and keys are required.', 400);
        }

        $userAgent = substr((string) ($_SERVER['HTTP_USER_AGENT'] ?? ''), 0, 255);
        Db::exec(
            'INSERT INTO push_subscriptions (operator, endpoint, p256dh, auth, userAgent, createdAt, lastSeenAt)
             VALUES (:operator, :endpoint, :p256dh, :auth, :userAgent, NOW(), NOW())
             ON DUPLICATE KEY UPDATE operator = VALUES(operator), p256dh = VALUES(p256dh), auth = VALUES(auth),
                 userAgent = VALUES(userAgent), lastSeenAt = NOW()',
            [
                ':operator' => (string) ($principal['username'] ?? ''),
                ':endpoint' => $endpoint,
                ':p256dh' => $p256dh,
                ':auth' => $auth,
                ':userAgent' => $userAgent !== '' ? $userAgent : null,
            ]
        );
        Response::ok(['subscribed' => true]);
    });

    $router->add('DELETE', '/api/push/subscribe', static function (): void {
        Auth::requireSession();
        $body = solari_json_body();
        $endpoint = trim((string) ($body['endpoint'] ?? ''));
        if ($endpoint === '') {
            Response::error('bad_request', 'endpoint is required.', 400);
        }
        Db::exec('DELETE FROM push_subscriptions WHERE endpoint = :endpoint', [':endpoint' => $endpoint]);
        Response::ok(['unsubscribed' => true]);
    });
};
