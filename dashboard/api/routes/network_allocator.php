<?php
declare(strict_types=1);

/**
 * Operator review for SoR network static-address allocation eligibility.
 * The SoR remains authoritative; writes are transactional and CDC-backed.
 */
require_once __DIR__ . '/../lib/Sor.php';
require_once __DIR__ . '/../lib/Operator.php';

return static function (Router $router): void {
    $router->get('/api/networks/allocator-eligibility', static function (): void {
        $db = Sor::db();
        try {
            $networks = $db->query(
                "SELECT n.id,n.name,n.purpose,n.enabled,n.management,n.is_default,
                        n.allocator_eligible,n.allocator_approved_by,n.allocator_approved_at,
                        v.vlan_tag,v.name AS vlan_name
                   FROM networks n LEFT JOIN vlans v ON v.id=n.vlan_id AND v.deleted_at IS NULL
                  WHERE n.deleted_at IS NULL ORDER BY n.name,n.id"
            )->fetchAll();
            $subnets = $db->query(
                "SELECT s.id,s.network_id,s.cidr,CAST(s.gateway_ip AS CHAR) AS gateway_ip,
                        GROUP_CONCAT(DISTINCT CASE WHEN d.kind='dynamic_pool'
                          THEN CONCAT(CAST(d.range_start AS CHAR),'–',CAST(d.range_end AS CHAR)) END
                          ORDER BY d.id SEPARATOR ', ') AS dhcp_pools
                   FROM subnets s LEFT JOIN dhcp_ranges d
                     ON d.subnet_id=s.id AND d.deleted_at IS NULL
                  WHERE s.deleted_at IS NULL GROUP BY s.id,s.network_id,s.cidr,s.gateway_ip
                  ORDER BY s.network_id,s.id"
            )->fetchAll();
        } catch (Throwable $e) {
            error_log('network allocator eligibility read failed: ' . $e->getMessage());
            Response::error('service_unavailable', 'Network allocation review is unavailable.', 503);
        }

        $byNetwork = [];
        foreach ($subnets as $subnet) {
            $subnet['gateway_ip'] = network_allocator_display_ip((string) ($subnet['gateway_ip'] ?? ''));
            $subnet['dhcp_pools'] = $subnet['dhcp_pools'] ?: '';
            $subnet['ipv4'] = network_allocator_is_ipv4_cidr((string) $subnet['cidr']);
            $byNetwork[(int) $subnet['network_id']][] = $subnet;
        }
        foreach ($networks as &$network) {
            $network['id'] = (int) $network['id'];
            $network['enabled'] = (bool) $network['enabled'];
            $network['is_default'] = (bool) $network['is_default'];
            $network['allocator_eligible'] = (bool) $network['allocator_eligible'];
            $network['vlan_tag'] = $network['vlan_tag'] === null ? null : (int) $network['vlan_tag'];
            $network['subnets'] = $byNetwork[$network['id']] ?? [];
            $network['ipv4_subnet_count'] = count(array_filter(
                $network['subnets'], static fn(array $s): bool => $s['ipv4']
            ));
            $network['overlapping_networks'] = [];
        }
        unset($network);
        $ipv4Rows = [];
        foreach ($networks as $network) {
            foreach ($network['subnets'] as $subnet) {
                if ($subnet['ipv4']) {
                    $ipv4Rows[] = ['id' => $network['id'], 'name' => $network['name'], 'cidr' => $subnet['cidr']];
                }
            }
        }
        foreach ($networks as &$network) {
            foreach ($network['subnets'] as $ownSubnet) {
                if (!$ownSubnet['ipv4']) {
                    continue;
                }
                foreach ($ipv4Rows as $row) {
                    if ($row['id'] !== $network['id']
                        && network_allocator_ipv4_overlaps((string) $ownSubnet['cidr'], (string) $row['cidr'])) {
                        $network['overlapping_networks'][$row['id']] = $row['name'] . ' (' . $row['cidr'] . ')';
                    }
                }
            }
            $network['overlapping_networks'] = array_values($network['overlapping_networks']);
            $network['ipv4_overlap_count'] = count($network['overlapping_networks']);
        }
        unset($network);
        Response::ok($networks);
    });

    $router->post('/api/networks/{id}/allocator-eligibility', static function (array $params): void {
        $operator = Operator::requireOperator();
        $id = $params['id'] ?? '';
        if (!is_string($id) || !ctype_digit($id) || (int) $id < 1) {
            Response::error('bad_request', 'Network id must be a positive integer.', 400);
        }
        $body = solari_json_body();
        if (!isset($body['eligible']) || !is_bool($body['eligible'])) {
            Response::error('bad_request', 'eligible must be true or false.', 400);
        }
        if (($body['confirm'] ?? false) !== true) {
            Response::error('confirmation_required', 'Explicit confirmation is required.', 400);
        }

        $db = Sor::db();
        try {
            $db->beginTransaction();
            $query = $db->prepare(
                'SELECT id,enabled,deleted_at FROM networks WHERE id=:id FOR UPDATE'
            );
            $query->execute([':id' => (int) $id]);
            $network = $query->fetch();
            if (!$network || $network['deleted_at'] !== null) {
                $db->rollBack();
                Response::error('not_found', 'Network is not live in the SoR.', 404);
            }
            if ($body['eligible'] && !(bool) $network['enabled']) {
                $db->rollBack();
                Response::error('conflict', 'A disabled network cannot be approved.', 409);
            }
            if ($body['eligible']) {
                $query = $db->prepare('SELECT cidr FROM subnets WHERE network_id=:id AND deleted_at IS NULL');
                $query->execute([':id' => (int) $id]);
                $ipv4Count = 0;
                foreach ($query->fetchAll(PDO::FETCH_COLUMN) as $cidr) {
                    if (network_allocator_is_ipv4_cidr((string) $cidr)) {
                        $ipv4Count++;
                    }
                }
                if ($ipv4Count !== 1) {
                    $db->rollBack();
                    Response::error('conflict',
                        'Approval requires exactly one live IPv4 subnet; this network has ' . $ipv4Count . '.', 409);
                }
                $query = $db->prepare(
                    'SELECT n.name,s.cidr FROM subnets s JOIN networks n ON n.id=s.network_id ' .
                    'WHERE s.network_id<>:id AND s.deleted_at IS NULL AND n.deleted_at IS NULL'
                );
                $query->execute([':id' => (int) $id]);
                $ownQuery = $db->prepare('SELECT cidr FROM subnets WHERE network_id=:id AND deleted_at IS NULL');
                $ownQuery->execute([':id' => (int) $id]);
                $ownCidr = null;
                foreach ($ownQuery->fetchAll(PDO::FETCH_COLUMN) as $cidr) {
                    if (network_allocator_is_ipv4_cidr((string) $cidr)) {
                        $ownCidr = (string) $cidr;
                        break;
                    }
                }
                $overlaps = [];
                foreach ($query->fetchAll() as $other) {
                    if (network_allocator_ipv4_overlaps($ownCidr ?? '', (string) $other['cidr'])) {
                        $overlaps[] = (string) $other['name'] . ' (' . (string) $other['cidr'] . ')';
                    }
                }
                if ($overlaps) {
                    $db->rollBack();
                    Response::error('conflict',
                        'Overlapping SoR IPv4 prefixes must be reconciled first: ' . implode(', ', $overlaps), 409);
                }
                $update = $db->prepare(
                    'UPDATE networks SET allocator_eligible=1,allocator_approved_by=:operator,' .
                    'allocator_approved_at=UTC_TIMESTAMP() WHERE id=:id'
                );
                $update->execute([':operator' => $operator, ':id' => (int) $id]);
            } else {
                $update = $db->prepare(
                    'UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,' .
                    'allocator_approved_at=NULL WHERE id=:id'
                );
                $update->execute([':id' => (int) $id]);
            }
            $db->commit();
        } catch (Throwable $e) {
            if ($db->inTransaction()) {
                $db->rollBack();
            }
            error_log('network allocator eligibility update failed: ' . $e->getMessage());
            Response::error('service_unavailable', 'Network allocation approval could not be saved.', 503);
        }
        Response::ok(['id' => (int) $id, 'allocator_eligible' => $body['eligible']]);
    });
};

/** Return true only for a syntactically valid IPv4 CIDR. */
function network_allocator_is_ipv4_cidr(string $cidr): bool
{
    $parts = explode('/', $cidr, 2);
    if (count($parts) !== 2 || !ctype_digit($parts[1])) {
        return false;
    }
    $packed = @inet_pton($parts[0]);
    return $packed !== false && strlen($packed) === 4
        && (int) $parts[1] >= 0 && (int) $parts[1] <= 32;
}

/** MariaDB can render an IPv4-mapped INET6 value; keep the operator view concise. */
function network_allocator_display_ip(string $ip): ?string
{
    if ($ip === '') {
        return null;
    }
    return preg_match('/^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i', $ip, $match) === 1
        ? $match[1] : $ip;
}

/** Return true if two syntactically valid IPv4 CIDRs share any address. */
function network_allocator_ipv4_overlaps(string $a, string $b): bool
{
    if (!network_allocator_is_ipv4_cidr($a) || !network_allocator_is_ipv4_cidr($b)) {
        return false;
    }
    [$ipA, $prefixA] = explode('/', $a, 2);
    [$ipB, $prefixB] = explode('/', $b, 2);
    $common = min((int) $prefixA, (int) $prefixB);
    $packedA = inet_pton($ipA);
    $packedB = inet_pton($ipB);
    $wholeBytes = intdiv($common, 8);
    if (substr($packedA, 0, $wholeBytes) !== substr($packedB, 0, $wholeBytes)) {
        return false;
    }
    $remainingBits = $common % 8;
    if ($remainingBits === 0) {
        return true;
    }
    $mask = (0xff << (8 - $remainingBits)) & 0xff;
    return (ord($packedA[$wholeBytes]) & $mask) === (ord($packedB[$wholeBytes]) & $mask);
}
