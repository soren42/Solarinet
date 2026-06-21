<?php
declare(strict_types=1);

/**
 * Discovery mutations (§6) — adopt / ignore a discovered candidate.
 *   POST /api/discovery/{discId}/adopt   [body: {spec?:<probeSpec>}]
 *   POST /api/discovery/{discId}/ignore
 *
 * Both route through solariCtl (ADOPT / IGNORE). PHP writes no monitoring tables
 * and holds no CA material; the C bridge performs the actual adopt/ignore.
 * Adopt is a building, non-destructive op so it does not require the operator
 * role; it still records who acted (op=) for audit when available.
 */

return static function (Router $router): void {

    // POST /api/discovery/{discId}/adopt
    $router->post('/api/discovery/{discId}/adopt', static function (array $p): void {
        $discId = self_disc_id($p['discId']);
        $body   = solari_json_body();

        $args = ['disc' => $discId];
        // A probe spec, if supplied, may carry JSON; SolariCtl URL-encodes it.
        if (isset($body['spec'])) {
            $spec = is_string($body['spec']) ? $body['spec'] : json_encode($body['spec']);
            if ($spec !== '' && $spec !== false) {
                $args['spec'] = $spec;
            }
        }
        $op = Operator::name();
        if ($op !== '') {
            $args['op'] = $op;
        }

        SolariCtl::call('ADOPT', $args);
        Response::ok(['discId' => $discId, 'status' => 'adopting']);
    });

    // POST /api/discovery/{discId}/ignore
    $router->post('/api/discovery/{discId}/ignore', static function (array $p): void {
        $discId = self_disc_id($p['discId']);
        $op = Operator::name();
        $args = ['disc' => $discId];
        if ($op !== '') {
            $args['op'] = $op;
        }
        SolariCtl::call('IGNORE', $args);
        Response::ok(['discId' => $discId, 'status' => 'ignored']);
    });
};

/** Validate a {discId} path segment is a positive integer id. */
function self_disc_id(string $raw): string
{
    if (preg_match('/^\d+$/', $raw) !== 1 || $raw === '0') {
        Response::error('bad_request', 'discId must be a positive integer.', 400);
    }
    return $raw;
}
