#!/usr/bin/env python3
"""Read and validate UniFi topology for a later, reviewed SoR reconciliation.

This module is intentionally GET-only. It emits a deterministic snapshot and
checks internal consistency; it never changes UniFi or writes to the SoR.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import ipaddress
import json
import os
import sys
import uuid
from urllib.parse import urlencode, urlsplit

import unifipolld


class TopologyError(ValueError):
    pass


def _rows(value, label):
    if isinstance(value, list):
        return value
    if not isinstance(value, dict) or not isinstance(value.get("data"), list):
        raise TopologyError(f"UniFi {label} response has no data array")
    return value["data"]


def list_all(client, path, label, page_size=100):
    """Read a complete paginated list, rejecting inconsistent/partial pages."""
    result = []
    seen_ids = set()
    offset = 0
    expected = None
    while True:
        query = urlencode({"offset": offset, "limit": page_size})
        response = client.get(f"{path}?{query}")
        rows = _rows(response, label)
        if not isinstance(response, dict):
            raise TopologyError(f"UniFi {label} response is not paginated")
        count, limit, response_offset = response.get("count"), response.get("limit"), response.get("offset")
        total = response.get("totalCount")
        if (isinstance(total, bool) or not isinstance(total, int) or total < 0
                or isinstance(count, bool) or not isinstance(count, int) or count != len(rows)
                or isinstance(limit, bool) or not isinstance(limit, int) or limit < 1 or len(rows) > limit
                or isinstance(response_offset, bool) or response_offset != offset):
            raise TopologyError(f"UniFi {label} response has invalid pagination metadata")
        if expected is None:
            expected = total
        elif expected != total:
            raise TopologyError(f"UniFi {label} totalCount changed during pagination")
        for row in rows:
            if not isinstance(row, dict):
                raise TopologyError(f"UniFi {label} page contains a malformed row")
            identifier = validate_uuid(row.get("id"), label[:-1] if label.endswith("s") else label)
            if identifier in seen_ids:
                raise TopologyError(f"UniFi {label} pagination contains a duplicate ID")
            seen_ids.add(identifier)
        result.extend(rows)
        if len(result) > expected:
            raise TopologyError(f"UniFi {label} returned more rows than totalCount")
        offset += len(rows)
        if offset >= expected:
            break
        if not rows:
            raise TopologyError(f"UniFi {label} pagination stopped before totalCount")
    if len(result) != expected:
        raise TopologyError(f"UniFi {label} returned {len(result)} of {expected} rows")
    return result


def validate_uuid(value, label):
    if not isinstance(value, str):
        raise TopologyError(f"UniFi {label} has no UUID")
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError) as error:
        raise TopologyError(f"UniFi {label} has an invalid UUID") from error
    canonical = str(parsed)
    if canonical != value.lower():
        raise TopologyError(f"UniFi {label} has a noncanonical UUID")
    return canonical


def validate_gateway_url(value):
    parsed = urlsplit(value)
    if (parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment):
        raise TopologyError("UNIFI_GW must be an HTTPS URL without embedded credentials, query, or fragment")
    return value.rstrip("/")


def normalise_network(site, summary, detail):
    """Validate a UniFi network and map only fields represented by the SoR."""
    network_id = validate_uuid(summary.get("id"), "network")
    if validate_uuid(detail.get("id"), "network detail") != network_id:
        raise TopologyError(f"UniFi network detail id mismatch for {network_id}")
    name = detail.get("name")
    if not isinstance(name, str) or not name.strip() or len(name) > 64:
        raise TopologyError(f"UniFi network {network_id} has an invalid name")
    if not isinstance(detail.get("enabled"), bool):
        raise TopologyError(f"UniFi network {network_id} has no enabled state")
    management = detail.get("management")
    if management not in ("GATEWAY", "SWITCH", "UNMANAGED"):
        raise TopologyError(f"UniFi network {network_id} has an unsupported management mode")
    vlan_tag = detail.get("vlanId")
    if isinstance(vlan_tag, bool) or not isinstance(vlan_tag, int) or not 1 <= vlan_tag <= 4094:
        raise TopologyError(f"UniFi network {network_id} has an invalid VLAN tag")

    result = {
        "id": network_id,
        "site_id": site["id"],
        "site_name": site.get("name", ""),
        "name": name,
        "vlan_tag": vlan_tag,
        "enabled": detail["enabled"],
        "management": management,
        "subnet": None,
        "gateway_ip": None,
        "dhcp": None,
    }
    ipv4 = detail.get("ipv4Configuration")
    if ipv4 is None:
        return result
    if not isinstance(ipv4, dict):
        raise TopologyError(f"UniFi network {network_id} has malformed IPv4 configuration")
    host = ipv4.get("hostIpAddress")
    prefix = ipv4.get("prefixLength")
    if not isinstance(host, str) or isinstance(prefix, bool) or not isinstance(prefix, int):
        raise TopologyError(f"UniFi network {network_id} has incomplete IPv4 subnet data")
    try:
        address = ipaddress.ip_address(host)
        subnet = ipaddress.ip_network(f"{host}/{prefix}", strict=False)
    except ValueError as error:
        raise TopologyError(f"UniFi network {network_id} has invalid IPv4 subnet data") from error
    if address.version != 4 or subnet.version != 4:
        raise TopologyError(f"UniFi network {network_id} has non-IPv4 gateway data")
    result["subnet"] = str(subnet)
    result["gateway_ip"] = str(address)

    dhcp_config = ipv4.get("dhcpConfiguration")
    if dhcp_config is not None:
        if not isinstance(dhcp_config, dict):
            raise TopologyError(f"UniFi network {network_id} has malformed DHCP configuration")
        mode = dhcp_config.get("mode")
        if mode == "SERVER":
            address_range = dhcp_config.get("ipAddressRange")
            if not isinstance(address_range, dict):
                raise TopologyError(f"UniFi network {network_id} has no DHCP range")
            start, stop = address_range.get("start"), address_range.get("stop")
            try:
                first, last = ipaddress.ip_address(start), ipaddress.ip_address(stop)
            except (TypeError, ValueError) as error:
                raise TopologyError(f"UniFi network {network_id} has an invalid DHCP range") from error
            if (first.version != 4 or last.version != 4 or first not in subnet or last not in subnet
                    or int(first) > int(last) or int(first) <= int(address) <= int(last)
                    or first in (subnet.network_address, subnet.broadcast_address)
                    or last in (subnet.network_address, subnet.broadcast_address)):
                raise TopologyError(f"UniFi network {network_id} DHCP range is outside or conflicts with its subnet")
            result["dhcp"] = {"mode": mode, "start": str(first), "stop": str(last)}
        elif mode == "RELAY":
            raise TopologyError(f"UniFi network {network_id} uses DHCP relay, which this snapshot contract does not yet represent")
        else:
            raise TopologyError(f"UniFi network {network_id} has unsupported DHCP mode")
    return result


def collect_topology(client, site_id=None):
    sites = list_all(client, "/proxy/network/integration/v1/sites", "sites")
    ids = set()
    for site in sites:
        if not isinstance(site, dict):
            raise TopologyError("UniFi sites list contains a malformed row")
        site_id_value = validate_uuid(site.get("id"), "site")
        if site_id_value in ids:
            raise TopologyError("UniFi sites list contains duplicate IDs")
        ids.add(site_id_value)
        site["id"] = site_id_value
    if site_id:
        site_id = validate_uuid(site_id, "configured site")
        sites = [site for site in sites if validate_uuid(site["id"], "site") == site_id]
        if len(sites) != 1:
            raise TopologyError("Configured UniFi site id did not match exactly one site")
    elif len(sites) != 1:
        raise TopologyError("Set UNIFI_SITE_ID when UniFi returns more than one site")
    if not sites:
        raise TopologyError("UniFi returned no sites")

    output = []
    for site in sites:
        networks = list_all(client, f"/proxy/network/integration/v1/sites/{site['id']}/networks", "networks")
        if not networks:
            raise TopologyError(f"UniFi site {site['id']} returned no networks")
        normalized = []
        network_ids = set()
        for summary in networks:
            if not isinstance(summary, dict):
                raise TopologyError("UniFi networks list contains a malformed row")
            network_id = validate_uuid(summary.get("id"), "network")
            if network_id in network_ids:
                raise TopologyError("UniFi networks list contains duplicate IDs")
            network_ids.add(network_id)
            detail = client.get(f"/proxy/network/integration/v1/sites/{site['id']}/networks/{network_id}")
            if not isinstance(detail, dict):
                raise TopologyError(f"UniFi network detail is malformed for {network_id}")
            normalized.append(normalise_network(site, summary, detail))
        output.append({"id": site["id"], "name": site.get("name", ""), "networks": normalized})
    snapshot = {"schema_version": 1,
                "collected_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
                "sites": output}
    snapshot["source_subnet_conflicts"] = internal_subnet_conflicts(snapshot)
    return snapshot


def subnet_conflicts(snapshot, existing_subnets):
    """Return overlapping live SoR prefixes; equal CIDRs are reported as matches."""
    existing = []
    for row in existing_subnets:
        try:
            existing.append((row["cidr"], ipaddress.ip_network(row["cidr"], strict=False)))
        except (KeyError, TypeError, ValueError) as error:
            raise TopologyError("SoR subnet query returned invalid CIDR data") from error
    conflicts = []
    for site in snapshot["sites"]:
        for network in site["networks"]:
            cidr = network["subnet"]
            if cidr is None:
                continue
            candidate = ipaddress.ip_network(cidr)
            for existing_cidr, prefix in existing:
                if candidate.overlaps(prefix):
                    conflicts.append({"site": site["name"], "network": network["name"],
                                      "source_cidr": cidr, "sor_cidr": existing_cidr,
                                      "relation": "same" if candidate == prefix else "overlap"})
    return conflicts


def compare_sor(snapshot, sor_state):
    """Build a conservative, read-only reconciliation report for UniFi networks.

    Names and VLAN tags are useful review hints, not identity. Native IDs in
    external_refs are the only exact correlation. Any non-identical overlapping
    subnet blocks an unambiguous prefix match; this function never proposes a
    SoR write.
    """
    networks = sor_state.get("networks", [])
    vlans = sor_state.get("vlans", [])
    subnets = sor_state.get("subnets", [])
    refs = sor_state.get("external_refs", [])
    by_external = {}
    for ref in refs:
        key = (ref.get("subject_table"), str(ref.get("external_id", "")).lower())
        by_external.setdefault(key, []).append(ref.get("subject_id"))

    rows = []
    for site in snapshot["sites"]:
        for source in site["networks"]:
            name_matches = [row for row in networks
                            if str(row.get("name", "")).casefold() == source["name"].casefold()]
            vlan_matches = [row for row in vlans if row.get("vlan_tag") == source["vlan_tag"]]
            source_subnet = source.get("subnet")
            prefix_matches = []
            if source_subnet:
                source_prefix = ipaddress.ip_network(source_subnet)
                for row in subnets:
                    try:
                        prefix = ipaddress.ip_network(row["cidr"], strict=False)
                    except (KeyError, TypeError, ValueError) as error:
                        raise TopologyError("SoR subnet query returned invalid CIDR data") from error
                    if source_prefix.overlaps(prefix):
                        prefix_matches.append({
                            "subnet_id": row.get("id"), "cidr": str(prefix),
                            "network_id": row.get("network_id"),
                            "network_name": row.get("network_name"),
                            "relation": "same" if source_prefix == prefix else "overlap",
                        })
            native_network_ids = by_external.get(("networks", source["id"].lower()), [])
            reasons = []
            if snapshot.get("source_subnet_conflicts"):
                reasons.append("source_snapshot_has_internal_overlap")
            if len(native_network_ids) > 1:
                reasons.append("duplicate_external_network_refs")
            if not native_network_ids:
                reasons.append("network_external_ref_missing")
            if len(name_matches) > 1:
                reasons.append("network_name_candidate_ambiguous")
            if len(vlan_matches) == 0:
                reasons.append("vlan_tag_not_imported")
            elif len(vlan_matches) > 1:
                reasons.append("vlan_tag_candidate_ambiguous")
            if len(native_network_ids) == 1:
                exact_network = [row for row in networks if row.get("id") == native_network_ids[0]]
                if len(exact_network) != 1:
                    reasons.append("external_network_ref_target_missing")
                elif len(vlan_matches) == 1 and exact_network[0].get("vlan_id") != vlan_matches[0].get("id"):
                    reasons.append("network_vlan_assignment_mismatch")
            if any(item["relation"] == "overlap" for item in prefix_matches):
                reasons.append("source_prefix_overlaps_sor_subnets")
            exact_prefixes = [item for item in prefix_matches if item["relation"] == "same"]
            if source_subnet and len(exact_prefixes) != 1:
                reasons.append("no_exact_sor_subnet")
            elif exact_prefixes and native_network_ids and exact_prefixes[0].get("network_id") not in native_network_ids:
                reasons.append("subnet_network_correlation_mismatch")
            if source.get("gateway_ip") and exact_prefixes:
                sor_subnet = next(row for row in subnets if row.get("id") == exact_prefixes[0]["subnet_id"])
                if sor_subnet.get("gateway_ip") != source["gateway_ip"]:
                    reasons.append("gateway_mismatch")
            dhcp_overlaps = []
            if source.get("dhcp") and source_subnet:
                pool_start = int(ipaddress.ip_address(source["dhcp"]["start"]))
                pool_end = int(ipaddress.ip_address(source["dhcp"]["stop"]))
                for subnet in subnets:
                    try:
                        prefix = ipaddress.ip_network(subnet["cidr"], strict=False)
                    except (KeyError, TypeError, ValueError) as error:
                        raise TopologyError("SoR subnet query returned invalid CIDR data") from error
                    if not ipaddress.ip_network(source_subnet).overlaps(prefix):
                        continue
                    for pool in sor_state.get("dhcp_ranges", []):
                        if pool.get("subnet_id") != subnet.get("id"):
                            continue
                        try:
                            start = int(ipaddress.ip_address(pool["range_start"]))
                            end = int(ipaddress.ip_address(pool["range_end"]))
                        except (KeyError, TypeError, ValueError) as error:
                            raise TopologyError("SoR DHCP range query returned invalid address data") from error
                        if pool_start <= end and start <= pool_end:
                            dhcp_overlaps.append({"range_id": pool.get("id"),
                                                  "subnet_id": subnet.get("id"),
                                                  "start": pool["range_start"], "end": pool["range_end"],
                                                  "relation": "same" if pool_start == start and pool_end == end else "overlap"})
                            reasons.append("dhcp_pool_overlaps_sor_range")
                if exact_prefixes:
                    exact_dhcp = [pool for pool in sor_state.get("dhcp_ranges", [])
                                  if pool.get("subnet_id") == exact_prefixes[0]["subnet_id"]
                                  and pool.get("kind") == "dynamic_pool"
                                  and pool.get("range_start") == source["dhcp"]["start"]
                                  and pool.get("range_end") == source["dhcp"]["stop"]]
                    if not exact_dhcp:
                        reasons.append("dhcp_range_mismatch")
            rows.append({
                "site": site["name"], "source_network_id": source["id"],
                "source_network_name": source["name"], "source_vlan_tag": source["vlan_tag"],
                "source_subnet": source_subnet, "source_gateway_ip": source.get("gateway_ip"),
                "source_dhcp": source.get("dhcp"),
                "correlated_network_ids": native_network_ids,
                "name_candidate_network_ids": [row.get("id") for row in name_matches],
                "name_candidate_networks": [{"id": row.get("id"), "name": row.get("name"),
                                             "purpose": row.get("purpose"), "vlan_id": row.get("vlan_id")}
                                            for row in name_matches],
                "tag_candidate_vlan_ids": [row.get("id") for row in vlan_matches],
                "tag_candidate_vlans": [{"id": row.get("id"), "vlan_tag": row.get("vlan_tag"),
                                         "name": row.get("name")} for row in vlan_matches],
                "overlapping_sor_subnets": prefix_matches,
                "overlapping_sor_dhcp_ranges": dhcp_overlaps,
                "review_reasons": sorted(set(reasons)),
                "status": "review_required" if reasons else "correlated",
            })
    return {"mode": "read_only_comparison", "write_performed": False, "networks": rows}


def load_sor_state():
    """Read the minimal network/IPAM slice in a read-only SoR transaction."""
    if unifipolld.pymysql is None:
        raise RuntimeError("pymysql is required for SoR comparison")
    password = os.environ.get("SOR_DB_PASSWORD") or os.environ.get("SOR_DB_PASS")
    if not password:
        raise RuntimeError("SOR_DB_PASSWORD is required for SoR comparison")
    connection = unifipolld.pymysql.connect(
        host=os.environ.get("SOR_DB_HOST", "10.1.0.200"),
        port=int(os.environ.get("SOR_DB_PORT", "3306")),
        user=os.environ.get("SOR_DB_USER", "solari"), password=password,
        database=os.environ.get("SOR_DB_NAME", "sor"), charset="utf8mb4",
        connect_timeout=5, read_timeout=10, write_timeout=10, autocommit=False,
    )
    try:
        with connection.cursor(unifipolld.pymysql.cursors.DictCursor) as cursor:
            cursor.execute("SET TRANSACTION READ ONLY")
            connection.begin()
            cursor.execute("SELECT id, vlan_tag, name FROM vlans WHERE deleted_at IS NULL ORDER BY vlan_tag, id")
            vlans = cursor.fetchall()
            cursor.execute("SELECT id, name, purpose, vlan_id FROM networks WHERE deleted_at IS NULL ORDER BY name, id")
            networks = cursor.fetchall()
            cursor.execute("SELECT sn.id, sn.cidr, sn.network_id, INET6_NTOA(sn.gateway_ip) AS gateway_ip, "
                           "n.name AS network_name FROM subnets sn LEFT JOIN networks n "
                           "ON n.id=sn.network_id AND n.deleted_at IS NULL "
                           "WHERE sn.deleted_at IS NULL ORDER BY sn.cidr, sn.id")
            subnets = cursor.fetchall()
            cursor.execute("SELECT id, subnet_id, INET6_NTOA(range_start) AS range_start, "
                           "INET6_NTOA(range_end) AS range_end, kind FROM dhcp_ranges "
                           "WHERE deleted_at IS NULL ORDER BY subnet_id, id")
            dhcp_ranges = cursor.fetchall()
            cursor.execute("SELECT er.subject_table, er.subject_id, er.external_id "
                           "FROM external_refs er JOIN sources s ON s.id=er.source_id "
                           "WHERE s.slug='unifi' AND er.deleted_at IS NULL "
                           "AND er.subject_table='networks' ORDER BY er.external_id")
            external_refs = cursor.fetchall()
        connection.commit()
        return {"vlans": vlans, "networks": networks, "subnets": subnets,
                "dhcp_ranges": dhcp_ranges, "external_refs": external_refs}
    except Exception:
        connection.rollback()
        raise
    finally:
        connection.close()


def internal_subnet_conflicts(snapshot):
    prefixes = []
    for site in snapshot["sites"]:
        for network in site["networks"]:
            if network["subnet"] is not None:
                prefixes.append((site["name"], network["name"], network["subnet"],
                                 ipaddress.ip_network(network["subnet"])))
    conflicts = []
    for index, left in enumerate(prefixes):
        for right in prefixes[index + 1:]:
            if left[3].overlaps(right[3]):
                conflicts.append({"left": f"{left[0]}/{left[1]} {left[2]}",
                                  "right": f"{right[0]}/{right[1]} {right[2]}",
                                  "relation": "same" if left[3] == right[3] else "overlap"})
    return conflicts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site-id", default=os.environ.get("UNIFI_SITE_ID"))
    parser.add_argument("--ca-file", default=os.environ.get("UNIFI_CA_FILE"))
    parser.add_argument("--allow-insecure-tls", action="store_true",
                        help="explicitly allow the controller's unverified self-signed certificate")
    parser.add_argument("--compare-sor", action="store_true",
                        help="include a read-only comparison with current SoR network/IPAM rows")
    args = parser.parse_args(argv)
    gateway, api_key = os.environ.get("UNIFI_GW"), os.environ.get("UNIFI_API_KEY")
    if not gateway or not api_key:
        parser.error("UNIFI_GW and UNIFI_API_KEY are required")
    if not args.ca_file and not args.allow_insecure_tls:
        parser.error("set UNIFI_CA_FILE or explicitly pass --allow-insecure-tls")
    if not args.ca_file:
        print("WARNING: TLS certificate verification is disabled; the UniFi API key is sent over an unverified connection.",
              file=sys.stderr)
    try:
        gateway = validate_gateway_url(gateway)
        client = unifipolld.UniFiClient(gateway, api_key, args.ca_file)
        snapshot = collect_topology(client, args.site_id)
        if args.compare_sor:
            snapshot["sor_comparison"] = compare_sor(snapshot, load_sor_state())
        print(json.dumps(snapshot, indent=2, sort_keys=True))
    except Exception as error:  # avoid logging request URLs or credential-bearing context
        print(f"unifi_topology: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
