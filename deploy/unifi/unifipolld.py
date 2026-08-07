#!/usr/bin/env python3
"""Fail-soft UniFi Integration API poller for the panel A1 gear feed."""
import argparse
import json
import math
import os
import ssl
import sys
import time
from pathlib import Path
from urllib.error import URLError, HTTPError
from urllib.request import Request, urlopen

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
    def __init__(self, gateway, api_key, cafile=None):
        self.gateway = gateway.rstrip("/")
        self.api_key = api_key
        # nosec: self-signed home gateway, see CONTRACT-AW.md §2
        self.context = ssl.create_default_context(cafile=cafile) if cafile else ssl._create_unverified_context()

    def get(self, path):
        request = Request(self.gateway + path, headers={"X-API-KEY": self.api_key})
        with urlopen(request, context=self.context, timeout=12) as response:  # nosec: gateway is locally administered
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
        return
    with db_connect(database) as db, db.cursor() as cur:
        for row in rows:
            cur.execute("""INSERT INTO networkGear (gearId,name,kind,model,mgmtIp,lastSeenAt)
                VALUES (%(gearId)s,%(name)s,%(kind)s,%(model)s,%(mgmtIp)s,UTC_TIMESTAMP())
                ON DUPLICATE KEY UPDATE name=VALUES(name),kind=VALUES(kind),model=VALUES(model),mgmtIp=VALUES(mgmtIp),lastSeenAt=UTC_TIMESTAMP()""", row)
            cur.execute("""INSERT INTO gearInterfaceCurrent (gearId,ifIndex,ifName,inRateKbps,outRateKbps,operStatus,sampledAt)
                VALUES (%(gearId)s,0,'aggregate',%(inRateKbps)s,%(outRateKbps)s,%(operStatus)s,UTC_TIMESTAMP())
                ON DUPLICATE KEY UPDATE ifName=VALUES(ifName),inRateKbps=VALUES(inRateKbps),outRateKbps=VALUES(outRateKbps),operStatus=VALUES(operStatus),sampledAt=UTC_TIMESTAMP()""", row)


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
    except Exception:  # noqa: BLE001 — review S5: an unexpected API shape
        # raised TypeError, escaped the tuple, and wedged the unit through
        # StartLimitBurst. ANY per-cycle failure is a skipped cycle.
        # Exception details can carry request URLs but never the API-key header.
        log("poll failed; skipping cycle")
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
    client = UniFiClient(gateway, key, cafile)
    while True:
        poll_cycle(client, args.database, args.state, args.dry_run)
        if args.once:
            return 0
        time.sleep(15)


if __name__ == "__main__":
    sys.exit(main())
