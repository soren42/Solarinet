#!/usr/bin/env python3
"""Fail-soft UniFi Integration API poller for the panel A1 gear feed."""
import argparse
import http.client
import ipaddress
import json
import math
import os
import socket
import ssl
import sys
import time
from pathlib import Path
from urllib.error import URLError, HTTPError
from urllib.request import HTTPSHandler, ProxyHandler, Request, build_opener, urlopen

try:
    import pymysql
except ImportError:  # Dry-run and API diagnostics do not require a DB client.
    pymysql = None

def _die(msg):
    # One clear line, nonzero exit — systemd Restart sees an honest config
    # error instead of a stack trace storm (review S5).
    print(f"unifipolld: fatal: {msg}", flush=True)
    raise SystemExit(2)


HERE = Path(__file__).resolve().parent


def log(message):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} unifipolld: {message}", flush=True)


def model_kind(model):
    """Map the API model string to the CONTRACT-AW §2 networkGear enum."""
    value = (model or "").lower()
    # Backup must precede generic U5G AP matching.
    if "u5g backup" in value:
        return "wanBackup"
    if value.startswith("udm"):
        return "gateway"
    if "usw pro" in value:
        return "switch"
    if any(token in value for token in ("usw ultra", "usw flex", "usw lite")):
        return "hub"
    if any(token in value for token in ("u6", "u7", "u5g")):
        return "ap"
    return "other"


# How long a motionless counter keeps its last rate before the port counts as idle.
COUNTER_HOLD_SECONDS = 300


def derive_rate_kbps(previous, current, elapsed):
    """Match SNMP convention: octet delta * 8 / 1000 / elapsed seconds."""
    if previous is None or current is None or elapsed <= 0 or current < previous:
        return 0
    return int((current - previous) * 8 / 1000 / elapsed)


def load_state(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return {}


def save_state(path, state):
    temporary = f"{path}.tmp"
    try:
        with open(temporary, "w") as out:
            json.dump(state, out)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    except OSError as error:
        log(f"WARNING: cannot save rate state: {error}")


class UniFiClient:
    def __init__(self, gateway, api_key, cafile=None, connect_ip=None):
        self.gateway = gateway.rstrip("/")
        self.api_key = api_key
        # nosec: self-signed home gateway, see CONTRACT-AW.md §2
        self.context = ssl.create_default_context(cafile=cafile) if cafile else ssl._create_unverified_context()
        self.opener = None
        if connect_ip:
            try:
                self.connect_ip = str(ipaddress.ip_address(connect_ip))
            except ValueError as error:
                raise ValueError("UNIFI_CONNECT_IP must be a valid IP address") from error
            self.opener = build_opener(
                ProxyHandler({}),
                _ConnectIPHTTPSHandler(self.connect_ip, context=self.context),
            )

    def get(self, path):
        request = Request(self.gateway + path, headers={"X-API-KEY": self.api_key})
        if self.opener is not None:
            response = self.opener.open(request, timeout=12)
        else:
            response = urlopen(request, context=self.context, timeout=12)  # nosec: gateway is locally administered
        with response:
            return json.load(response)

    def device_statistics(self, site_id, device_id):
        """Best-effort per-device statistics/latest fetch; None on any failure.

        One device's stats endpoint erroring (e.g. a device that doesn't
        support it) must not drop that device's other fields nor abort the
        cycle — fail-soft per device, not just per cycle.
        """
        try:
            return self.get(f"/proxy/network/integration/v1/sites/{site_id}/devices/{device_id}/statistics/latest")
        except (HTTPError, URLError, OSError, ValueError):
            return None


class _ConnectIPHTTPSConnection(http.client.HTTPSConnection):
    """Connect to an IP while keeping the URL hostname for SNI and TLS checks."""

    def __init__(self, host, connect_ip, **kwargs):
        self.connect_ip = connect_ip
        super().__init__(host, **kwargs)

    def connect(self):
        if self._tunnel_host:
            raise OSError("UNIFI_CONNECT_IP cannot be combined with an HTTPS proxy tunnel")
        sock = socket.create_connection((self.connect_ip, self.port), self.timeout, self.source_address)
        try:
            self.sock = self._context.wrap_socket(sock, server_hostname=self.host)
        except Exception:
            sock.close()
            raise


class _ConnectIPHTTPSHandler(HTTPSHandler):
    def __init__(self, connect_ip, context):
        super().__init__(context=context)
        self.connect_ip = connect_ip

    def https_open(self, request):
        def connection(host, **kwargs):
            return _ConnectIPHTTPSConnection(host, connect_ip=self.connect_ip, **kwargs)

        return self.do_open(connection, request, context=self._context)


def data_items(value):
    if isinstance(value, list):
        return value
    if isinstance(value, dict):
        for key in ("data", "devices", "items", "results"):
            if isinstance(value.get(key), list):
                return value[key]
    return []


def pick(value, *names, default=None):
    if not isinstance(value, dict):
        return default
    for name in names:
        if value.get(name) is not None:
            return value[name]
    return default


def numeric(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def device_stats(item):
    """Return aggregate rx/tx Kbps or counters from common Integration shapes.

    Live-verified 2026-08-07 against the real gateway: the /devices list item
    itself carries no statistics; the per-device GET .../statistics/latest
    endpoint returns {"uplink": {"rxRateBps": N, "txRateBps": N}, ...} in
    BITS/sec despite the "Bps" suffix (UniFi's own naming) — normalise_devices
    merges that response into item["statistics"] before calling this.
    """
    stats = pick(item, "statistics", "stats", default=item)
    if not isinstance(stats, dict):
        stats = item
    rx_rate = numeric(pick(stats, "rxKbps", "rx_kbps", "rxRateKbps", "downloadKbps"))
    tx_rate = numeric(pick(stats, "txKbps", "tx_kbps", "txRateKbps", "uploadKbps"))
    if rx_rate is not None or tx_rate is not None:
        return int(rx_rate or 0), int(tx_rate or 0), None, None
    uplink = pick(stats, "uplink", default={})
    rx_bps = numeric(pick(uplink, "rxRateBps"))
    tx_bps = numeric(pick(uplink, "txRateBps"))
    if rx_bps is not None or tx_bps is not None:
        return int((rx_bps or 0) / 1000), int((tx_bps or 0) / 1000), None, None
    rx = numeric(pick(stats, "rxBytes", "rxOctets", "bytesRx", "receivedBytes"))
    tx = numeric(pick(stats, "txBytes", "txOctets", "bytesTx", "transmittedBytes"))
    return None, None, int(rx) if rx is not None else None, int(tx) if tx is not None else None


def device_online(item):
    """True unless the device explicitly reports itself offline.

    Live-verified 2026-08-07: the real API's /devices list item carries
    state:"ONLINE"/"OFFLINE" (a string), not the online/isOnline/connected
    booleans this originally checked — those never matched, so operStatus
    silently defaulted to "up" for every device regardless of true state.
    """
    explicit = pick(item, "online", "isOnline", "connected")
    if explicit is not None:
        return bool(explicit)
    state_text = pick(item, "state", "status")
    if isinstance(state_text, str):
        return state_text.strip().upper() not in ("OFFLINE", "DOWN", "DISCONNECTED")
    return True


def rate_bytes_per_second_kbps(value):
    """Convert classic UniFi byte/s rates to decimal kilobits/s once."""
    rate = numeric(value)
    return max(0, int((rate or 0) * 8 / 1000))


def rate_bits_per_second_kbps(value):
    """Convert classic UniFi gateway bit/s rates to decimal kilobits/s."""
    rate = numeric(value)
    return max(0, int((rate or 0) / 1000))


def classic_device_rows(devices):
    """Normalize classic stat/device rows, retaining actual port topology."""
    rows = {}
    mac_to_id = {}
    for item in devices:
        mac = str(pick(item, "mac", "macAddress", default="")).lower()
        if not mac:
            continue
        gear_id = f"unifi-{mac}"
        model = str(pick(item, "model", "modelName", default=""))
        ports = []
        for port in item.get("port_table", []) if isinstance(item.get("port_table"), list) else []:
            if not isinstance(port, dict) or numeric(port.get("port_idx")) is None or int(port["port_idx"]) <= 0:
                continue
            rx = pick(port, "rx_bytes-r", "rx_bytes_r", "rxRate"); tx = pick(port, "tx_bytes-r", "tx_bytes_r", "txRate")
            ports.append({
                "ifIndex": max(0, int(port["port_idx"])),
                "ifName": str(pick(port, "name", "port_name", default=f"port-{port['port_idx']}")),
                "inRateKbps": rate_bytes_per_second_kbps(rx),
                "outRateKbps": rate_bytes_per_second_kbps(tx),
                "inOctets": max(0, int(numeric(pick(port, "rx_bytes", "rxBytes", default=0)) or 0)),
                "outOctets": max(0, int(numeric(pick(port, "tx_bytes", "txBytes", default=0)) or 0)),
                "operStatus": 1 if bool(pick(port, "up", "is_up", default=False)) else 2,
                "isUplink": 1 if bool(pick(port, "is_uplink", "isUplink", default=False)) else 0,
            })
        uplink = pick(item, "uplink", default={})
        if not isinstance(uplink, dict):
            uplink = {}
        # Some switches never flag is_uplink; their uplink dict still names the port.
        if not any(port["isUplink"] for port in ports):
            own_port = numeric(pick(uplink, "port_idx", "portIdx"))
            for port in ports:
                if port["ifIndex"] == own_port:
                    port["isUplink"] = 1
        rows[gear_id] = {
            "gearId": gear_id,
            "macAddress": mac,
            "parentMac": str(pick(uplink, "uplink_mac", "uplinkMac", default="")).lower() or None,
            "uplinkPort": pick(uplink, "port_idx", "portIdx"),
            "uplinkRemotePort": numeric(pick(uplink, "uplink_remote_port", "uplinkRemotePort")),
            "ports": ports,
            "wanInRateKbps": rate_bits_per_second_kbps(pick(uplink, "rx_rate", "rxRate")),
            "wanOutRateKbps": rate_bits_per_second_kbps(pick(uplink, "tx_rate", "txRate")),
            "isGateway": model_kind(model) == "gateway",
        }
        mac_to_id[mac] = gear_id

    for row in rows.values():
        parent_mac = row["parentMac"]
        row["uplinkGearId"] = mac_to_id.get(parent_mac) if parent_mac else None
    # Break each directed cycle at the gateway when present (the WAN is the
    # panel graph root); otherwise cut a deterministic member. Retain the
    # remaining MAC-derived parents rather than dropping the entire loop.
    for start in rows:
        path = []
        positions = {}
        current = start
        while current in rows and rows[current]["uplinkGearId"]:
            if current in positions:
                cycle = path[positions[current]:]
                gateways = [member for member in cycle if rows[member]["isGateway"]]
                cut = gateways[0] if gateways else max(cycle)
                rows[cut]["uplinkGearId"] = None
                break
            positions[current] = len(path)
            path.append(current)
            current = rows[current]["uplinkGearId"]
    return rows


def counter_rate(last, octets, now):
    """Rate over the last two counter moves, held until the next move.

    Two, because a USPM16 flushes its counters on alternate reports: a single gap
    reads full traffic, then nothing. Returns the new state entry; its "rate" stays
    None until three moves have been seen, and a counter reset starts over.
    """
    if not last or octets < last["octets"]:
        return {"octets": octets, "moves": [], "rate": None}
    if octets != last["octets"]:
        moves = (last["moves"] + [[now, octets]])[-3:]
        rate = derive_rate_kbps(moves[0][1], octets, now - moves[0][0]) if len(moves) == 3 else None
        return {"octets": octets, "moves": moves, "rate": rate}
    if last["moves"] and now - last["moves"][-1][0] > COUNTER_HOLD_SECONDS:
        return {**last, "rate": 0}
    return last


def rates_from_counters(rows, state, now):
    """Replace reported port rates with counter deltas wherever enough moves have been seen.

    Reported rates are instantaneous snapshots: over one minute a WAN port carrying a
    steady 5 Mbps by its counters reported 22 kbps, 25 Mbps, then 12 kbps. Counters only
    move every 15 s on a gateway and about every 48 s on a USPM16, so a per-poll delta
    would read zero between moves.
    """
    prior = state.setdefault("ports", {})
    for row in rows:
        for port in row.get("ports", []):
            key = f"{row['gearId']}/{port['ifIndex']}"
            last = prior.get(key) or {}
            entry = {"in": counter_rate(last.get("in"), port["inOctets"], now),
                     "out": counter_rate(last.get("out"), port["outOctets"], now)}
            prior[key] = entry
            if entry["in"]["rate"] is not None:
                port["inRateKbps"] = entry["in"]["rate"]
            if entry["out"]["rate"] is not None:
                port["outRateKbps"] = entry["out"]["rate"]


def attach_uplinks_from_parent_ports(rows):
    """Access points have no port table, so read their link from the parent's end."""
    by_id = {row["gearId"]: row for row in rows}
    for row in rows:
        parent = by_id.get(row.get("uplinkGearId"))
        if row.get("ports") or not parent:
            continue
        for port in parent.get("ports", []):
            if port["ifIndex"] == row.get("uplinkRemotePort"):
                row["ports"] = [{
                    "ifIndex": 1, "ifName": "uplink", "isUplink": 1, "operStatus": port["operStatus"],
                    "inRateKbps": port["outRateKbps"], "outRateKbps": port["inRateKbps"],
                    "inOctets": port["outOctets"], "outOctets": port["inOctets"],
                }]
                break


def aggregates_from_uplinks(rows):
    """A device's aggregate is its uplink: the Integration API reports zero for most gear."""
    for row in rows:
        uplink = next((port for port in row.get("ports", []) if port["isUplink"]), None)
        if uplink:
            row["inRateKbps"], row["outRateKbps"] = uplink["inRateKbps"], uplink["outRateKbps"]


def normalise_devices(devices, state, now, stats_fetcher=None):
    rows = []
    prior = state.setdefault("devices", {})
    for item in devices:
        device_id = str(pick(item, "mac", "macAddress", "id", "deviceId", default="")).lower()
        if not device_id:
            continue
        model = str(pick(item, "model", "modelName", "shortname", default=""))
        if stats_fetcher is not None and "statistics" not in item and "stats" not in item:
            fetched = stats_fetcher(item)
            if fetched is not None:
                item = {**item, "statistics": fetched}
        rx, tx, rx_octets, tx_octets = device_stats(item)
        old = prior.get(device_id, {})
        elapsed = now - float(old.get("ts", now))
        if rx is None:
            rx = derive_rate_kbps(old.get("rxOctets"), rx_octets, elapsed)
        if tx is None:
            tx = derive_rate_kbps(old.get("txOctets"), tx_octets, elapsed)
        prior[device_id] = {"ts": now, "rxOctets": rx_octets, "txOctets": tx_octets}
        rows.append({
            "gearId": f"unifi-{device_id}", "name": str(pick(item, "name", "displayName", "hostname", default=device_id)),
            "kind": model_kind(model), "model": model, "mgmtIp": pick(item, "ip", "ipAddress", "mgmtIp"),
            "inRateKbps": max(0, int(rx)), "outRateKbps": max(0, int(tx)),
            "operStatus": 1 if device_online(item) else 2,
        })
    return rows


def db_connect(database):
    if pymysql is None:
        raise RuntimeError("pymysql is required for database writes")
    return pymysql.connect(host=os.environ.get("SOLARI_DB_HOST", "127.0.0.1"), port=int(os.environ.get("SOLARI_DB_PORT", "3306")),
        user=os.environ.get("SOLARI_DB_USER", "solari"),
        # Review S5: a KeyError here escaped every fail-soft catch and
        # crash-looped the unit into StartLimitBurst permanent failure.
        password=os.environ.get("SOLARI_DB_PASS") or _die("SOLARI_DB_PASS unset — check run/db.env in the unit"), database=database,
        autocommit=True, cursorclass=pymysql.cursors.DictCursor)


def write_rows(rows, database, dry_run):
    if dry_run:
        for row in rows:
            print(f"DRY-RUN networkGear {row['gearId']} kind={row['kind']} name={row['name']}")
            print(f"DRY-RUN gearInterfaceCurrent {row['gearId']} ifIndex=0 in={row['inRateKbps']} out={row['outRateKbps']}")
            for port in row.get("ports", []):
                print(f"DRY-RUN gearInterfaceCurrent {row['gearId']} ifIndex={port['ifIndex']} uplink={port['isUplink']}")
        return
    with db_connect(database) as db, db.cursor() as cur:
        for row in rows:
            # pymysql escapes every value it is handed, used or not, and a port list cannot be escaped.
            scalars = {key: value for key, value in row.items() if key != "ports"}
            cur.execute("""INSERT INTO networkGear
                (gearId,name,kind,model,mgmtIp,macAddress,uplinkGearId,ports,lastSeenAt)
                VALUES (%(gearId)s,%(name)s,%(kind)s,%(model)s,%(mgmtIp)s,%(macAddress)s,%(uplinkGearId)s,%(portCount)s,UTC_TIMESTAMP())
                ON DUPLICATE KEY UPDATE name=VALUES(name),kind=VALUES(kind),model=VALUES(model),mgmtIp=VALUES(mgmtIp),
                macAddress=IF(%(classicSeen)s=1,VALUES(macAddress),macAddress),
                uplinkGearId=IF(%(classicSeen)s=1,VALUES(uplinkGearId),uplinkGearId),
                ports=COALESCE(VALUES(ports),ports),lastSeenAt=UTC_TIMESTAMP()""", scalars)
            cur.execute("""INSERT INTO gearInterfaceCurrent (gearId,ifIndex,ifName,inRateKbps,outRateKbps,operStatus,sampledAt)
                VALUES (%(gearId)s,0,'aggregate',%(inRateKbps)s,%(outRateKbps)s,%(operStatus)s,UTC_TIMESTAMP())
                ON DUPLICATE KEY UPDATE ifName=VALUES(ifName),inRateKbps=VALUES(inRateKbps),outRateKbps=VALUES(outRateKbps),operStatus=VALUES(operStatus),sampledAt=UTC_TIMESTAMP()""", scalars)
            for port in row.get("ports", []):
                port_row = {**port, "gearId": row["gearId"]}
                cur.execute("""INSERT INTO gearInterfaceCurrent
                    (gearId,ifIndex,ifName,inOctets,outOctets,inRateKbps,outRateKbps,operStatus,isUplink,sampledAt)
                    VALUES (%(gearId)s,%(ifIndex)s,%(ifName)s,%(inOctets)s,%(outOctets)s,%(inRateKbps)s,%(outRateKbps)s,%(operStatus)s,%(isUplink)s,UTC_TIMESTAMP())
                    ON DUPLICATE KEY UPDATE ifName=VALUES(ifName),inOctets=VALUES(inOctets),outOctets=VALUES(outOctets),
                    inRateKbps=VALUES(inRateKbps),outRateKbps=VALUES(outRateKbps),operStatus=VALUES(operStatus),
                    isUplink=VALUES(isUplink),sampledAt=UTC_TIMESTAMP()""", port_row)


def poll_once(client, database, state_path, dry_run=False):
    state = load_state(state_path)
    sites = data_items(client.get("/proxy/network/integration/v1/sites"))
    if not sites:
        raise RuntimeError("UniFi API returned no sites")
    site_id = pick(sites[0], "id", "siteId")
    if not site_id:
        raise RuntimeError("UniFi site has no id")
    devices = data_items(client.get(f"/proxy/network/integration/v1/sites/{site_id}/devices"))

    def fetch_stats(item):
        device_id = pick(item, "id", "deviceId")
        return client.device_statistics(site_id, device_id) if device_id else None

    rows = normalise_devices(devices, state, time.time(), stats_fetcher=fetch_stats)
    # The classic endpoint enriches rates/topology, but its failure must not
    # interfere with the existing Integration API path.
    try:
        classic = data_items(client.get("/proxy/network/api/s/default/stat/device"))
        by_id = {row["gearId"]: row for row in rows}
        enriched = classic_device_rows(classic)
        for gear_id, detail in enriched.items():
            if gear_id in by_id:
                by_id[gear_id].update(detail)
                by_id[gear_id]["classicSeen"] = 1
                if detail["isGateway"]:
                    by_id[gear_id]["inRateKbps"] = detail["wanInRateKbps"]
                    by_id[gear_id]["outRateKbps"] = detail["wanOutRateKbps"]
            else:
                item = next((d for d in classic if f"unifi-{str(pick(d, 'mac', 'macAddress', default='')).lower()}" == gear_id), {})
                by_id[gear_id] = {
                    **detail, "name": str(pick(item, "name", "hostname", default=gear_id)),
                    "kind": model_kind(str(pick(item, "model", "modelName", default=""))),
                    "model": str(pick(item, "model", "modelName", default="")), "mgmtIp": pick(item, "ip", "ipAddress"),
                    "inRateKbps": detail["wanInRateKbps"], "outRateKbps": detail["wanOutRateKbps"],
                    "operStatus": 1 if device_online(item) else 2,
                    "classicSeen": 1,
                }
        rows = list(by_id.values())
    except Exception as exc:  # noqa: BLE001 — optional classic endpoint is fail-soft.
        log(f"classic device enrichment skipped: {type(exc).__name__}")
    for row in rows:
        row.setdefault("macAddress", None)
        row.setdefault("uplinkGearId", None)
        row.setdefault("ports", [])
        row.setdefault("classicSeen", 0)
        row["portCount"] = len(row["ports"]) or None
    rates_from_counters(rows, state, time.time())
    attach_uplinks_from_parent_ports(rows)
    aggregates_from_uplinks(rows)
    write_rows(rows, database, dry_run)
    save_state(state_path, state)
    log(f"polled {len(rows)} UniFi devices" + (" (dry run)" if dry_run else ""))
    return rows


def poll_cycle(client, database, state_path, dry_run=False):
    """One fail-soft iteration; an API/DB failure only skips this cadence."""
    try:
        poll_once(client, database, state_path, dry_run)
        return True
    except SystemExit:
        raise                       # config errors (_die) must stop the unit
    except Exception as exc:  # noqa: BLE001 — review S5: an unexpected API shape
        # raised TypeError, escaped the tuple, and wedged the unit through
        # StartLimitBurst. ANY per-cycle failure is a skipped cycle.
        # Log the type only: exception details can carry request URLs.
        log(f"poll failed ({type(exc).__name__}); skipping cycle")
        return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--database", default=os.environ.get("UNIFI_DB_NAME", "solarinet"))
    parser.add_argument("--state", default=os.environ.get("UNIFI_STATE", str(HERE / "unifipolld.state.json")))
    args = parser.parse_args()
    gateway, key = os.environ.get("UNIFI_GW"), os.environ.get("UNIFI_API_KEY")
    if not gateway or not key:
        log("FATAL: UNIFI_GW and UNIFI_API_KEY are required")
        return 2
    # run/pki/ca.pem is SolariNet's OWN internal CA (dashboard/SCP certs) —
    # it does NOT sign the UniFi gateway's self-signed cert, so it must never
    # be auto-guessed here (confirmed live: auto-guessing it made every poll
    # fail SSL verification). Only an operator-supplied UNIFI_CA_FILE (e.g.
    # a cert pulled from the gateway itself) enables verification; otherwise
    # this intentionally falls back to skip-verify (nosec, see UniFiClient).
    cafile = os.environ.get("UNIFI_CA_FILE")
    client = UniFiClient(gateway, key, cafile, connect_ip=os.environ.get("UNIFI_CONNECT_IP"))
    while True:
        poll_cycle(client, args.database, args.state, args.dry_run)
        if args.once:
            return 0
        time.sleep(15)


if __name__ == "__main__":
    sys.exit(main())
