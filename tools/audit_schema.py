#!/usr/bin/env python3
"""Read-only comparison of a live schema with the generated migration snapshot.

This checks current schema shape only. It does not prove which migrations ran,
validate application data, or authorize migration-ledger adoption.
"""
from __future__ import annotations

import argparse
import difflib
import pathlib
import re
import subprocess
import sys

from export_schema import commonClientArgs, normalizeDefiners

ROOT = pathlib.Path(__file__).resolve().parents[1]
DB_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
LEDGER = "solari_migration_ledger"


def snapshotPath(database: str) -> pathlib.Path:
    if database == "monitoring":
        return ROOT / "db" / "schema.sql"
    return ROOT / "netdb" / "sor" / "schema.snapshot.sql"


def snapshotBody(text: str) -> str:
    """Drop generated manifest comments while preserving dump SQL comments."""
    lines = text.splitlines()
    first = next((index for index, line in enumerate(lines)
                  if line.strip() and not line.startswith("--")), None)
    if first is None:
        return ""
    return normalizeSchemaDdl("\n".join(lines[first:]).rstrip() + "\n")


def normalizeSchemaDdl(sql: str) -> str:
    """Ignore dump-only account, ID-counter, and delimiter formatting differences."""
    sql = normalizeDefiners(sql)
    sql = re.sub(r"\sAUTO_INCREMENT=\d+\b", "", sql)
    return re.sub(r"[ \t]*\n[ \t]*\*/;;", " */;;", sql)


def runQuery(args: argparse.Namespace, sql: str) -> str:
    command = ["mariadb", *commonClientArgs(args), "--batch", "--skip-column-names",
               "--default-character-set=utf8mb4", "--database=" + args.dbName,
               "--execute=" + sql]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError("MariaDB query failed: " + result.stderr.strip()[:500])
    return result.stdout.strip()


def liveDump(args: argparse.Namespace) -> str:
    """Export only schema, omitting the runner's own ledger if it exists."""
    ledger = runQuery(
        args,
        "SELECT COUNT(*) FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='solari_migration_ledger' "
        "AND TABLE_TYPE='BASE TABLE';",
    ) == "1"
    command = ["mariadb-dump", *commonClientArgs(args), "--no-data", "--routines", "--events",
               "--skip-comments", "--skip-dump-date", "--skip-lock-tables",
               "--skip-tz-utc", "--skip-add-locks", "--skip-disable-keys"]
    if ledger:
        command.append("--ignore-table=" + args.dbName + "." + LEDGER)
    command.append(args.dbName)
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError("mariadb-dump failed: " + result.stderr.strip()[:500])
    return normalizeSchemaDdl("\n".join(
        line.rstrip() for line in result.stdout.rstrip().splitlines()) + "\n")


def audit(args: argparse.Namespace, output=sys.stdout) -> bool:
    expected = snapshotBody(snapshotPath(args.database).read_text(encoding="utf-8"))
    actual = liveDump(args)
    if actual == expected:
        print("MATCH: live schema matches the candidate snapshot.", file=output)
        print("This does not establish migration history, data safety, or grants.",
              file=output)
        return True
    diff = difflib.unified_diff(
        expected.splitlines(keepends=True), actual.splitlines(keepends=True),
        fromfile="candidate-snapshot", tofile="live-schema")
    output.writelines(diff)
    print("\nDIFFERENT: review schema drift before any migration or adoption.", file=output)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", choices=["monitoring", "sor"], required=True)
    parser.add_argument("--db-name", dest="dbName", required=True)
    parser.add_argument("--defaults-file", dest="defaultsFile",
                        help="MariaDB option file; keep credentials out of arguments")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--user")
    args = parser.parse_args()
    if not DB_NAME_RE.fullmatch(args.dbName):
        parser.error("database name must be alphanumeric or underscore")
    try:
        return 0 if audit(args) else 1
    except (OSError, RuntimeError, ValueError) as error:
        print(f"schema audit failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
