#!/usr/bin/env python3
"""Generate and atomically publish a CA-signed CRL from the F-01 ledger.

The database remains authoritative. OpenSSL receives only a disposable index
file reconstructed from one issuer's ledger rows; its CA database is never
persisted. Run this under an account allowed to read the selected CA key.
"""
from __future__ import annotations

import argparse
import datetime
import fcntl
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HEX_RE = re.compile(r"^[0-9A-Fa-f]{1,40}$")
FP_RE = re.compile(r"^[0-9A-Fa-f]{64}$")
REASONS = {"unspecified", "keyCompromise", "affiliationChanged", "superseded",
           "cessationOfOperation"}
DB_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")


def command(argv: list[str], *, input_bytes: bytes | None = None) -> bytes:
    result = subprocess.run(argv, input=input_bytes, capture_output=True)
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()[:500]
        raise ValueError(f"{pathlib.Path(argv[0]).name} failed: {detail}")
    return result.stdout


def sql_text(value: str) -> str:
    encoded = value.encode("utf-8").hex()
    return "CONVERT(0x" + encoded + " USING utf8mb4)" if encoded else "CONVERT(X'' USING utf8mb4)"


def mysql(args: argparse.Namespace, sql: str) -> list[list[str]]:
    argv = ["mariadb"]
    if args.defaults_file:
        argv.append("--defaults-extra-file=" + str(args.defaults_file))
    argv += ["--batch", "--raw", "--skip-column-names", "--default-character-set=utf8mb4"]
    if args.host:
        argv.append("--host=" + args.host)
    if args.port:
        argv.append("--port=" + str(args.port))
    if args.user:
        argv.append("--user=" + args.user)
    argv.append("--database=" + args.database)
    output = command(argv, input_bytes=sql.encode("utf-8")).decode("utf-8")
    return [line.split("\t") for line in output.splitlines() if line]


def fingerprint(cert_path: pathlib.Path) -> str:
    der = command(["openssl", "x509", "-in", str(cert_path), "-outform", "DER"])
    return hashlib.sha256(der).hexdigest()


def assert_key_matches(cert_path: pathlib.Path, key_path: pathlib.Path) -> None:
    cert_pub = command(["openssl", "x509", "-in", str(cert_path), "-pubkey", "-noout"])
    cert_der = command(["openssl", "pkey", "-pubin", "-outform", "DER"], input_bytes=cert_pub)
    key_der = command(["openssl", "pkey", "-in", str(key_path), "-pubout", "-outform", "DER"])
    if cert_der != key_der:
        raise ValueError("CA certificate and private key do not match")


def crl_index(revocations: list[tuple[str, str, int]], now: datetime.datetime) -> bytes:
    lines = []
    for serial, reason, revoked_at_ms in sorted(revocations, key=lambda row: row[0]):
        if not HEX_RE.fullmatch(serial) or reason not in REASONS or revoked_at_ms <= 0:
            raise ValueError("revocation ledger contains malformed serial, reason, or time")
        revoked_at = datetime.datetime.fromtimestamp(revoked_at_ms / 1000,
                                                    tz=datetime.timezone.utc)
        if revoked_at > now + datetime.timedelta(minutes=1):
            raise ValueError("revocation ledger contains a future timestamp")
        # OpenSSL's index format uses UTCTime before 2050 and GeneralizedTime
        # thereafter. A far-future expiry keeps the reconstructed revoked-only
        # entries from being pruned as expired; expiry is not copied into CRL.
        date_format = "%y%m%d%H%M%SZ" if revoked_at.year < 2050 else "%Y%m%d%H%M%SZ"
        revoke_text = revoked_at.strftime(date_format)
        serial = serial.upper().lstrip("0") or "0"
        if len(serial) % 2:
            serial = "0" + serial
        lines.append("R\t99991231235959Z\t" + revoke_text + "," + reason +
                     "\t" + serial + "\tunknown\t/CN=SolariNet-Revoked")
    return ("\n".join(lines) + ("\n" if lines else "")).encode("ascii")


def parse_crl_metadata(crl_path: pathlib.Path) -> tuple[int, datetime.datetime, datetime.datetime, set[str], bytes]:
    number_text = command(["openssl", "crl", "-in", str(crl_path), "-noout", "-crlnumber"])
    match = re.fullmatch(rb"crlNumber=(?:0x)?([0-9A-Fa-f]+)\s*", number_text)
    if not match:
        raise ValueError("generated CRL has no parseable CRL number: " +
                         number_text.decode("ascii", "replace").strip()[:120])
    number = int(match.group(1), 16)
    last_text = command(["openssl", "crl", "-in", str(crl_path), "-noout", "-lastupdate"])
    match = re.fullmatch(rb"lastUpdate=(.+)\s*", last_text)
    if not match:
        raise ValueError("generated CRL has no lastUpdate")
    last_update = datetime.datetime.strptime(match.group(1).decode("ascii"),
                                             "%b %d %H:%M:%S %Y %Z").replace(
                                                 tzinfo=datetime.timezone.utc)
    next_text = command(["openssl", "crl", "-in", str(crl_path), "-noout", "-nextupdate"])
    match = re.fullmatch(rb"nextUpdate=(.+)\s*", next_text)
    if not match:
        raise ValueError("generated CRL has no nextUpdate")
    next_update = datetime.datetime.strptime(match.group(1).decode("ascii"),
                                             "%b %d %H:%M:%S %Y %Z").replace(
                                                 tzinfo=datetime.timezone.utc)
    text = command(["openssl", "crl", "-in", str(crl_path), "-noout", "-text"])
    serials = {((value.decode("ascii").upper().lstrip("0")) or "0")
               for value in re.findall(rb"Serial Number:\s*([0-9A-Fa-f]+)", text)}
    der = command(["openssl", "crl", "-in", str(crl_path), "-outform", "DER"])
    return number, last_update, next_update, serials, der


def verify_candidate(crl_path: pathlib.Path, ca_path: pathlib.Path, expected_number: int,
                     revocations: list[tuple[str, str, int]], hours: int) -> tuple[str, datetime.datetime]:
    command(["openssl", "crl", "-in", str(crl_path), "-noout", "-verify", "-CAfile", str(ca_path)])
    crl_issuer = command(["openssl", "crl", "-in", str(crl_path), "-noout", "-issuer",
                          "-nameopt", "RFC2253"]).decode("utf-8", "replace").strip()
    ca_subject = command(["openssl", "x509", "-in", str(ca_path), "-noout", "-subject",
                          "-nameopt", "RFC2253"]).decode("utf-8", "replace").strip()
    if not crl_issuer.startswith("issuer=") or not ca_subject.startswith("subject=") or \
            crl_issuer.removeprefix("issuer=") != ca_subject.removeprefix("subject="):
        raise ValueError("generated CRL issuer name differs from the configured CA subject")
    number, last_update, next_update, serials, der = parse_crl_metadata(crl_path)
    expected_serials = {(serial.upper().lstrip("0") or "0") for serial, _, _ in revocations}
    now = datetime.datetime.now(datetime.timezone.utc)
    if number != expected_number:
        raise ValueError(f"generated CRL number {number} differs from allocated {expected_number}")
    if serials != expected_serials:
        raise ValueError("generated CRL revoked serial set differs from the database ledger")
    if not now - datetime.timedelta(minutes=5) <= last_update <= now + datetime.timedelta(minutes=2):
        raise ValueError("generated CRL thisUpdate is outside the permitted clock-skew bound")
    if not now < next_update <= now + datetime.timedelta(hours=hours, minutes=2):
        raise ValueError("generated CRL freshness interval is outside the configured bound")
    return hashlib.sha256(der).hexdigest(), next_update


def allocate_number(args: argparse.Namespace, issuer_fp: str) -> int:
    result = mysql(args,
        "INSERT IGNORE INTO certificateCrlState(issuerFp,nextNumber) VALUES (" +
        sql_text(issuer_fp) + ",1); UPDATE certificateCrlState "
        "SET nextNumber=LAST_INSERT_ID(nextNumber+1) WHERE issuerFp=" + sql_text(issuer_fp) +
        "; SELECT LAST_INSERT_ID()-1;")
    values = [row[0] for row in result if len(row) == 1 and row[0].isdigit()]
    if not values:
        raise ValueError("could not allocate a monotonic CRL number")
    number = int(values[-1])
    if number < 1 or number >= 2**64:
        raise ValueError("allocated CRL number is outside supported range")
    return number


def read_ledger(args: argparse.Namespace, issuer_fp: str) -> list[tuple[str, str, int]]:
    state = mysql(args, "SELECT complete FROM certificateInventoryState WHERE singletonId=1")
    if len(state) != 1 or state[0][0] != "1":
        raise ValueError("certificate inventory became incomplete during CRL generation")
    rows = mysql(args,
        "SELECT serialHex,reason,CAST(UNIX_TIMESTAMP(revokedAt)*1000 AS UNSIGNED) "
        "FROM certificateRevocation WHERE issuerFp=" + sql_text(issuer_fp) +
        " ORDER BY serialHex")
    result: list[tuple[str, str, int]] = []
    seen: set[str] = set()
    for serial, reason, revoked_at in rows:
        canonical = serial.upper().lstrip("0") or "0"
        if canonical in seen:
            raise ValueError("revocation ledger repeats a serial for the configured CA")
        seen.add(canonical)
        result.append((canonical, reason, int(revoked_at)))
    result.sort(key=lambda row: row[0])
    return result


def publish_state(args: argparse.Namespace, issuer_fp: str, number: int,
                  crl_fp: str, next_update: datetime.datetime) -> None:
    next_ms = int(next_update.timestamp() * 1000)
    result = mysql(args,
        "UPDATE certificateCrlState SET publishedNumber=" + str(number) +
        ",generatedAt=UTC_TIMESTAMP(6),crlFp=" + sql_text(crl_fp) +
        ",nextUpdate=FROM_UNIXTIME(" + str(next_ms) + "/1000) WHERE issuerFp=" +
        sql_text(issuer_fp) + " AND COALESCE(publishedNumber,0)<" + str(number) +
        "; SELECT ROW_COUNT();")
    counts = [int(row[0]) for row in result if len(row) == 1 and row[0].isdigit()]
    if not counts or counts[-1] != 1:
        raise ValueError("published CRL state could not be committed monotonically")


def generate(args: argparse.Namespace) -> None:
    ca_path = pathlib.Path(args.ca_cert).resolve(strict=True)
    key_path = pathlib.Path(args.ca_key).resolve(strict=True)
    output_path = pathlib.Path(args.output).expanduser().resolve()
    if not output_path.parent.is_dir():
        raise ValueError("CRL output directory must already exist")
    if not 1 <= args.crl_hours <= 168:
        raise ValueError("CRL lifetime must be from 1 through 168 hours")
    if not DB_NAME_RE.fullmatch(args.database):
        raise ValueError("database name must contain only letters, digits, and underscores")
    if output_path in {ca_path, key_path}:
        raise ValueError("CRL output must not overwrite the CA certificate or private key")

    issuer_fp = fingerprint(ca_path)
    assert_key_matches(ca_path, key_path)
    revocations = read_ledger(args, issuer_fp)
    other_issuers = mysql(args,
        "SELECT DISTINCT issuerFp FROM certificateRevocation WHERE issuerFp<>" + sql_text(issuer_fp))
    if other_issuers and not getattr(args, "permit_other_issuers", False):
        raise ValueError("ledger has other issuing CAs; run a CRL generation for every issuer, "
                         "using --permit-other-issuers for each per-CA run")
    lock_path = output_path.with_name(output_path.name + ".lock")
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        number = allocate_number(args, issuer_fp)
        if output_path.exists():
            command(["openssl", "crl", "-in", str(output_path), "-noout", "-verify",
                     "-CAfile", str(ca_path)])
            old_issuer = command(["openssl", "crl", "-in", str(output_path), "-noout",
                                  "-issuer", "-nameopt", "RFC2253"]).decode(
                                      "utf-8", "replace").strip()
            configured_subject = command(["openssl", "x509", "-in", str(ca_path), "-noout",
                                           "-subject", "-nameopt", "RFC2253"]).decode(
                                               "utf-8", "replace").strip()
            if old_issuer.removeprefix("issuer=") != \
                    configured_subject.removeprefix("subject="):
                raise ValueError("existing CRL issuer differs from the configured CA subject")
            old_number, _old_last, _old_next, _old_serials, _old_der = parse_crl_metadata(output_path)
            if number <= old_number:
                raise ValueError("allocated CRL number would roll back the existing publication")
        with tempfile.TemporaryDirectory(prefix=".solari-crl-", dir=output_path.parent) as temp_name:
            temp_dir = pathlib.Path(temp_name)
            new_certs = temp_dir / "newcerts"
            new_certs.mkdir(mode=0o700)
            index_path = temp_dir / "index.txt"
            index_path.write_bytes(crl_index(revocations, datetime.datetime.now(datetime.timezone.utc)))
            (temp_dir / "serial").write_text("01\n", encoding="ascii")
            number_hex = format(number, "X")
            if len(number_hex) % 2:
                number_hex = "0" + number_hex
            (temp_dir / "crlnumber").write_text(number_hex + "\n", encoding="ascii")
            config_path = temp_dir / "ca.cnf"
            config = (
                "[ ca ]\ndefault_ca = issuer\n"
                "[ issuer ]\n"
                f"dir = {temp_dir}\n"
                "database = $dir/index.txt\n"
                "new_certs_dir = $dir/newcerts\n"
                f"certificate = {ca_path}\nprivate_key = {key_path}\n"
                "serial = $dir/serial\ncrlnumber = $dir/crlnumber\n"
                "default_md = sha256\n"
                f"default_crl_hours = {args.crl_hours}\n"
                "policy = policy\nunique_subject = no\n"
                "[ policy ]\ncommonName = supplied\n"
            )
            config_path.write_text(config, encoding="utf-8")
            candidate = temp_dir / "candidate.crl.pem"
            command(["openssl", "ca", "-batch", "-config", str(config_path),
                     "-gencrl", "-out", str(candidate)])
            crl_fp, next_update = verify_candidate(candidate, ca_path, number,
                                                   revocations, args.crl_hours)
            if read_ledger(args, issuer_fp) != revocations:
                raise ValueError("revocation ledger changed during CRL generation; retry")
            with candidate.open("rb") as stream:
                os.fsync(stream.fileno())
            os.replace(candidate, output_path)
            directory_fd = os.open(output_path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            publish_state(args, issuer_fp, number, crl_fp, next_update)
            print(f"issuer_fp={issuer_fp} crl_number={number} revoked={len(revocations)} "
                  f"crl_fp={crl_fp} next_update={next_update.isoformat()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--ca-cert", required=True)
    parser.add_argument("--ca-key", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--crl-hours", type=int, default=24)
    parser.add_argument("--permit-other-issuers", action="store_true",
                        help="generate only the selected CA CRL when other issuers also have ledger entries")
    parser.add_argument("--defaults-file")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--user")
    args = parser.parse_args()
    try:
        generate(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"CRL generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
