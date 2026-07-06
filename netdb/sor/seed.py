#!/usr/bin/env python3
"""Seed the SolariNet System of Record (SoR, MariaDB `sor` on cesium) from the
live systems of record, in FK-safe order, with provenance on every row and
`external_refs` for native-key correlation so re-runs UPSERT instead of
duplicating.

Sources (priority order):
  1. netdb  -- netdb/akoria-hosts.yml (essential; the richest source)
  2. samba_ad -- Samba AD on radium (users/groups/members + akoria.org zone)
  3. forgejo -- Forgejo on cesium (repositories + forge entity)

Idempotency note: the schema's live-uniqueness pattern is UNIQUE(name,deleted_at)
with deleted_at NULL. In MariaDB NULLs are DISTINCT in a UNIQUE index, so that
does NOT dedupe live rows and `INSERT ... ON DUPLICATE KEY UPDATE` will NOT fire.
We therefore correlate explicitly: SELECT the live row by its natural key (or via
external_refs where no natural key exists), then UPDATE it or INSERT a new one.

Usage:
  SOR_DB_PASSWORD=...  [SSH_PASS=...] [AD_ADMIN_PASS=...] \
  python3 seed.py [--netdb] [--ad] [--forgejo] [--all]

  --netdb    seed from akoria-hosts.yml           (default if no flag given)
  --ad       also seed Samba AD (needs SSH_PASS + AD_ADMIN_PASS)
  --forgejo  also seed Forgejo repos (needs SSH_PASS)
  --all      netdb + ad + forgejo

Env:
  SOR_DB_HOST      default 10.1.0.200
  SOR_DB_USER      default solari
  SOR_DB_PASSWORD  required
  SOR_DB_NAME      default sor
  SSH_PASS         ssh password for jason@ (AD/forgejo collection)
  AD_ADMIN_PASS    Samba AD administrator password (AD only)
"""
import os
import sys
import re
import json
import datetime
import argparse
import subprocess

import pymysql

HERE = os.path.dirname(os.path.abspath(__file__))
YAML_SRC = os.path.join(HERE, "..", "akoria-hosts.yml")

NOW = datetime.datetime.now().replace(microsecond=0)

# Radium / cesium reachability (collection over SSH).
RADIUM_HOST = "10.1.0.10"
CESIUM_HOST = "10.1.0.200"
SSH_USER = "jason"
AD_ADMIN_USER = "administrator"

# ---------------------------------------------------------------------------
# Connection + generic upsert helpers
# ---------------------------------------------------------------------------

def connect():
    return pymysql.connect(
        host=os.environ.get("SOR_DB_HOST", "10.1.0.200"),
        user=os.environ.get("SOR_DB_USER", "solari"),
        password=os.environ["SOR_DB_PASSWORD"],
        database=os.environ.get("SOR_DB_NAME", "sor"),
        charset="utf8mb4",
        autocommit=False,
    )


def source_ids(cur):
    cur.execute("SELECT slug, id FROM sources")
    return {slug: sid for slug, sid in cur.fetchall()}


# running tally of inserted/updated rows per table for the final report
STATS = {}


def _bump(table, inserted):
    s = STATS.setdefault(table, {"insert": 0, "update": 0})
    s["insert" if inserted else "update"] += 1


def upsert(cur, table, keycols, row, id_col="id"):
    """SELECT live row by natural key `keycols`; UPDATE it or INSERT. Returns id.

    keycols: list of columns whose values identify the live row. `deleted_at IS
    NULL` is always appended. INET6 columns are compared/inserted via CAST so a
    dotted-quad string matches the v4-mapped stored form.
    """
    inet6 = row.get("__inet6__", ())
    def ph(col):
        return f"CAST(%s AS INET6)" if col in inet6 else "%s"

    where = " AND ".join(f"`{c}`={ph(c)}" for c in keycols) + " AND `deleted_at` IS NULL"
    cur.execute(f"SELECT `{id_col}` FROM `{table}` WHERE {where} LIMIT 1",
                [row[c] for c in keycols])
    hit = cur.fetchone()
    data = {k: v for k, v in row.items() if k != "__inet6__"}
    if hit:
        rid = hit[0]
        setcols = [c for c in data if c != id_col]
        if setcols:
            cur.execute(
                f"UPDATE `{table}` SET " +
                ",".join(f"`{c}`={ph(c)}" for c in setcols) +
                f" WHERE `{id_col}`=%s",
                [data[c] for c in setcols] + [rid])
        _bump(table, False)
        return rid
    cols = list(data)
    cur.execute(
        f"INSERT INTO `{table}` (" + ",".join(f"`{c}`" for c in cols) + ") VALUES (" +
        ",".join(ph(c) for c in cols) + ")",
        [data[c] for c in cols])
    _bump(table, True)
    return cur.lastrowid


def ref_get(cur, source_id, table, external_id):
    cur.execute(
        "SELECT subject_id FROM external_refs WHERE source_id=%s AND subject_table=%s "
        "AND external_id=%s AND deleted_at IS NULL LIMIT 1",
        (source_id, table, external_id))
    r = cur.fetchone()
    return r[0] if r else None


def ref_set(cur, source_id, table, subject_id, external_id, url=None):
    if ref_get(cur, source_id, table, external_id) is None:
        cur.execute(
            "INSERT INTO external_refs "
            "(source_id,subject_table,subject_id,external_id,external_url,asserted_kind,asserted_at) "
            "VALUES (%s,%s,%s,%s,%s,'machine',%s)",
            (source_id, table, subject_id, external_id, url, NOW))
        _bump("external_refs", True)


def upsert_by_ref(cur, source_id, table, external_id, keycols, row, url=None, id_col="id"):
    """Idempotent insert for tables with no natural key: correlate via external_refs."""
    rid = ref_get(cur, source_id, table, external_id)
    data = {k: v for k, v in row.items() if k != "__inet6__"}
    inet6 = row.get("__inet6__", ())
    def ph(col):
        return f"CAST(%s AS INET6)" if col in inet6 else "%s"
    if rid is not None:
        setcols = [c for c in data if c != id_col]
        if setcols:
            cur.execute(
                f"UPDATE `{table}` SET " + ",".join(f"`{c}`={ph(c)}" for c in setcols) +
                f" WHERE `{id_col}`=%s", [data[c] for c in setcols] + [rid])
        _bump(table, False)
        return rid
    cols = list(data)
    cur.execute(
        f"INSERT INTO `{table}` (" + ",".join(f"`{c}`" for c in cols) + ") VALUES (" +
        ",".join(ph(c) for c in cols) + ")", [data[c] for c in cols])
    _bump(table, True)
    rid = cur.lastrowid
    ref_set(cur, source_id, table, rid, external_id, url)
    return rid


# ---------------------------------------------------------------------------
# netdb classification helpers
# ---------------------------------------------------------------------------

VENDOR_PATTERNS = [
    (r"UDR|UGC|USW|UniFi|U7 Pro|Ubiquiti", "Ubiquiti"),
    (r"PowerEdge|Dell", "Dell"),
    (r"Raspberry Pi|Pi 500|Pi 5\b|CM5|Radio Pi", "Raspberry Pi"),
    (r"UGREEN", "UGREEN"),
    (r"Zima", "IceWhale"),
    (r"Epson", "Epson"),
    (r"Mac Mini|iPad|Apple", "Apple"),
    (r"Minisforum|MS-R1", "Minisforum"),
    (r"EliteDesk|HP ", "HP"),
    (r"Argon", "Argon40"),
    (r"PlayStation|Sony", "Sony"),
    (r"Google TV", "Google"),
    (r"\bNest\b", "Google Nest"),
    (r"Echo Dot", "Amazon"),
    (r"Tesla", "Tesla"),
    (r"Tachyon|Particle", "Particle"),
    (r"Luckfox", "Luckfox"),
    (r"Nettool", "Nettool.io"),
    (r"EufyMake", "EufyMake"),
    (r"Eufy", "Eufy"),
    (r"YouYeeToo", "YouYeeToo"),
    (r"EleksMaker", "EleksMaker"),
    (r"ArkKVM", "ArkKVM"),
    (r"Pawaii", "Pawaii"),
]

HWTYPE_PATTERNS = [
    (r"UDR|UGC|Dream Router|router", "router"),
    (r"USW|switch", "switch"),
    (r"U7 Pro|WiFi|AP\b", "ap"),
    (r"DXP4800|NAS", "nas"),
    (r"PowerEdge|X1 \(x86", "server"),
    (r"Zima|Raspberry Pi|Argon|Luckfox|CM5|Pi 5|Pi 500|Tachyon", "sbc"),
    (r"Mac Mini|EliteDesk|Minisforum", "workstation"),
    (r"iPad", "tablet"),
    (r"RR-600W|scanner", "scanner"),
    (r"ET-8550|EufyMake|printer", "printer"),
    (r"PlayStation", "console"),
    (r"Google TV|Echo Dot", "media"),
    (r"Nest|Pawaii", "iot"),
    (r"Tesla", "vehicle"),
    (r"ArkKVM", "kvm"),
    (r"MoCA", "moca"),
    (r"Eufybase", "camera"),
    (r"Nettool", "sensor"),
]


def parse_hw(hw):
    """Return (make, model, hw_type) or None when hw is unknown ('-','?','')."""
    if not hw or hw.strip() in ("-", "?", ""):
        return None
    make = None
    for pat, m in VENDOR_PATTERNS:
        if re.search(pat, hw, re.I):
            make = m
            break
    if make is None:
        make = hw.split()[0]
    hw_type = "other"
    for pat, t in HWTYPE_PATTERNS:
        if re.search(pat, hw, re.I):
            hw_type = t
            break
    model = hw.strip()
    # strip a leading duplicate of the make word for a tidier model string
    if model.lower().startswith(make.lower() + " "):
        model = model[len(make) + 1:]
    return make, model or hw.strip(), hw_type


ENTITY_TYPE_RULES = [
    (r"gateway|router|firewall|switch|WiFi|AP\b|MoCA|network tool|control panel|KVM", "network_gear"),
    (r"NVR|camera", "appliance"),
    (r"printer|scanner", "peripheral"),
    (r"NAS", "appliance"),
    (r"thermostat|water dish|Home Assistant|voice|media|console|smart ", "iot_device"),
    (r"tablet", "mobile_device"),
    (r"vehicle", "vehicle"),
    (r"workstation|SBC|dev|IDE", "workstation"),
    (r"monitoring|dashboard|BIND|Forgejo|AD DC|Keycloak|MariaDB|n8n|Grafana|"
     r"resolver|Pi-hole|time source|GPS|radio backend|services|repo|AI", "server"),
]


def classify_entity(hw, role):
    text = f"{hw or ''} {role or ''}"
    for pat, t in ENTITY_TYPE_RULES:
        if re.search(pat, text, re.I):
            return t
    return "other"


def lifecycle_from_status(status):
    if not status:
        return "active"
    s = status.lower()
    if s.startswith("planned"):
        return "planned"
    if "offline" in s:
        return "offline"
    if "retired" in s:
        return "retired"
    return "active"


# 2nd octet -> logical network segment (from the netdb comment groupings)
NETWORK_BY_OCTET = {
    0: ("core", "core networking + grandfathered core services"),
    1: ("production", "production services"),
    2: ("IoT", "IoT devices"),
    3: ("peripherals", "printers / scanners / peripherals"),
    4: ("personal", "personal devices + vehicles"),
    5: ("workstations", "workstations and SBCs"),
    6: ("netmgmt", "network management + observability"),
    7: ("security", "security / cameras / NVR"),
}


def prov(source_id, kind="human"):
    return {"source_id": source_id, "asserted_kind": kind, "asserted_at": NOW}


# ---------------------------------------------------------------------------
# 1. netdb seeder
# ---------------------------------------------------------------------------

def seed_netdb(cur, src):
    import yaml
    with open(YAML_SRC) as fh:
        d = yaml.safe_load(fh)
    sid = src["netdb"]
    P = prov(sid, "human")

    hosts = d["hosts"]
    ifaces = d.get("ifaces", {})
    cnames = d["cnames"]
    reserved = d.get("reserved", {})

    # ---- networks + subnets (from every IP present) -----------------------
    all_ips = [v["ip"] for v in hosts.values()]
    all_ips += list(ifaces.values())
    all_ips += [v["ip"] for v in reserved.values()]

    net_id = {}          # segment name -> networks.id
    subnet_id = {}       # "10.a.b.0/24" -> subnets.id
    for ip in all_ips:
        o = ip.split(".")
        seg_octet = int(o[1])
        seg_name, seg_purpose = NETWORK_BY_OCTET.get(seg_octet, ("other", "unclassified"))
        if seg_name not in net_id:
            net_id[seg_name] = upsert(cur, "networks", ["name"], {
                "name": seg_name, "purpose": seg_purpose, **P})
        cidr = ".".join(o[:3]) + ".0/24"
        if cidr not in subnet_id:
            subnet_id[cidr] = upsert(cur, "subnets", ["cidr"], {
                "cidr": cidr,
                "net_address": ".".join(o[:3]) + ".0",
                "prefix_len": 24,
                "network_id": net_id[seg_name],
                "description": f"{seg_name} /24",
                **prov(sid, "machine"),
                "__inet6__": ("net_address",),
            })

    def subnet_of(ip):
        return subnet_id[".".join(ip.split(".")[:3]) + ".0/24"]

    # ---- hosts -> hardware + entities + primary IP ------------------------
    ent_id = {}          # host name -> entities.id
    for name in sorted(hosts):
        h = hosts[name]
        hw_unit_id = None
        parsed = parse_hw(h.get("hw"))
        if parsed:
            make, model, hw_type = parsed
            model_id = upsert(cur, "hardware_models", ["make", "model"], {
                "make": make, "model": model, "hw_type": hw_type, **P})
            # hardware_units have no natural key -> correlate by host name
            hw_unit_id = upsert_by_ref(cur, sid, "hardware_units", name, None, {
                "model_id": model_id,
                "notes": f"realizes host {name}",
                **P})

        eid = upsert(cur, "entities", ["name"], {
            "name": name,
            "entity_type": classify_entity(h.get("hw"), h.get("role")),
            "role": h.get("role"),
            "lifecycle": lifecycle_from_status(h.get("status")),
            "hardware_unit_id": hw_unit_id,
            **P})
        ent_id[name] = eid
        ref_set(cur, sid, "entities", eid, name)

        upsert(cur, "ip_addresses", ["address"], {
            "address": h["ip"],
            "subnet_id": subnet_of(h["ip"]),
            "entity_id": eid,
            "assignment": "static",
            "is_primary": 1,
            **P,
            "__inet6__": ("address",),
        })

    # ---- reserved -> planned entity + reserved (non-primary) IP -----------
    for name in sorted(reserved):
        r = reserved[name]
        hw_unit_id = None
        parsed = parse_hw(r.get("hw"))
        if parsed:
            make, model, hw_type = parsed
            model_id = upsert(cur, "hardware_models", ["make", "model"], {
                "make": make, "model": model, "hw_type": hw_type, **P})
            hw_unit_id = upsert_by_ref(cur, sid, "hardware_units", name, None, {
                "model_id": model_id, "notes": f"reserved for {name}", **P})
        eid = upsert(cur, "entities", ["name"], {
            "name": name,
            "entity_type": classify_entity(r.get("hw"), r.get("role")),
            "role": r.get("role"),
            "lifecycle": "planned",
            "hardware_unit_id": hw_unit_id,
            **P})
        ent_id[name] = eid
        ref_set(cur, sid, "entities", eid, name)
        # reserved IP: NOT primary, entity-attached, so it stays out of DNS views
        upsert(cur, "ip_addresses", ["address"], {
            "address": r["ip"],
            "subnet_id": subnet_of(r["ip"]),
            "entity_id": eid,
            "assignment": "reserved",
            "is_primary": 0,
            "notes": "reserved (netdb reserved: block)",
            **P,
            "__inet6__": ("address",),
        })

    # ---- ifaces -> secondary interface + secondary IP ---------------------
    for label in sorted(ifaces):
        host, _, ifname = label.partition("-")
        if host not in ent_id:
            print(f"  WARN iface {label}: parent host {host} not found; skipping", file=sys.stderr)
            continue
        ip = ifaces[label]
        if_kind = "wifi" if "wifi" in ifname else "physical"
        iface_id = upsert(cur, "interfaces", ["entity_id", "name"], {
            "entity_id": ent_id[host],
            "name": ifname,
            "if_kind": if_kind,
            "notes": f"netdb iface {label}",
            **prov(sid, "human"),
        })
        upsert(cur, "ip_addresses", ["address"], {
            "address": ip,
            "subnet_id": subnet_of(ip),
            "interface_id": iface_id,
            "entity_id": ent_id[host],
            "assignment": "static",
            "is_primary": 0,
            **P,
            "__inet6__": ("address",),
        })

    # ---- dns zone (akoria.net, sor_rendered) ------------------------------
    zone_id = upsert(cur, "dns_zones", ["name"], {
        "name": d["domain"],
        "kind": "forward",
        "authority": "sor_rendered",
        "primary_ns_entity_id": ent_id.get("xenon"),
        "default_ttl": 300,
        "hostmaster": "hostmaster.akoria.net.",
        **P})

    # ---- cnames -> CNAME dns_records (functional aliases) ------------------
    for alias in sorted(cnames):
        target = cnames[alias]
        tid = ent_id.get(target)
        if tid is None:
            print(f"  WARN cname {alias}->{target}: target entity missing; skipping", file=sys.stderr)
            continue
        upsert(cur, "dns_records", ["zone_id", "name", "rrtype", "rdata"], {
            "zone_id": zone_id,
            "name": alias,
            "rrtype": "CNAME",
            "rdata": target,
            "target_entity_id": tid,
            "is_generated": 0,
            **P})

    return ent_id


# ---------------------------------------------------------------------------
# 2. Samba AD seeder
# ---------------------------------------------------------------------------

AD_COLLECT = r'''
set -e
# NB: iterate line-by-line (read), not `for x in $(...)`, because AD group names
# contain spaces ("Cert Publishers") which word-splitting would shred.
sudo samba-tool user list | while IFS= read -r u; do
  [ -z "$u" ] && continue
  echo "U|$u"
  sudo samba-tool user show "$u" --attributes=objectGUID,objectSid,userPrincipalName,displayName,mail,userAccountControl 2>/dev/null | sed 's/^/A|/'
  echo "E|"
done
sudo samba-tool computer list | while IFS= read -r c; do
  [ -z "$c" ] && continue
  echo "K|$c"
  sudo samba-tool computer show "$c" --attributes=objectGUID,objectSid 2>/dev/null | sed 's/^/A|/'
  echo "E|"
done
sudo samba-tool group list | while IFS= read -r g; do
  [ -z "$g" ] && continue
  echo "G|$g"
  sudo samba-tool group show "$g" --attributes=objectGUID,objectSid,description 2>/dev/null | sed 's/^/A|/'
  echo "M|"
  sudo samba-tool group listmembers "$g" 2>/dev/null | sed 's/^/m|/'
  echo "E|"
done
'''


def ssh_run(host, script, timeout=240):
    """Run a shell script on `host` by piping it to `bash -s` over stdin, which
    sidesteps all remote-shell requoting of multi-line scripts."""
    ssh_pass = os.environ.get("SSH_PASS")
    if not ssh_pass:
        raise RuntimeError("SSH_PASS not set (needed for AD/forgejo collection)")
    cmd = ["sshpass", "-e", "ssh", "-o", "StrictHostKeyChecking=no",
           f"{SSH_USER}@{host}", "bash -s"]
    e = dict(os.environ, SSHPASS=ssh_pass)
    out = subprocess.run(cmd, input=script, capture_output=True, text=True,
                         env=e, timeout=timeout)
    if out.returncode != 0:
        raise RuntimeError(f"ssh {host} failed rc={out.returncode}: {out.stderr[:400]}")
    return out.stdout


def _parse_ldif_blocks(text):
    """Parse the U|/K|/G|/A|/M|/m|/E| stream into user/computer/group records."""
    users, computers, groups = [], [], []
    cur = None
    kind = None
    members = None
    for line in text.splitlines():
        if line.startswith("U|"):
            cur = {"sam": line[2:].strip(), "attrs": {}}; kind = "user"
        elif line.startswith("K|"):
            cur = {"sam": line[2:].strip(), "attrs": {}}; kind = "computer"
        elif line.startswith("G|"):
            cur = {"sam": line[2:].strip(), "attrs": {}, "members": []}; kind = "group"; members = None
        elif line.startswith("A|"):
            body = line[2:]
            if ":" in body and not body.startswith("dn:"):
                k, _, v = body.partition(":")
                cur["attrs"][k.strip()] = v.strip()
        elif line.startswith("M|"):
            members = cur["members"]
        elif line.startswith("m|"):
            if members is not None:
                members.append(line[2:].strip())
        elif line.startswith("E|"):
            if kind == "user":
                users.append(cur)
            elif kind == "computer":
                computers.append(cur)
            elif kind == "group":
                groups.append(cur)
            cur = None
    return users, computers, groups


SERVICE_USERS = {"krbtgt", "guest", "dns-radium"}


def seed_ad(cur, src):
    sid = src["samba_ad"]
    # Collection runs `sudo samba-tool ...` on radium (passwordless sudo); the
    # AD administrator password is not needed for read-only enumeration.
    text = ssh_run(RADIUM_HOST, AD_COLLECT)
    users, computers, groups = _parse_ldif_blocks(text)

    user_id = {}   # sAMAccountName(lower) -> users.id
    for u in users:
        a = u["attrs"]
        sam = u["sam"]
        kind = "service_account" if sam.lower() in SERVICE_USERS else "person"
        uac = a.get("userAccountControl")
        enabled = 0 if (uac and int(uac) & 0x2) else 1
        guid = a.get("objectGUID")
        row = {
            "username": sam, "realm": "akoria.org", "kind": kind,
            "display_name": a.get("displayName"), "email": a.get("mail"),
            "upn": a.get("userPrincipalName"),
            "ad_guid": guid, "ad_sid": a.get("objectSid"),
            "enabled": enabled, **prov(sid, "machine")}
        uid = upsert(cur, "users", ["username", "realm"], row)
        user_id[sam.lower()] = uid
        if guid:
            ref_set(cur, sid, "users", uid, guid)

    for c in computers:
        a = c["attrs"]
        sam = c["sam"]
        guid = a.get("objectGUID")
        row = {
            "username": sam, "realm": "akoria.org", "kind": "machine_account",
            "display_name": sam, "ad_guid": guid, "ad_sid": a.get("objectSid"),
            "enabled": 1, **prov(sid, "machine")}
        uid = upsert(cur, "users", ["username", "realm"], row)
        user_id[sam.lower()] = uid
        if guid:
            ref_set(cur, sid, "users", uid, guid)

    group_id = {}  # name(lower) -> groups.id
    for g in groups:
        a = g["attrs"]
        guid = a.get("objectGUID")
        row = {
            "name": g["sam"], "realm": "akoria.org", "kind": "security",
            "ad_guid": guid, "ad_sid": a.get("objectSid"),
            "description": a.get("description"), **prov(sid, "machine")}
        gid = upsert(cur, "groups", ["name", "realm"], row)
        group_id[g["sam"].lower()] = gid
        if guid:
            ref_set(cur, sid, "groups", gid, guid)

    # memberships (users + nested groups)
    for g in groups:
        gid = group_id[g["sam"].lower()]
        for m in g.get("members", []):
            ml = m.lower()
            if ml in user_id:
                upsert(cur, "group_members", ["group_id", "member_user_id"], {
                    "group_id": gid, "member_user_id": user_id[ml],
                    **prov(sid, "machine")})
            elif ml in group_id:
                upsert(cur, "group_members", ["group_id", "member_group_id"], {
                    "group_id": gid, "member_group_id": group_id[ml],
                    **prov(sid, "machine")})

    # akoria.org zone (external; radium is authoritative)
    cur.execute("SELECT id FROM entities WHERE name='radium' AND deleted_at IS NULL LIMIT 1")
    r = cur.fetchone()
    upsert(cur, "dns_zones", ["name"], {
        "name": "akoria.org", "kind": "forward", "authority": "external",
        "primary_ns_entity_id": r[0] if r else None,
        "default_ttl": 300, "hostmaster": "hostmaster.akoria.org.",
        "notes": "authoritative on Samba AD (radium); mirrored, not rendered",
        **prov(sid, "machine")})

    return {"users": len(users), "computers": len(computers), "groups": len(groups)}


# ---------------------------------------------------------------------------
# 3. Forgejo seeder
# ---------------------------------------------------------------------------

def seed_forgejo(cur, src):
    sid = src["forgejo"]
    # Forgejo API is reachable on cesium :3000; QuakeKit is public so no token
    # is needed. (Private repos would require a read:repository token.)
    remote = ("curl -s -m 8 "
              "'http://localhost:3000/api/v1/repos/search?limit=50&private=true' "
              "-H 'Authorization: token '\"${FORGEJO_TOKEN:-}\"")
    out = ssh_run(CESIUM_HOST, remote)
    data = json.loads(out).get("data", [])

    # the forge itself is an entity (Forgejo application running on cesium)
    forge_id = upsert(cur, "entities", ["name"], {
        "name": "forgejo",
        "entity_type": "application",
        "role": "Forgejo git forge (git.akoria.net)",
        "lifecycle": "active",
        **prov(sid, "machine")})
    upsert(cur, "applications", ["entity_id"], {
        "entity_id": forge_id, "vendor": "Forgejo",
        "upstream_url": "https://forgejo.org", "install_kind": "package",
        **prov(sid, "machine")}, id_col="entity_id")
    cur.execute("SELECT id FROM entities WHERE name='cesium' AND deleted_at IS NULL LIMIT 1")
    ces = cur.fetchone()
    if ces:
        upsert(cur, "relationships", ["from_entity_id", "to_entity_id", "rel_type"], {
            "from_entity_id": forge_id, "to_entity_id": ces[0],
            "rel_type": "runs_on", **prov(sid, "machine")})

    repos = 0
    for r in data:
        owner = r["owner"]["login"]
        # correlate the Forgejo account to a directory user by username
        cur.execute("SELECT id FROM users WHERE LOWER(username)=%s AND deleted_at IS NULL LIMIT 1",
                    (owner.lower(),))
        u = cur.fetchone()
        rid = upsert_by_ref(cur, sid, "repositories", str(r["id"]),
                            ["owner_name", "name"], {
            "owner_name": owner, "name": r["name"],
            "owner_user_id": u[0] if u else None,
            "forge_entity_id": forge_id,
            "clone_url": r.get("clone_url"),
            "default_branch": r.get("default_branch"),
            "visibility": "private" if r.get("private") else "public",
            "is_archived": 1 if r.get("archived") else 0,
            "is_mirror": 1 if r.get("mirror") else 0,
            "description": (r.get("description") or "")[:255],
            **prov(sid, "machine")},
            url=r.get("html_url"))
        repos += 1
    return {"repos": repos}


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--netdb", action="store_true")
    ap.add_argument("--ad", action="store_true")
    ap.add_argument("--forgejo", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    do_netdb = args.netdb or args.all or not (args.ad or args.forgejo)
    do_ad = args.ad or args.all
    do_forgejo = args.forgejo or args.all

    cn = connect()
    summary = {}
    try:
        with cn.cursor() as cur:
            src = source_ids(cur)
            if do_netdb:
                seed_netdb(cur, src)
            if do_ad:
                try:
                    summary["ad"] = seed_ad(cur, src)
                except Exception as ex:
                    summary["ad_error"] = str(ex)
                    print(f"AD seeding failed (continuing): {ex}", file=sys.stderr)
            if do_forgejo:
                try:
                    summary["forgejo"] = seed_forgejo(cur, src)
                except Exception as ex:
                    summary["forgejo_error"] = str(ex)
                    print(f"Forgejo seeding failed (continuing): {ex}", file=sys.stderr)
        cn.commit()
    except Exception:
        cn.rollback()
        raise
    finally:
        cn.close()

    print("\n=== seed complete ===")
    for table in sorted(STATS):
        s = STATS[table]
        print(f"  {table:<20} +{s['insert']:<4} ins   ~{s['update']:<4} upd")
    if summary:
        print("  extras:", json.dumps(summary))


if __name__ == "__main__":
    main()
