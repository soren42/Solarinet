#!/usr/bin/env python3
"""Reconcile retained and externally archived leaf certificates into F-01 inventory.

The default mode is read-only. Applying the inventory requires both --apply and
--confirm-complete; operators must stop certificate issuance for the duration
and attest that the supplied archive contains every out-of-band SIGN result.
The tool never reads or requests a CA private key.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass

NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
PEM_RE = re.compile(rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", re.S)


@dataclass(frozen=True)
class Issuance:
    issuer_fp: str
    serial_hex: str
    leaf_fp: str
    node_id: int | None
    enrollment_id: int | None
    origin: str
    issued_by: str
    issued_at_ms: int | None

    @property
    def identity(self) -> tuple[str, str]:
        return self.issuer_fp, self.serial_hex


def run(command: list[str], *, data: bytes | None = None) -> bytes:
    result = subprocess.run(command, input=data, capture_output=True)
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()[:400]
        raise ValueError(f"{pathlib.Path(command[0]).name} failed: {detail}")
    return result.stdout


def certificates(pem: bytes) -> list[bytes]:
    found = PEM_RE.findall(pem)
    if not found:
        raise ValueError("input contains no PEM certificate")
    residue = PEM_RE.sub(b"", pem)
    if residue.strip():
        raise ValueError("input contains data outside PEM certificates")
    return found


def der_and_serial(cert: bytes) -> tuple[bytes, str]:
    der = run(["openssl", "x509", "-inform", "PEM", "-outform", "DER"], data=cert)
    serial_line = run(["openssl", "x509", "-inform", "PEM", "-noout", "-serial"], data=cert)
    match = re.fullmatch(rb"serial=([0-9A-Fa-f]+)\s*", serial_line)
    if not match:
        raise ValueError("OpenSSL returned an invalid certificate serial")
    serial = match.group(1).decode("ascii").upper().lstrip("0") or "0"
    if len(serial) > 40:
        raise ValueError("certificate serial exceeds inventory column limit")
    return der, serial


def not_before_ms(cert: bytes) -> int:
    value = run(["openssl", "x509", "-inform", "PEM", "-noout", "-startdate"], data=cert)
    match = re.fullmatch(rb"notBefore=(.+)\s*", value)
    if not match:
        raise ValueError("OpenSSL returned an invalid notBefore date")
    parsed = datetime.datetime.strptime(match.group(1).decode("ascii"), "%b %d %H:%M:%S %Y %Z")
    return int(parsed.replace(tzinfo=datetime.timezone.utc).timestamp() * 1000)


def ca_fingerprints(bundle: bytes) -> dict[str, bytes]:
    result: dict[str, bytes] = {}
    for ca in certificates(bundle):
        der = run(["openssl", "x509", "-inform", "PEM", "-outform", "DER"], data=ca)
        result[hashlib.sha256(der).hexdigest()] = ca
    return result


def inspect_leaf(pem: bytes, cas: dict[str, bytes]) -> tuple[str, str, str, int]:
    leaf_certs = certificates(pem)
    if len(leaf_certs) != 1:
        raise ValueError("an issuance entry must contain exactly one leaf certificate")
    leaf = leaf_certs[0]
    der, serial = der_and_serial(leaf)
    leaf_fp = hashlib.sha256(der).hexdigest()
    matches = []
    # Ignore expiry/not-yet-valid time when identifying a historical issuer;
    # signature and chain validation still have to succeed.
    with tempfile.TemporaryDirectory(prefix="solari-cert-backfill-") as directory:
        ca_path = pathlib.Path(directory) / "issuer.pem"
        leaf_path = pathlib.Path(directory) / "leaf.pem"
        leaf_path.write_bytes(leaf)
        for issuer_fp, issuer in cas.items():
            ca_path.write_bytes(issuer)
            try:
                run(["openssl", "verify", "-no_check_time", "-partial_chain",
                     "-CAfile", str(ca_path), str(leaf_path)])
                matches.append(issuer_fp)
            except ValueError:
                continue
    if len(matches) != 1:
        raise ValueError(f"leaf {leaf_fp} has {len(matches)} matching configured issuers; expected one")
    return matches[0], serial, leaf_fp, not_before_ms(leaf)


def mysql(args: argparse.Namespace, sql: str) -> list[list[str]]:
    command = ["mariadb"]
    if args.defaults_file:
        command.append("--defaults-extra-file=" + str(args.defaults_file))
    command += ["--batch", "--raw", "--skip-column-names", "--default-character-set=utf8mb4"]
    if args.host:
        command.append("--host=" + args.host)
    if args.port:
        command.append("--port=" + str(args.port))
    if args.user:
        command.append("--user=" + args.user)
    command.append("--database=" + args.database)
    output = run(command, data=sql.encode("utf-8")).decode("utf-8")
    return [line.split("\t") for line in output.splitlines() if line]


def sql_string(value: str) -> str:
    raw = value.encode("utf-8")
    return "CONVERT(0x" + raw.hex() + " USING utf8mb4)" if raw else "CONVERT(X'' USING utf8mb4)"


def read_inputs(args: argparse.Namespace, ca_bundle: bytes) -> list[Issuance]:
    cas = ca_fingerprints(ca_bundle)
    rows = mysql(args,
        "SELECT enrId,nodeId,CAST(UNIX_TIMESTAMP(decidedAt)*1000 AS UNSIGNED),"
        "COALESCE(decidedBy,''),HEX(issuedCertPem) FROM enrollment "
        "WHERE issuedCertPem IS NOT NULL "
        "ORDER BY enrId")
    records: list[Issuance] = []
    for enr_id, node_id, issued_at, issued_by, pem_hex in rows:
        if not node_id:
            raise ValueError(f"enrollment {enr_id} retains an issued certificate without a node association")
        try:
            pem = bytes.fromhex(pem_hex)
        except ValueError as error:
            raise ValueError(f"enrollment {enr_id} has malformed stored certificate bytes") from error
        issuer, serial, fp, cert_start_ms = inspect_leaf(pem, cas)
        if not issued_at:
            issued_at = cert_start_ms
        records.append(Issuance(issuer, serial, fp, int(node_id), int(enr_id),
                                "enrollment", issued_by or "historical-backfill",
                                int(issued_at or 0)))

    if args.external_manifest:
        manifest_path = pathlib.Path(args.external_manifest).expanduser().resolve()
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if not isinstance(manifest, list):
            raise ValueError("external manifest must be a JSON array")
        for index, item in enumerate(manifest):
            if not isinstance(item, dict) or set(item) - {"certificate", "nodeId", "issuedBy", "issuedAtMs"}:
                raise ValueError(f"external manifest entry {index + 1} has invalid fields")
            cert_name = item.get("certificate")
            if not isinstance(cert_name, str) or not cert_name:
                raise ValueError(f"external manifest entry {index + 1} requires a certificate path")
            cert_path = pathlib.Path(cert_name)
            if not cert_path.is_absolute():
                cert_path = manifest_path.parent / cert_path
            pem = cert_path.read_bytes()
            issuer, serial, fp, cert_start_ms = inspect_leaf(pem, cas)
            node_id = item.get("nodeId")
            issued_at = item.get("issuedAtMs", cert_start_ms)
            issued_by = item.get("issuedBy", "external-archive")
            if node_id is not None and (isinstance(node_id, bool) or
                                        not isinstance(node_id, int) or node_id <= 0):
                raise ValueError(f"external manifest entry {index + 1} has invalid nodeId")
            if isinstance(issued_at, bool) or not isinstance(issued_at, int) or issued_at < 0:
                raise ValueError(f"external manifest entry {index + 1} has invalid issuedAtMs")
            if not isinstance(issued_by, str) or not issued_by or len(issued_by) > 128:
                raise ValueError(f"external manifest entry {index + 1} has invalid issuedBy")
            records.append(Issuance(issuer, serial, fp, node_id, None, "backfill",
                                    issued_by, issued_at))

    # Revocation rows created before migration 031 carry a trusted issuer/serial
    # identity but no retained PEM. Keep them represented and require their
    # issuer to be present in the configured CA bundle.
    ca_ids = set(cas)
    for issuer, serial, fp, node_id, revoked_by, revoked_at in mysql(args,
        "SELECT issuerFp,serialHex,leafCertFp,COALESCE(nodeId,0),revokedBy "
        "FROM certificateRevocation"):
        issuer = issuer.lower()
        serial = serial.upper().lstrip("0") or "0"
        fp = fp.lower()
        if issuer not in ca_ids:
            raise ValueError(f"revocation {issuer}/{serial} refers to an issuer absent from the CA bundle")
        records.append(Issuance(issuer, serial, fp, int(node_id) or None, None,
                                "backfill", revoked_by or "historical-revocation", None))
    return records


def reconcile(args: argparse.Namespace, records: list[Issuance], ca_ids: set[str]
              ) -> tuple[list[Issuance], list[Issuance], int]:
    by_identity: dict[tuple[str, str], Issuance] = {}
    by_fp: dict[str, Issuance] = {}
    for record in records:
        prior = by_identity.get(record.identity)
        if prior and (prior.leaf_fp != record.leaf_fp or
                      (prior.node_id and record.node_id and prior.node_id != record.node_id)):
            raise ValueError(f"conflicting records for issuer/serial {record.issuer_fp}/{record.serial_hex}")
        if record.leaf_fp in by_fp and by_fp[record.leaf_fp].identity != record.identity:
            raise ValueError(f"leaf fingerprint {record.leaf_fp} maps to multiple issuer/serial identities")
        if prior is None or (prior.enrollment_id is None and record.enrollment_id is not None):
            by_identity[record.identity] = record
            by_fp[record.leaf_fp] = record

    existing = mysql(args,
        "SELECT issuerFp,serialHex,leafCertFp,COALESCE(nodeId,0),COALESCE(enrollmentId,0),"
        "origin,issuedBy,COALESCE(CAST(UNIX_TIMESTAMP(issuedAt)*1000 AS UNSIGNED),0) "
        "FROM certificateIssuance")
    known: dict[tuple[str, str], tuple[str, int, int, str]] = {}
    for issuer, serial, fp, node_id, enr_id, origin, issued_by, issued_at in existing:
        identity = (issuer.lower(), serial.upper().lstrip("0") or "0")
        if identity[0] not in ca_ids:
            raise ValueError(f"existing issuance {identity[0]}/{identity[1]} refers to an absent CA")
        record = Issuance(identity[0], identity[1], fp.lower(), int(node_id) or None,
                          int(enr_id) or None, origin, issued_by or "existing-inventory",
                          int(issued_at) or None)
        prior = by_identity.get(identity)
        if prior and (prior.leaf_fp.lower() != record.leaf_fp.lower() or
                      (prior.node_id and record.node_id and prior.node_id != record.node_id)):
            raise ValueError(f"existing inventory conflicts with source history for {identity[0]}/{identity[1]}")
        if prior is None or (prior.enrollment_id is None and record.enrollment_id is not None):
            by_identity[identity] = record
            by_fp[record.leaf_fp] = record
        known[identity] = (record.leaf_fp, int(node_id), int(enr_id), origin)
    current_nodes = mysql(args,
        "SELECT nodeId,leafCertSerial,leafCertFp,leafIssuerFp FROM node "
        "WHERE leafCertSerial IS NOT NULL OR leafCertFp IS NOT NULL OR leafIssuerFp IS NOT NULL")
    for node_id, serial, fp, issuer in current_nodes:
        if not serial or not fp or not issuer:
            raise ValueError(f"node {node_id} has a partial current certificate identity")
        identity = (issuer.lower(), serial.upper().lstrip("0") or "0")
        record = by_identity.get(identity)
        if record is None or record.leaf_fp.lower() != fp.lower() or record.node_id != int(node_id):
            raise ValueError(f"node {node_id} current certificate is absent or mismatched in supplied history")
    associated_nodes = sorted({record.node_id for record in by_identity.values()
                               if record.node_id is not None})
    if associated_nodes:
        present = {int(row[0]) for row in mysql(args,
            "SELECT nodeId FROM node WHERE nodeId IN (" +
            ",".join(str(node_id) for node_id in associated_nodes) + ")")}
        missing = sorted(set(associated_nodes) - present)
        if missing:
            raise ValueError("certificate history references missing node IDs: " +
                             ",".join(str(node_id) for node_id in missing))
    inserts: list[Issuance] = []
    enrichments: list[Issuance] = []
    for identity, record in by_identity.items():
        prior = known.get(identity)
        if prior and (prior[0] != record.leaf_fp.lower() or
                      prior[1] not in (0, record.node_id or 0) or
                      prior[2] not in (0, record.enrollment_id or 0)):
            raise ValueError(f"existing inventory conflicts for issuer/serial {identity[0]}/{identity[1]}")
        if prior is None:
            inserts.append(record)
        elif record.enrollment_id and prior[2] == 0:
            enrichments.append(record)
    for identity, prior in known.items():
        if identity not in by_identity:
            raise ValueError(f"existing issuance {identity[0]}/{identity[1]} cannot be reconciled to supplied history")
    state = mysql(args, "SELECT complete FROM certificateInventoryState WHERE singletonId=1")
    if len(state) != 1:
        raise ValueError("certificate inventory state row is missing or duplicated")
    return inserts, enrichments, int(state[0][0])


def apply_records(args: argparse.Namespace, inserts: list[Issuance],
                  enrichments: list[Issuance]) -> None:
    body = ["START TRANSACTION;"]
    for item in inserts:
        node_id = "NULL" if item.node_id is None else str(item.node_id)
        enrollment_id = "NULL" if item.enrollment_id is None else str(item.enrollment_id)
        body.append(
            "INSERT INTO certificateIssuance "
            "(issuerFp,serialHex,leafCertFp,nodeId,enrollmentId,origin,issuedBy,issuedAt) VALUES (" +
            ",".join([sql_string(item.issuer_fp), sql_string(item.serial_hex),
                      sql_string(item.leaf_fp), node_id, enrollment_id,
                      sql_string(item.origin), sql_string(item.issued_by),
                      ("NULL" if item.issued_at_ms is None else
                       "FROM_UNIXTIME(" + str(item.issued_at_ms) + "/1000)")]) + ");")
        if item.enrollment_id is not None:
            body.append(
                "UPDATE certificateRevocation SET enrollmentId=" + str(item.enrollment_id) +
                " WHERE issuerFp=" + sql_string(item.issuer_fp) + " AND serialHex=" +
                sql_string(item.serial_hex) + " AND enrollmentId IS NULL;")
    for item in enrichments:
        body.append(
            "UPDATE certificateIssuance SET enrollmentId=" + str(item.enrollment_id) +
            ",nodeId=" + ("NULL" if item.node_id is None else str(item.node_id)) +
            ",origin='enrollment',issuedBy=" + sql_string(item.issued_by) +
            ",issuedAt=" + ("NULL" if item.issued_at_ms is None else
            "FROM_UNIXTIME(" + str(item.issued_at_ms) + "/1000)") + " WHERE issuerFp=" +
            sql_string(item.issuer_fp) + " AND serialHex=" + sql_string(item.serial_hex) +
            " AND enrollmentId IS NULL;")
        body.append(
            "UPDATE certificateRevocation SET enrollmentId=" + str(item.enrollment_id) +
            " WHERE issuerFp=" + sql_string(item.issuer_fp) + " AND serialHex=" +
            sql_string(item.serial_hex) + " AND enrollmentId IS NULL;")
    body += [
        "UPDATE certificateInventoryState SET complete=1,completedAt=UTC_TIMESTAMP(6),completedBy=" +
        sql_string(args.operator) + " WHERE singletonId=1 AND complete=0;",
        "COMMIT;",
    ]
    # MariaDB's handler rolls back the entire batch if any row or gate update
    # fails, so the client cannot accidentally commit a partial inventory.
    procedure = "solari_inventory_" + str(os.getpid())
    sql = ("DELIMITER //\nCREATE PROCEDURE " + procedure + "() BEGIN\n"
           "DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN ROLLBACK; RESIGNAL; END;\n" +
           "\n".join(body) + "\nEND//\nDELIMITER ;\nCALL " + procedure + "();\nDROP PROCEDURE " +
           procedure + ";\n")
    try:
        mysql(args, sql)
    except ValueError:
        try:
            mysql(args, "DROP PROCEDURE IF EXISTS " + procedure + ";")
        except ValueError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, help="MariaDB schema name")
    parser.add_argument("--ca-bundle", required=True, help="PEM bundle containing all issuing CA certificates")
    parser.add_argument("--external-manifest", help="JSON archive of direct SIGN outputs and node associations")
    parser.add_argument("--operator", default="", help="audited operator name used when completing inventory")
    parser.add_argument("--apply", action="store_true", help="write rows and complete the readiness gate")
    parser.add_argument("--confirm-complete", action="store_true",
                        help="attest all historical direct-sign outputs are supplied or never existed")
    parser.add_argument("--confirm-quiesced", action="store_true",
                        help="attest that the server and all certificate issuers are stopped")
    parser.add_argument("--defaults-file", help="MariaDB option file; credentials stay out of argv")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--user")
    args = parser.parse_args()
    try:
        if not NAME_RE.fullmatch(args.database):
            raise ValueError("database name must contain only letters, digits, and underscores")
        if args.apply and not (args.confirm_complete and args.confirm_quiesced and args.operator.strip()):
            raise ValueError("--apply requires --confirm-complete, --confirm-quiesced, and --operator")
        if args.confirm_complete and not args.operator.strip():
            raise ValueError("--confirm-complete requires a non-empty --operator")
        if len(args.operator) > 128 or any(ord(char) < 32 for char in args.operator):
            raise ValueError("operator attribution must be at most 128 characters without controls")
        ca_bundle = pathlib.Path(args.ca_bundle).expanduser().read_bytes()
        records = read_inputs(args, ca_bundle)
        inserts, enrichments, complete = reconcile(args, records, set(ca_fingerprints(ca_bundle)))
        print(f"certificates_seen={len({r.identity for r in records})} "
              f"new={len(inserts)} enriched={len(enrichments)} "
              f"already_complete={complete}")
        if args.apply:
            if complete:
                print("inventory is already marked complete; no write performed")
            else:
                apply_records(args, inserts, enrichments)
                print("inventory reconciliation applied and readiness gate completed")
        else:
            print("dry-run only; no database changes made")
    except (OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        print(f"certificate inventory backfill failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
