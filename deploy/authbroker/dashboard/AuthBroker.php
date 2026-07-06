<?php
declare(strict_types=1);

/**
 * AuthBroker — Tab5 push-approval as a second factor for the SolariNet dashboard.
 *
 * ADDITIVE, CONFIG-GATED (mirrors lib/Oidc.php and deploy/nfc-2fa/Nfc2fa.php).
 * Completely inert unless the `authbroker` block of the credential store has
 * `enabled: true`. When disabled, login behaves exactly as today.
 *
 * Flow (see deploy/authbroker/README.md): after primary auth succeeds (local
 * bcrypt or OIDC), the dashboard calls requireApproval() which POSTs to the
 * local authbrokerd HTTP API. authbrokerd publishes auth/request/<id> over MQTT
 * to the Tab5, the operator taps Approve/Deny, the device publishes a SIGNED
 * response, authbrokerd verifies it and returns the decision. Only "approve"
 * lets the session be established.
 *
 * The broker holds only the device's PUBLIC key, so a broker/dashboard
 * compromise cannot forge an approval — the Tab5 is a true possession factor.
 *
 * Config block (in solari-auth.json, alongside `users`, `directory`, `oidc`):
 *   "authbroker": {
 *     "enabled": true,
 *     "url": "http://127.0.0.1:9444",   // authbrokerd HTTP API (localhost)
 *     "token": "…shared bearer…",        // matches [http] token in authbroker.conf
 *     "enforce": "all",                   // "all" | "admins" — who must approve
 *     "timeoutSeconds": 60,
 *     "device": ""                        // optional: target a specific device id
 *   }
 */
final class AuthBroker
{
    /** The `authbroker` config block merged over safe defaults. */
    public static function config(): array
    {
        $store = Auth::store();
        $c = (isset($store['authbroker']) && is_array($store['authbroker']))
            ? $store['authbroker'] : [];
        return $c + [
            'enabled'        => false,
            'url'            => 'http://127.0.0.1:9444',
            'token'          => '',
            'enforce'        => 'all',      // "all" | "admins"
            'timeoutSeconds' => 60,
            'device'         => '',
        ];
    }

    /** True only when push-approval is switched on. Half-config reads as off. */
    public static function isEnabled(): bool
    {
        return !empty(self::config()['enabled']);
    }

    /** Whether a given principal must pass the approval gate. */
    public static function isRequiredFor(array $principal): bool
    {
        if (!self::isEnabled()) {
            return false;
        }
        $enforce = strtolower((string) self::config()['enforce']);
        if ($enforce === 'admins') {
            return (strtolower((string) ($principal['role'] ?? '')) === 'admin');
        }
        return true; // "all"
    }

    /**
     * Block on a Tab5 approval for this login. Returns true on APPROVE, false on
     * deny / timeout / broker error. Fails CLOSED: if the broker is unreachable
     * and approval is required, access is denied.
     *
     * @param array $principal the already-primary-authenticated principal
     */
    public static function requireApproval(array $principal, string $sourceIp): bool
    {
        $cfg = self::config();
        $payload = json_encode([
            'action'    => 'login',
            'subject'   => (string) ($principal['username'] ?? ''),
            'detail'    => 'SolariNet dashboard',
            'source_ip' => $sourceIp,
            'device'    => (string) $cfg['device'],
            'ttl'       => (int) $cfg['timeoutSeconds'],
        ]);

        $ch = curl_init(rtrim((string) $cfg['url'], '/') . '/auth/request');
        curl_setopt_array($ch, [
            CURLOPT_POST           => true,
            CURLOPT_POSTFIELDS     => $payload,
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_TIMEOUT        => (int) $cfg['timeoutSeconds'] + 10,
            CURLOPT_HTTPHEADER     => [
                'Content-Type: application/json',
                'Authorization: Bearer ' . (string) $cfg['token'],
            ],
        ]);
        $body = curl_exec($ch);
        $code = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
        $err  = curl_error($ch);
        curl_close($ch);

        if ($body === false || $code >= 400) {
            error_log("[authbroker] request failed (http $code): $err");
            return false; // fail closed
        }
        $data = json_decode((string) $body, true);
        $decision = is_array($data) ? ($data['decision'] ?? '') : '';
        if ($decision !== 'approve') {
            error_log("[authbroker] login for "
                . ($principal['username'] ?? '?') . " -> " . var_export($decision, true));
        }
        return $decision === 'approve';
    }
}
