<?php
declare(strict_types=1);

/**
 * Rollup — derive health from probeCurrent up the catalog hierarchy:
 *   vantages -> probe target -> asset (system) -> pool.
 *
 * State semantics match /api/probes (ProbeRollup): a vantage is healthy iff its
 * outcome is 'ok'. A target with no vantages (nothing has probed it yet) is
 * 'unknown'. An asset is the worst of its targets; a pool is a count of its
 * assets by state.
 */
final class Rollup
{
    /** Fold a set of probeCurrent vantage rows into a single state. */
    public static function fromVantages(array $vantages): string
    {
        if ($vantages === []) {
            return 'unknown';
        }
        $ok = 0; $bad = 0;
        foreach ($vantages as $v) {
            if (($v['outcome'] ?? null) === 'ok') { $ok++; } else { $bad++; }
        }
        if ($bad === 0) { return 'up'; }
        if ($ok === 0)  { return 'down'; }
        return 'degraded';
    }

    /** targetId => state, across all current vantages. */
    public static function targetStates(): array
    {
        $byT = [];
        foreach (Db::rows('SELECT targetId, outcome FROM probeCurrent') as $r) {
            $byT[(string) $r['targetId']][] = $r;
        }
        $out = [];
        foreach ($byT as $tid => $vs) {
            $out[$tid] = self::fromVantages($vs);
        }
        return $out;
    }

    /** assetId => state, rolling each asset's probe targets up (worst wins). */
    public static function assetStates(): array
    {
        $ts  = self::targetStates();
        $byA = [];
        foreach (Db::rows('SELECT assetId, targetId FROM probeTarget WHERE assetId IS NOT NULL') as $r) {
            $aid = (int) $r['assetId'];
            $byA[$aid][] = $ts[(string) $r['targetId']] ?? 'unknown';
        }
        $out = [];
        foreach ($byA as $aid => $states) {
            $out[$aid] = self::worst($states);
        }
        return $out;
    }

    /** Worst state in a list (down > degraded > up > unknown). */
    public static function worst(array $states): string
    {
        if (in_array('down', $states, true))     { return 'down'; }
        if (in_array('degraded', $states, true)) { return 'degraded'; }
        if (in_array('up', $states, true))       { return 'up'; }
        return 'unknown';
    }

    /** Empty {up,degraded,down,unknown} counter. */
    public static function emptyHealth(): array
    {
        return ['up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0];
    }
}
