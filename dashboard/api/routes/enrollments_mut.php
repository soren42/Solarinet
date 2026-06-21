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

        SolariCtl::call('APPROVE', ['enr' => $enrId, 'op' => $op]);
        Response::ok(['enrId' => $enrId, 'status' => 'approved', 'decidedBy' => $op]);
    });

    // POST /api/enrollments/{enrId}/reject
    $router->post('/api/enrollments/{enrId}/reject', static function (array $p): void {
        $enrId = enr_id_param($p['enrId']);
        // Rejecting is reversible (the node may re-enroll); a named operator is
        // recorded but the privileged-role gate is not required.
        $op = Operator::name();
        $args = ['enr' => $enrId];
        if ($op !== '') {
            $args['op'] = $op;
        }
        SolariCtl::call('REJECT', $args);
        Response::ok(['enrId' => $enrId, 'status' => 'rejected', 'decidedBy' => $op]);
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
