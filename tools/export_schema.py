#!/usr/bin/env python3
"""Generate current schema snapshots from a clean migration run.

The migration runner remains the install authority. This file is a reproducible,
reviewable schema snapshot for inspection and tooling, never a replacement for
ledgered installation.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import secrets
import subprocess
import sys
import tempfile

from migrate import EXPECTED_CHARACTER_SET, EXPECTED_COLLATION, migrationFiles

ROOT = pathlib.Path(__file__).resolve().parents[1]
DB_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
DEFINER_ID = r"`(?:``|[^`])*`@`(?:``|[^`])*`"


def normalizeDefiners(sql: str) -> str:
    """Leave DEFINER implicit so snapshots do not depend on the dump account."""
    sql = re.sub(r"/\*!50017 DEFINER=" + DEFINER_ID + r"\*/\s*", "", sql)
    return re.sub(
        r"/\*!50013 DEFINER=" + DEFINER_ID + r" (SQL SECURITY (?:DEFINER|INVOKER)) \*/",
        r"/*!50013 \1 */",
        sql,
    )


def commonClientArgs(args: argparse.Namespace) -> list[str]:
    """Build safe MariaDB connection flags without exposing credential values."""
    result: list[str] = []
    if args.defaultsFile:
        result.append("--defaults-extra-file=" + args.defaultsFile)
    if args.host:
        result.append("--host=" + args.host)
    if args.port:
        result.append("--port=" + str(args.port))
    if args.user:
        result.append("--user=" + args.user)
    return result


def runSql(args: argparse.Namespace, sql: str) -> None:
    """Execute server-level SQL and report bounded, credential-free errors."""
    command = ["mariadb", *commonClientArgs(args), "--batch", "--skip-column-names"]
    result = subprocess.run(command, input=sql, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError("MariaDB command failed: " + result.stderr.strip()[:500])


def migrationManifest(stream: str) -> list[tuple[str, str]]:
    """Return ordered migration IDs and hashes using the canonical runner order."""
    return [(migrationId, checksum)
            for migrationId, _path, checksum in migrationFiles(stream)]


def export(args: argparse.Namespace) -> None:
    """Build the exact migrated schema in a disposable DB and atomically export it."""
    defaultOutput = (ROOT / "db" / "schema.sql" if args.database == "monitoring"
                     else ROOT / "netdb" / "sor" / "schema.snapshot.sql")
    output = pathlib.Path(args.output or defaultOutput).expanduser().resolve()
    if not DB_NAME_RE.fullmatch(args.dbNamePrefix):
        raise ValueError("database prefix must contain only letters, digits, and underscores")
    scratchName = f"{args.dbNamePrefix}_{os.getpid()}_{secrets.token_hex(5)}"
    if len(scratchName) > 64:
        raise ValueError("database prefix is too long")
    created = False
    try:
        runSql(args, f"CREATE DATABASE `{scratchName}` CHARACTER SET "
                     f"{EXPECTED_CHARACTER_SET} COLLATE {EXPECTED_COLLATION};")
        created = True
        migrate = [sys.executable, str(ROOT / "tools" / "migrate.py"),
                   "--database", args.database, "--db-name", scratchName]
        if args.defaultsFile:
            migrate += ["--defaults-file", args.defaultsFile]
        if args.host:
            migrate += ["--host", args.host]
        if args.port:
            migrate += ["--port", str(args.port)]
        if args.user:
            migrate += ["--user", args.user]
        subprocess.run(migrate, cwd=ROOT, check=True)
        subprocess.run(migrate + ["--check"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

        dump = ["mariadb-dump", *commonClientArgs(args), "--no-data", "--routines", "--events",
                "--skip-comments", "--skip-dump-date", "--skip-lock-tables",
                "--skip-tz-utc", "--skip-add-locks", "--skip-disable-keys",
                "--ignore-table=" + scratchName + ".solari_migration_ledger", scratchName]
        result = subprocess.run(dump, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError("mariadb-dump failed: " + result.stderr.strip()[:500])
        dumpText = normalizeDefiners(
            "\n".join(line.rstrip() for line in result.stdout.rstrip().splitlines()) + "\n"
        )
        manifest = migrationManifest(args.database)
        header = [
            "-- GENERATED FILE: tools/export_schema.py",
            f"-- Source: {args.database} migrations in tools/migrate.py execution order.",
            "-- The migration runner, not this snapshot, is authoritative for installs.",
            f"-- Regenerate with: python3 tools/export_schema.py --database {args.database}",
            "-- Applied migration manifest:",
        ]
        header.extend(f"--   {migrationId} sha256={checksum}" for migrationId, checksum in manifest)
        content = "\n".join(header) + "\n\n" + dumpText
    finally:
        if created:
            runSql(args, f"DROP DATABASE `{scratchName}`;")

    output.parent.mkdir(parents=True, exist_ok=True)
    tempFile: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=output.parent,
                                         prefix=output.name + ".", suffix=".tmp",
                                         delete=False) as file:
            tempFile = pathlib.Path(file.name)
            file.write(content)
            file.flush()
            os.fsync(file.fileno())
        os.replace(tempFile, output)
        tempFile = None
    finally:
        if tempFile is not None:
            tempFile.unlink(missing_ok=True)
    print(f"generated {output} from {len(manifest)} migrations")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", choices=["monitoring", "sor"], default="monitoring")
    parser.add_argument("--output", help="snapshot path (defaults to each stream's snapshot file)")
    parser.add_argument("--db-name-prefix", dest="dbNamePrefix", default="solari_schema_export")
    parser.add_argument("--defaults-file", dest="defaultsFile",
                        help="MariaDB option file; secrets stay out of argv")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--user")
    args = parser.parse_args()
    try:
        export(args)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"schema export failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
