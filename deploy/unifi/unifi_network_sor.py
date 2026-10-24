#!/usr/bin/env python3
"""Read UniFi network configuration into the SoR (never writes to UniFi).

The default mode is a dry run. `--apply` updates the SoR's `vlans`, `networks`,
`subnets`, `dhcp_ranges`, and `external_refs` in one transaction. Network rows
remain allocator-ineligible until a human explicitly approves them.

API access requires UNIFI_GW, UNIFI_API_KEY, and UNIFI_CA_FILE. The CA file must
verify the controller certificate; this importer does not disable TLS checks.
Database credentials come from SORSYNC_CONF [sor_unifi], with [sor] retained
as the backwards-compatible staged-upgrade fallback.
"""
import argparse
import configparser
import ipaddress
import json
import os
import sys
from pathlib import Path
from urllib.parse import quote

from unifipolld import UniFiClient, data_items, pick

try:
    import pymysql
except ImportError:  # Normalization and dry-run tests do not need a DB driver.
    pymysql = None

HERE = Path(__file__).resolve().parent
DEFAULT_SORSYNC_CONF = HERE.parent / "sorsync" / "sorsync.conf"
sys.path.insert(0, str(HERE.parent / "sorsync"))
from db_config import database_options

IMPORT_LOCK = "solarinet-unifi-network-import"
API_ROOT = "/proxy/network/integration/v1"


class ImportConflict(ValueError):
    """The SoR has an ambiguous identity that must be resolved by an operator."""


def _stored_ip(value):
    """Canonicalize MariaDB INET6 output, including IPv4-mapped IPv6 values."""
    if value is None:
        return None
    address = ipaddress.ip_address(str(value))
    return str(address.ipv4_mapped or address)


def _items(value):
    return data_items(value)


def _paginate(client, path):
    offset = 0
    all_rows = []
    while True:
        separator = "&" if "?" in path else "?"
        page = client.get(f"{path}{separator}offset={offset}&limit=200")
        rows = _items(page)
        all_rows.extend(rows)
        total = page.get("totalCount") if isinstance(page, dict) else None
        if total is None or offset + len(rows) >= int(total):
            return all_rows
        if not rows:
            raise RuntimeError(f"UniFi pagination stopped early for {path}")
        offset += len(rows)


def normalize_network(site, detail):
    """Validate one API detail response and return allocator-neutral SoR facts."""
    site_id = str(pick(site, "id", "siteId", default="")).strip()
    network_id = str(pick(detail, "id", "networkId", default="")).strip()
    name = str(pick(detail, "name", default="")).strip()
    if not site_id or not network_id or not name:
        raise ValueError("UniFi site/network response is missing a stable id or name")

    raw_vlan = pick(detail, "vlanId")
    vlan_id = None if raw_vlan is None else int(raw_vlan)
    if vlan_id is not None and not 1 <= vlan_id <= 4094:
        raise ValueError(f"network {name!r} has invalid VLAN id {vlan_id}")

    ipv4 = pick(detail, "ipv4Configuration", default={}) or {}
    cidr = gateway = None
    prefix_len = None
    host_ip = pick(ipv4, "hostIpAddress")
    prefix = pick(ipv4, "prefixLength")
    if host_ip is not None or prefix is not None:
        if host_ip is None or prefix is None:
            raise ValueError(f"network {name!r} has incomplete IPv4 subnet data")
        address = ipaddress.ip_address(str(host_ip))
        if address.version != 4:
            raise ValueError(f"network {name!r} returned non-IPv4 hostIpAddress")
        subnet = ipaddress.ip_network(f"{address}/{int(prefix)}", strict=False)
        cidr, prefix_len, gateway = str(subnet), subnet.prefixlen, str(address)

    dhcp = pick(ipv4, "dhcpConfiguration", default={}) or {}
    dhcp_mode = str(pick(dhcp, "mode", default="NONE")).upper()
    pool = pick(dhcp, "ipAddressRange", default=None)
    pool_start = pool_end = None
    lease_seconds = pick(dhcp, "leaseTimeSeconds")
    if dhcp_mode == "SERVER":
        if cidr is None or not isinstance(pool, dict):
            raise ValueError(f"network {name!r} reports DHCP SERVER without subnet/range")
        pool_start = str(ipaddress.ip_address(str(pick(pool, "start"))))
        pool_end = str(ipaddress.ip_address(str(pick(pool, "stop"))))
        subnet = ipaddress.ip_network(cidr)
        start_ip, end_ip = ipaddress.ip_address(pool_start), ipaddress.ip_address(pool_end)
        if start_ip.version != 4 or end_ip.version != 4 or start_ip > end_ip:
            raise ValueError(f"network {name!r} has invalid DHCP range ordering/family")
        if start_ip not in subnet or end_ip not in subnet:
            raise ValueError(f"network {name!r} DHCP range is outside {cidr}")
        if lease_seconds is not None and int(lease_seconds) <= 0:
            raise ValueError(f"network {name!r} has invalid DHCP lease time")

    return {
        "site_id": site_id,
        "site_name": str(pick(site, "name", default=site_id)),
        "network_id": network_id,
        "name": name,
        "enabled": bool(pick(detail, "enabled", default=True)),
        "management": pick(detail, "management"),
        "is_default": bool(pick(detail, "isDefault", "default", default=False)),
        "vlan_id": vlan_id,
        "cidr": cidr,
        "prefix_len": prefix_len,
        "gateway_ip": gateway,
        "dhcp_mode": dhcp_mode,
        "range_start": pool_start,
        "range_end": pool_end,
        "lease_seconds": None if lease_seconds is None else int(lease_seconds),
    }


def fetch_snapshot(client):
    """Fetch complete sites, network pages, and per-network detail records."""
    sites = _paginate(client, f"{API_ROOT}/sites")
    if not sites:
        raise RuntimeError("UniFi API returned no sites; refusing an empty import")
    snapshots = []
    for site in sites:
        site_id = str(pick(site, "id", "siteId", default="")).strip()
        if not site_id:
            raise ValueError("UniFi site response is missing its id")
        site_path = f"{API_ROOT}/sites/{quote(site_id, safe='')}"
        overview = _paginate(client, f"{site_path}/networks")
        if not overview:
            raise RuntimeError(f"UniFi site {site_id} returned no networks; refusing an empty import")
        networks = []
        seen = set()
        for row in overview:
            network_id = str(pick(row, "id", "networkId", default="")).strip()
            if not network_id or network_id in seen:
                raise ValueError(f"UniFi site {site_id} has a missing or duplicate network id")
            seen.add(network_id)
            detail = client.get(f"{site_path}/networks/{quote(network_id, safe='')}")
            networks.append(normalize_network(site, detail))
        snapshots.append({"site_id": site_id, "networks": networks})
    return snapshots


def _source_id(cur):
    cur.execute("SELECT id FROM sources WHERE slug='unifi' AND deleted_at IS NULL FOR UPDATE")
    row = cur.fetchone()
    if not row:
        raise RuntimeError("SoR source 'unifi' is missing or soft-deleted")
    return row[0]


def _external_ref(cur, source_id, subject_table, external_id):
    cur.execute(
        "SELECT id, subject_id, deleted_at FROM external_refs "
        "WHERE source_id=%s AND subject_table=%s AND external_id=%s "
        "ORDER BY id DESC LIMIT 1 FOR UPDATE",
        (source_id, subject_table, external_id))
    return cur.fetchone()


def _set_external_ref(cur, source_id, subject_table, external_id, subject_id):
    current = _external_ref(cur, source_id, subject_table, external_id)
    if current:
        ref_id = current[0]
        cur.execute(
            "UPDATE external_refs SET subject_id=%s, asserted_kind='machine', "
            "asserted_at=UTC_TIMESTAMP(), deleted_at=NULL WHERE id=%s",
            (subject_id, ref_id))
        return ref_id
    cur.execute(
        "INSERT INTO external_refs "
        "(source_id,subject_table,subject_id,external_id,asserted_kind,asserted_at) "
        "VALUES (%s,%s,%s,%s,'machine',UTC_TIMESTAMP())",
        (source_id, subject_table, subject_id, external_id))
    return cur.lastrowid


def _upsert_vlan(cur, source_id, record):
    tag = record["vlan_id"]
    if tag is None:
        return None
    ext_id = f"{record['site_id']}:vlan:{tag}"
    ref = _external_ref(cur, source_id, "vlans", ext_id)
    if ref:
        vlan_id = ref[1]
        cur.execute("SELECT vlan_tag,source_id FROM vlans WHERE id=%s FOR UPDATE", (vlan_id,))
        current = cur.fetchone()
        if not current:
            raise ImportConflict(f"external reference for VLAN {ext_id} points to a missing SoR row")
        if current[0] != tag or current[1] != source_id:
            raise ImportConflict(f"external reference for VLAN {ext_id} points to a different source or tag")
        cur.execute(
            "UPDATE vlans SET name=%s,source_id=%s,asserted_kind='machine', "
            "asserted_at=UTC_TIMESTAMP(),deleted_at=NULL WHERE id=%s",
            (record["name"], source_id, vlan_id))
    else:
        cur.execute("SELECT id,name,source_id FROM vlans WHERE vlan_tag=%s AND deleted_at IS NULL", (tag,))
        matches = cur.fetchall()
        if len(matches) > 1:
            raise ImportConflict(f"VLAN {tag} has multiple live SoR rows")
        if matches:
            vlan_id, existing_name, existing_source = matches[0]
            if existing_name != record["name"]:
                raise ImportConflict(
                    f"VLAN {tag} is already named {existing_name!r}; UniFi reports {record['name']!r}")
            if existing_source == source_id:
                cur.execute(
                    "UPDATE vlans SET asserted_kind='machine',asserted_at=UTC_TIMESTAMP(),deleted_at=NULL WHERE id=%s",
                    (vlan_id,))
        else:
            cur.execute(
                "INSERT INTO vlans (vlan_tag,name,purpose,source_id,asserted_kind,asserted_at) "
                "VALUES (%s,%s,%s,%s,'machine',UTC_TIMESTAMP())",
                (tag, record["name"], f"Imported from UniFi site {record['site_name']}", source_id))
            vlan_id = cur.lastrowid
    _set_external_ref(cur, source_id, "vlans", ext_id, vlan_id)
    return vlan_id


def _upsert_network(cur, source_id, record, vlan_id):
    ext_id = f"{record['site_id']}:{record['network_id']}"
    ref = _external_ref(cur, source_id, "networks", ext_id)
    if ref:
        network_id = ref[1]
        cur.execute(
            "SELECT name,vlan_id,enabled,management,is_default,allocator_eligible,deleted_at,source_id "
            "FROM networks WHERE id=%s FOR UPDATE", (network_id,))
        current = cur.fetchone()
        if not current:
            raise ImportConflict(f"external reference for network {ext_id} points to a missing SoR row")
        if current[7] != source_id:
            raise ImportConflict(f"external reference for network {ext_id} points to a different SoR source")
        changed = (current[0] != record["name"] or current[1] != vlan_id
                   or bool(current[2]) != record["enabled"] or current[3] != record["management"]
                   or bool(current[4]) != record["is_default"] or current[6] is not None)
        cur.execute(
            "SELECT id FROM networks WHERE name=%s AND deleted_at IS NULL AND id<>%s LIMIT 1",
            (record["name"], network_id))
        if cur.fetchone():
            raise ImportConflict(f"network rename to {record['name']!r} conflicts with another live SoR row")
        eligibility = 0 if changed else int(bool(current[5]))
        cur.execute(
            "UPDATE networks SET name=%s,vlan_id=%s,enabled=%s,management=%s,is_default=%s, "
            "allocator_eligible=%s,allocator_approved_by=IF(%s,NULL,allocator_approved_by),"
            "allocator_approved_at=IF(%s,NULL,allocator_approved_at),source_id=%s,asserted_kind='machine',"
            "asserted_at=UTC_TIMESTAMP(),deleted_at=NULL WHERE id=%s",
            (record["name"], vlan_id, int(record["enabled"]), record["management"],
             int(record["is_default"]), eligibility, int(changed), int(changed), source_id, network_id))
    else:
        cur.execute("SELECT id,source_id FROM networks WHERE name=%s AND deleted_at IS NULL", (record["name"],))
        matches = cur.fetchall()
        if len(matches) > 1:
            raise ImportConflict(f"network name {record['name']!r} has multiple live SoR rows")
        if matches and matches[0][1] != source_id:
            raise ImportConflict(
                f"network name {record['name']!r} already belongs to a different SoR source")
        if matches:
            network_id = matches[0][0]
            cur.execute(
                "UPDATE networks SET vlan_id=%s,enabled=%s,management=%s,is_default=%s, "
                "allocator_eligible=0,allocator_approved_by=NULL,allocator_approved_at=NULL,"
                "asserted_kind='machine',asserted_at=UTC_TIMESTAMP(),"
                "deleted_at=NULL WHERE id=%s",
                (vlan_id, int(record["enabled"]), record["management"],
                 int(record["is_default"]), network_id))
        else:
            purpose = f"UniFi network from site {record['site_name']}"
            cur.execute(
                "INSERT INTO networks (name,purpose,vlan_id,enabled,management,is_default,"
                "allocator_eligible,source_id,asserted_kind,asserted_at) "
                "VALUES (%s,%s,%s,%s,%s,%s,0,%s,'machine',UTC_TIMESTAMP())",
                (record["name"], purpose, vlan_id, int(record["enabled"]), record["management"],
                 int(record["is_default"]), source_id))
            network_id = cur.lastrowid
    _set_external_ref(cur, source_id, "networks", ext_id, network_id)
    return network_id


def _upsert_subnet(cur, source_id, record, network_id):
    if record["cidr"] is None:
        return None
    ext_id = f"{record['site_id']}:{record['network_id']}:ipv4"
    imported_prefix = ipaddress.ip_network(record["cidr"], strict=False)
    cur.execute(
        "SELECT s.network_id,s.cidr FROM subnets s JOIN networks n ON n.id=s.network_id "
        "WHERE s.network_id<>%s AND s.deleted_at IS NULL AND n.deleted_at IS NULL FOR UPDATE",
        (network_id,))
    for other_network_id, other_cidr in cur.fetchall():
        try:
            other_prefix = ipaddress.ip_network(other_cidr, strict=False)
        except ValueError:
            continue
        if imported_prefix.version == 4 and other_prefix.version == 4 and imported_prefix.overlaps(other_prefix):
            # Overlapping controller state invalidates any prior approval on
            # the affected segment. Allocator checks eligibility under the
            # same subnet-row lock after this transaction commits.
            cur.execute(
                "UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,"
                "allocator_approved_at=NULL WHERE id=%s", (other_network_id,))
    ref = _external_ref(cur, source_id, "subnets", ext_id)
    if ref:
        subnet_id = ref[1]
        cur.execute("SELECT cidr,CAST(gateway_ip AS CHAR),deleted_at,source_id,network_id "
                    "FROM subnets WHERE id=%s FOR UPDATE",
                    (subnet_id,))
        current = cur.fetchone()
        if not current:
            raise ImportConflict(f"external reference for subnet {ext_id} points to a missing SoR row")
        if current[3] != source_id:
            raise ImportConflict(f"external reference for subnet {ext_id} points to a different SoR source")
        if current[4] != network_id:
            raise ImportConflict(f"external reference for subnet {ext_id} points to a different SoR network")
        if (current[0] != record["cidr"] or _stored_ip(current[1]) != record["gateway_ip"]
                or current[2] is not None):
            cur.execute("UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,"
                        "allocator_approved_at=NULL WHERE id=%s", (network_id,))
        cur.execute(
            "UPDATE subnets SET network_id=%s,cidr=%s,net_address=CAST(%s AS INET6),prefix_len=%s,"
            "gateway_ip=CAST(%s AS INET6),description=%s,source_id=%s,asserted_kind='machine',"
            "asserted_at=UTC_TIMESTAMP(),deleted_at=NULL WHERE id=%s",
            (network_id, record["cidr"], record["cidr"].split("/")[0], record["prefix_len"],
             record["gateway_ip"], f"UniFi observed: {record['name']}", source_id, subnet_id))
    else:
        cur.execute("SELECT id,network_id,source_id,CAST(gateway_ip AS CHAR) FROM subnets "
                    "WHERE cidr=%s AND deleted_at IS NULL",
                    (record["cidr"],))
        matches = cur.fetchall()
        if len(matches) > 1:
            raise ImportConflict(f"subnet {record['cidr']} has multiple live SoR rows")
        if matches and (matches[0][1] != network_id or matches[0][2] != source_id):
            raise ImportConflict(
                f"subnet {record['cidr']} is already asserted by another network/source; reconcile before import")
        if matches:
            subnet_id = matches[0][0]
            if _stored_ip(matches[0][3]) != record["gateway_ip"]:
                cur.execute("UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,"
                            "allocator_approved_at=NULL WHERE id=%s", (network_id,))
            cur.execute(
                "UPDATE subnets SET gateway_ip=CAST(%s AS INET6),description=%s,asserted_kind='machine',"
                "asserted_at=UTC_TIMESTAMP(),deleted_at=NULL WHERE id=%s",
                (record["gateway_ip"], f"UniFi observed: {record['name']}", subnet_id))
        else:
            cur.execute(
                "INSERT INTO subnets (network_id,cidr,net_address,prefix_len,gateway_ip,description,"
                "source_id,asserted_kind,asserted_at) VALUES (%s,%s,CAST(%s AS INET6),%s,"
                "CAST(%s AS INET6),%s,%s,'machine',UTC_TIMESTAMP())",
                (network_id, record["cidr"], record["cidr"].split("/")[0], record["prefix_len"],
                 record["gateway_ip"], f"UniFi observed: {record['name']}", source_id))
            subnet_id = cur.lastrowid
    _set_external_ref(cur, source_id, "subnets", ext_id, subnet_id)
    return subnet_id


def _upsert_dhcp_range(cur, source_id, record, subnet_id):
    if subnet_id is None or record["dhcp_mode"] != "SERVER":
        return None
    ext_id = f"{record['site_id']}:{record['network_id']}:ipv4:dhcp"
    ref = _external_ref(cur, source_id, "dhcp_ranges", ext_id)
    gateway_entity = None
    if record["management"] == "GATEWAY":
        cur.execute("SELECT id FROM entities WHERE name='chemistry' AND deleted_at IS NULL LIMIT 2")
        entities = cur.fetchall()
        if len(entities) == 1:
            gateway_entity = entities[0][0]
    note = f"UniFi site={record['site_name']} mode=SERVER lease={record['lease_seconds']}s"
    if ref:
        range_id = ref[1]
        cur.execute("SELECT CAST(range_start AS CHAR),CAST(range_end AS CHAR),deleted_at,source_id,subnet_id "
                    "FROM dhcp_ranges WHERE id=%s FOR UPDATE", (range_id,))
        current = cur.fetchone()
        if not current:
            raise ImportConflict(f"external reference for DHCP range {ext_id} points to a missing SoR row")
        if current[3] != source_id:
            raise ImportConflict(f"external reference for DHCP range {ext_id} points to a different SoR source")
        if current[4] != subnet_id:
            raise ImportConflict(f"external reference for DHCP range {ext_id} points to a different SoR subnet")
        if (_stored_ip(current[0]) != record["range_start"]
                or _stored_ip(current[1]) != record["range_end"]
                or current[2] is not None):
            cur.execute("UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,"
                        "allocator_approved_at=NULL WHERE id=%s", (record["sor_network_id"],))
        cur.execute(
            "UPDATE dhcp_ranges SET subnet_id=%s,range_start=CAST(%s AS INET6),range_end=CAST(%s AS INET6),"
            "kind='dynamic_pool',server_entity_id=%s,notes=%s,source_id=%s,asserted_kind='machine',"
            "asserted_at=UTC_TIMESTAMP(),deleted_at=NULL WHERE id=%s",
            (subnet_id, record["range_start"], record["range_end"], gateway_entity, note, source_id, range_id))
    else:
        cur.execute("UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,"
                    "allocator_approved_at=NULL WHERE id=%s", (record["sor_network_id"],))
        cur.execute(
            "INSERT INTO dhcp_ranges (subnet_id,range_start,range_end,kind,server_entity_id,notes,"
            "source_id,asserted_kind,asserted_at) VALUES (%s,CAST(%s AS INET6),CAST(%s AS INET6),"
            "'dynamic_pool',%s,%s,%s,'machine',UTC_TIMESTAMP())",
            (subnet_id, record["range_start"], record["range_end"], gateway_entity, note, source_id))
        range_id = cur.lastrowid
    _set_external_ref(cur, source_id, "dhcp_ranges", ext_id, range_id)
    return range_id


def _prune_stale_refs(cur, source_id, site_id, expected):
    cur.execute(
        "SELECT id,subject_table,subject_id,external_id FROM external_refs "
        "WHERE source_id=%s AND external_id LIKE %s AND deleted_at IS NULL FOR UPDATE",
        (source_id, f"{site_id}:%"))
    stale = [row for row in cur.fetchall() if (row[1], row[3]) not in expected]
    for ref_id, table, subject_id, external_id in stale:
        if table == "dhcp_ranges":
            cur.execute(
                "UPDATE networks n JOIN subnets s ON s.network_id=n.id "
                "JOIN dhcp_ranges d ON d.subnet_id=s.id "
                "SET n.allocator_eligible=0,n.allocator_approved_by=NULL,n.allocator_approved_at=NULL "
                "WHERE d.id=%s", (subject_id,))
        elif table == "subnets":
            cur.execute(
                "UPDATE networks n JOIN subnets s ON s.network_id=n.id "
                "SET n.allocator_eligible=0,n.allocator_approved_by=NULL,n.allocator_approved_at=NULL "
                "WHERE s.id=%s", (subject_id,))
        cur.execute("UPDATE external_refs SET deleted_at=UTC_TIMESTAMP() WHERE id=%s", (ref_id,))
        cur.execute(
            "SELECT COUNT(*) FROM external_refs WHERE subject_table=%s AND subject_id=%s AND deleted_at IS NULL",
            (table, subject_id))
        if cur.fetchone()[0]:
            continue
        if table not in {"vlans", "networks", "subnets", "dhcp_ranges"}:
            continue
        if table == "networks":
            cur.execute(
                "UPDATE networks SET allocator_eligible=0,allocator_approved_by=NULL,"
                "allocator_approved_at=NULL WHERE id=%s AND source_id=%s",
                (subject_id, source_id))
        cur.execute(
            f"UPDATE `{table}` SET deleted_at=UTC_TIMESTAMP(),asserted_at=UTC_TIMESTAMP() "
            "WHERE id=%s AND source_id=%s AND deleted_at IS NULL", (subject_id, source_id))


def apply_snapshots(connection, snapshots):
    """Apply complete per-site snapshots atomically; raises and rolls back on conflict."""
    cur = connection.cursor()
    lock_acquired = False
    try:
        cur.execute("SELECT GET_LOCK(%s,0)", (IMPORT_LOCK,))
        lock_acquired = cur.fetchone()[0] == 1
        if not lock_acquired:
            raise RuntimeError("another UniFi network import is already running")
        connection.begin()
        source_id = _source_id(cur)
        for snapshot in snapshots:
            site_id = snapshot["site_id"]
            records = snapshot["networks"]
            if not records:
                raise RuntimeError(f"site {site_id} has no networks; refusing to prune SoR rows")
            expected = set()
            for record in records:
                if record["site_id"] != site_id:
                    raise ValueError("snapshot contains a network from a different site")
                vlan_id = _upsert_vlan(cur, source_id, record)
                network_id = _upsert_network(cur, source_id, record, vlan_id)
                record["sor_network_id"] = network_id
                subnet_id = _upsert_subnet(cur, source_id, record, network_id)
                _upsert_dhcp_range(cur, source_id, record, subnet_id)
                expected.add(("networks", f"{site_id}:{record['network_id']}"))
                if vlan_id is not None:
                    expected.add(("vlans", f"{site_id}:vlan:{record['vlan_id']}"))
                if subnet_id is not None:
                    expected.add(("subnets", f"{site_id}:{record['network_id']}:ipv4"))
                if subnet_id is not None and record["dhcp_mode"] == "SERVER":
                    expected.add(("dhcp_ranges", f"{site_id}:{record['network_id']}:ipv4:dhcp"))
            _prune_stale_refs(cur, source_id, site_id, expected)
        connection.commit()
    except Exception:
        connection.rollback()
        raise
    finally:
        if lock_acquired:
            try:
                cur.execute("SELECT RELEASE_LOCK(%s)", (IMPORT_LOCK,))
            except Exception:
                pass
        cur.close()


def _db_connect(config_path):
    if pymysql is None:
        raise RuntimeError("PyMySQL is required to apply a UniFi network import")
    config = configparser.ConfigParser()
    if not config.read(config_path):
        raise RuntimeError(f"cannot read SoR connection settings from {config_path}")
    sor = database_options(config, "sor_unifi")
    return pymysql.connect(
        host=sor["host"], port=sor["port"], user=sor["user"], password=sor["password"],
        database=sor["name"], charset="utf8mb4", autocommit=False)


def _client_from_env():
    gateway, key, cafile = (os.environ.get("UNIFI_GW"), os.environ.get("UNIFI_API_KEY"),
                            os.environ.get("UNIFI_CA_FILE"))
    if not gateway or not key or not cafile:
        raise RuntimeError("UNIFI_GW, UNIFI_API_KEY, and UNIFI_CA_FILE are required")
    if not Path(cafile).is_file():
        raise RuntimeError("UNIFI_CA_FILE does not name a readable controller trust file")
    return UniFiClient(gateway, key, cafile, connect_ip=os.environ.get("UNIFI_CONNECT_IP"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="write the fetched snapshot to the SoR")
    parser.add_argument("--sorsync-config", default=os.environ.get("SORSYNC_CONF", str(DEFAULT_SORSYNC_CONF)))
    args = parser.parse_args(argv)
    try:
        snapshots = fetch_snapshot(_client_from_env())
        for snapshot in snapshots:
            for row in snapshot["networks"]:
                print(json.dumps({**row, "allocator_eligible": False}, sort_keys=True))
        if args.apply:
            connection = _db_connect(args.sorsync_config)
            try:
                apply_snapshots(connection, snapshots)
            finally:
                connection.close()
            print("UniFi network facts applied to SoR; allocator eligibility unchanged")
        else:
            print("DRY-RUN: no SoR or UniFi configuration was changed; pass --apply to write SoR facts")
        return 0
    except Exception as error:  # one failed site must not create a partial snapshot
        print(f"unifi_network_sor: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
