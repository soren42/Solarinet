#!/usr/bin/env python3
"""Apply ordered SolariNet SQL migrations with checksums and a durable ledger.

Requires the MariaDB command-line client. Existing databases without a ledger
are refused: their historical migration set must be audited and adopted by an
operator before this tool can safely make changes.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
ID_RE = re.compile(r"^[A-Za-z0-9_-]+$")
LEDGER = "solari_migration_ledger"


def migrationFiles(stream):
    """Return immutable (ID, path, checksum) entries in execution order."""
    if stream == "monitoring":
        folder = ROOT / "db" / "migrations"
        paths = sorted(folder.glob("*.sql"), key=lambda p: (int(p.name[:3]), p.name))
    else:
        folder = ROOT / "netdb" / "sor"
        paths = [folder / "schema.sql"]
        paths += sorted((folder / "migrations").glob("*.sql"),
                        key=lambda p: (int(p.name[:3]), p.name))
    return [(path.stem, path, hashlib.sha256(path.read_bytes()).hexdigest())
            for path in paths]


def mysqlCommand(args, sql):
    """Run SQL against one named DB; return stdout or raise without exposing secrets."""
    cmd = ["mariadb"]
    if args.defaultsFile:
        cmd.append("--defaults-extra-file=" + args.defaultsFile)
    if args.host:
        cmd.append("--host=" + args.host)
    if args.port:
        cmd.append("--port=" + str(args.port))
    if args.user:
        cmd.append("--user=" + args.user)
    cmd += ["--batch", "--skip-column-names", "--default-character-set=utf8mb4",
            "--database=" + args.dbName]
    run = subprocess.run(cmd, input=sql, text=True, capture_output=True)
    if run.returncode:
        raise RuntimeError("MariaDB command failed: " + run.stderr.strip()[:500])
    return run.stdout


def quote(value):
    """Quote an internal migration ID or checksum as a SQL string literal."""
    if not ID_RE.fullmatch(value):
        raise ValueError("invalid migration identifier")
    return "'" + value + "'"


def inventory(args):
    """Return current table names, including any existing migration ledger."""
    rows = mysqlCommand(args,
        "SELECT TABLE_NAME FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME;")
    return set(rows.splitlines())


def ledgerRows(args):
    """Return applied and interrupted rows keyed by their immutable file IDs."""
    raw = mysqlCommand(args, f"SELECT migration_id,checksum,state FROM {LEDGER} ORDER BY migration_id;")
    result = {}
    for line in raw.splitlines():
        fields = line.split("\t")
        if len(fields) != 3:
            raise RuntimeError("invalid migration ledger row")
        result[fields[0]] = (fields[1], fields[2])
    return result


def migrate(args):
    """Refuse unknown history, then apply and record one ordered SQL file at a time."""
    files = migrationFiles(args.database)
    tables = inventory(args)
    if LEDGER not in tables and tables:
        raise RuntimeError("untracked nonempty database; audit and adopt its migration "
                           "history before applying any SQL")
    if LEDGER not in tables and args.check:
        for fileId, _path, checksum in files:
            print(f"PENDING {fileId} {checksum[:12]}")
        return
    if LEDGER not in tables:
        mysqlCommand(args, f"""CREATE TABLE {LEDGER} (
            migration_id VARCHAR(128) PRIMARY KEY,
            checksum CHAR(64) NOT NULL,
            state ENUM('applying','applied') NOT NULL,
            started_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            applied_at TIMESTAMP NULL
        ) ENGINE=InnoDB;""")
    existing = ledgerRows(args)
    expected = {entry[0] for entry in files}
    for fileId, (checksum, state) in existing.items():
        if fileId not in expected:
            raise RuntimeError(f"ledger contains missing file: {fileId}")
        correct = next(entry[2] for entry in files if entry[0] == fileId)
        if checksum != correct or state != "applied":
            raise RuntimeError(f"checksum mismatch or interrupted migration: {fileId}")
    gap = False
    for fileId, path, checksum in files:
        if fileId in existing:
            if gap:
                raise RuntimeError(f"out-of-order migration history before {fileId}")
            continue
        gap = True
        if args.check:
            print(f"PENDING {fileId} {checksum[:12]}")
            continue
        mysqlCommand(args, f"INSERT INTO {LEDGER} (migration_id,checksum,state) "
                           f"VALUES ({quote(fileId)},{quote(checksum)},'applying');")
        try:
            mysqlCommand(args, path.read_text(encoding="utf-8"))
        except Exception:
            raise RuntimeError(f"migration {fileId} failed; ledger left as applying "
                               "for manual inspection") from None
        mysqlCommand(args, f"UPDATE {LEDGER} SET state='applied',applied_at=NOW() "
                           f"WHERE migration_id={quote(fileId)} AND state='applying';")
        print(f"APPLIED {fileId} {checksum[:12]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", choices=["monitoring", "sor"], required=True)
    parser.add_argument("--db-name", dest="dbName", required=True)
    parser.add_argument("--defaults-file", dest="defaultsFile",
                        help="MariaDB option file path; never include credentials in arguments")
    parser.add_argument("--host", help="MariaDB host; default uses the local socket")
    parser.add_argument("--port", type=int, help="MariaDB TCP port")
    parser.add_argument("--user", help="MariaDB account; password belongs in an option file")
    parser.add_argument("--check", action="store_true", help="report pending files only")
    args = parser.parse_args()
    if not NAME_RE.fullmatch(args.dbName):
        parser.error("database name must be alphanumeric or underscore")
    try:
        migrate(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"migration blocked: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
