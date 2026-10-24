#!/usr/bin/env python3
"""EXPLAIN the dashboard's exact CRL rollout SQL on CI's scratch MariaDB."""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SQL_LITERAL = re.compile(r"Db::rows\(\s*'((?:\\.|[^'\\])*)'\s*\)", re.S)


def main() -> int:
    if os.environ.get("SOLARI_TEST_DB") != "1":
        print("SKIP test_ca_crl_sql: SOLARI_TEST_DB unset")
        return 0
    host = os.environ.get("SOLARI_DB_HOST", "")
    port = os.environ.get("SOLARI_DB_PORT", "3306")
    user = os.environ.get("SOLARI_DB_USER", "")
    database = os.environ.get("SOLARI_DB_NAME", "")
    if host not in {"127.0.0.1", "localhost"} or not port.isdigit() \
            or not re.fullmatch(r"solarinet_[a-z0-9_]+", database):
        print("Refusing to query a non-local or non-scratch database", file=sys.stderr)
        return 2

    source = (ROOT / "dashboard/api/lib/Ca.php").read_text(encoding="utf-8")
    method_start = source.index("private static function crlRollout()")
    method_end = source.find("private static function ", method_start + 1)
    if method_end < 0:
        method_end = len(source)
    literals = SQL_LITERAL.findall(source[method_start:method_end])
    if len(literals) != 3:
        raise RuntimeError(f"Expected exactly three CRL rollout queries; found {len(literals)}")
    for index, literal in enumerate(literals, start=1):
        sql = literal.replace("\\\\", "\\").replace("\\'", "'")
        result = subprocess.run(
            ["mariadb", "--batch", "--skip-column-names", "-h", host, "-P", port,
             "-u", user, "--database=" + database, "--execute", "EXPLAIN " + sql],
            env=os.environ.copy(), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(
                f"MariaDB rejected CRL rollout query {index}/3; "
                f"check the migrated scratch schema: {detail}"
            )
        print(f"CRL rollout query {index}/3 parsed by MariaDB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
