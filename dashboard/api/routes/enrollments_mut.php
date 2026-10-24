<?php
declare(strict_types=1);

/**
 * Enrollment mutations (§6) — approve / reject a pending CSR.
 *   POST /api/enrollments/{enrId}/approve   (DESTRUCTIVE: mints a cert via CA)
 *   POST /api/enrollments/{enrId}/reject
 *
 * Approve is a privileged action: it causes the C bridge (the sole holder of the
 * CA key, §11.1) to sign the node's CSR. PHP therefore demands an operator role
 * AND an explicit confirm before contacting solariCtl. Reject is non-destructive
 * but still records the operator. PHP never reads csrPem and never signs.
 */

return static function (Router $router): void {

    // First-boot bearer exchange. The 256-bit token authenticates the node;
    // PHP forwards the CSR to C, which verifies its hash against the enrollment
    // row and prebound host/role before storing anything.
    $router->post('/api/enrollments', static function (): void {
        $contentLength = filter_var($_SERVER['CONTENT_LENGTH'] ?? null, FILTER_VALIDATE_INT);
        if (is_int($contentLength) && $contentLength > 16384) {
            Response::error('payload_too_large', 'Enrollment request exceeds 16 KiB.', 413);
        }
        $body = solari_json_body();
        $token = is_string($body['token'] ?? null) ? $body['token'] : '';
        $role = is_string($body['role'] ?? null) ? $body['role'] : '';
        $host = is_string($body['host'] ?? null) ? $body['host'] : '';
        $fp = is_string($body['certFp'] ?? null) ? $body['certFp'] : '';
        $csr = is_string($body['csrPem'] ?? null) ? $body['csrPem'] : '';
        if (preg_match('/\A[0-9a-f]{64}\z/i', $token) !== 1 || strlen($host) > 253 ||
            !in_array($role, ['client', 'monitor', 'server'], true) ||
            $host === '' || preg_match('/\A(?:[0-9a-f]{2}:){31}[0-9a-f]{2}\z/i', $fp) !== 1 ||
            $csr === '' || strlen($csr) > 4096) {
            Response::error('bad_request', 'Invalid enrollment request.', 400);
        }
        $reply = SolariCtl::call('ENROLL_SUBMIT', [
            'token' => $token, 'role' => $role, 'host' => $host,
            'certFp' => $fp, 'csrPem' => $csr,
        ]);
        $out = ['status' => $reply['status'] ?? 'pending'];
        if ($out['status'] === 'approved') {
            $out['nodeId'] = $reply['nodeId'] ?? '';
            $out['cert'] = $reply['cert'] ?? '';
            $out['ca'] = $reply['ca'] ?? '';
            if ($out['nodeId'] === '' || $out['cert'] === '' || $out['ca'] === '') {
                Response::error('control_error', 'Approved enrollment returned incomplete certificate material.', 502);
            }
        }
        Response::ok($out);
    }, ['auth' => 'public']);

    // Called after the node has validated and installed the returned cert/CA.
    // Until this succeeds, the same token may repeat the exchange after a lost
    // response, making pickup recoverable across network interruption.
    $router->post('/api/enrollments/ack', static function (): void {
        $body = solari_json_body();
        $token = is_string($body['token'] ?? null) ? $body['token'] : '';
        if (preg_match('/\A[0-9a-f]{64}\z/i', $token) !== 1) {
            Response::error('bad_request', 'Invalid enrollment acknowledgement.', 400);
        }
        $reply = SolariCtl::call('ENROLL_ACK', ['token' => $token]);
        Response::ok(['status' => $reply['status'] ?? 'acknowledged']);
    }, ['auth' => 'public']);

    // POST /api/enrollments/{enrId}/approve  — operator-gated + confirm-gated.
    $router->post('/api/enrollments/{enrId}/approve', static function (array $p): void {
        $enrId = enr_id_param($p['enrId']);
        $body  = solari_json_body();

        // Destructive: require operator role + explicit confirm (double-confirm
        // before any CA action). The C bridge re-checks RBAC via op=.
        $op = Operator::requireOperator();
        if (($body['confirm'] ?? null) !== true) {
            Response::error('confirm_required',
                'Approving a CSR signs a certificate; resend with {"confirm":true}.', 409);
        }

        // The dashboard peer is not allowed to execute privileged verbs itself.
        // Bind the operator in the queue record and let solariServer execute it.
        $reply = SolariCtl::callQueued('APPROVE', ['enr' => $enrId], $op);
        $out = ['enrId' => $enrId, 'status' => 'approved', 'decidedBy' => $op];
        if (enrollment_is_queued($reply)) {
            $out['status'] = 'queued';
            $out['request'] = $reply['request'];
        }
        Response::ok($out);
    });

    // POST /api/enrollments/{enrId}/reject
    $router->post('/api/enrollments/{enrId}/reject', static function (array $p): void {
        $enrId = enr_id_param($p['enrId']);
        // A named session user is recorded; the privileged-role gate is not
        // required for rejection, but the bridge action still runs in its queue.
        $op = Operator::name();
        if ($op === '') {
            Response::error('unauthorized', 'An authenticated user is required.', 401);
        }
        $reply = SolariCtl::callQueued('REJECT', ['enr' => $enrId], $op);
        $out = ['enrId' => $enrId, 'status' => 'rejected', 'decidedBy' => $op];
        if (enrollment_is_queued($reply)) {
            $out['status'] = 'queued';
            $out['request'] = $reply['request'];
        }
        Response::ok($out);
    });
};

/** Validate an {enrId} path segment is a positive integer id. */
function enr_id_param(string $raw): string
{
    if (preg_match('/^\d+$/', $raw) !== 1 || $raw === '0') {
        Response::error('bad_request', 'enrId must be a positive integer.', 400);
    }
    return $raw;
}

/** True when a queued action has not settled within the synchronous wait window. */
function enrollment_is_queued(array $reply): bool
{
    return ($reply['status'] ?? '') === 'queued' && isset($reply['request']);
}
