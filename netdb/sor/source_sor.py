#!/usr/bin/env python3
"""SoR-backed source boundary for the DNS generator.

This is the closed-loop replacement for gen-zones.py's YAML `load_source()`:
DNS is rendered FROM the System of Record instead of from akoria-hosts.yml.
It returns the exact same (domain, hosts, ifaces, cnames) shapes, reading the
three renderer-facing views the schema ships (v_dns_hosts / v_dns_ifaces /
v_dns_cnames), which already normalize INET6 v4-mapped addresses back to
dotted-quad.

Config via env (same DB the seeder writes):
  SOR_DB_HOST      default 10.1.0.200
  SOR_DB_USER      default solari
  SOR_DB_PASSWORD  required
  SOR_DB_NAME      default sor
"""
import os

import pymysql


def load_source():
    """Source-of-truth boundary. Returns (domain, hosts, ifaces, cnames)."""
    cn = pymysql.connect(
        host=os.environ.get("SOR_DB_HOST", "10.1.0.200"),
        user=os.environ.get("SOR_DB_USER", "solari"),
        password=os.environ["SOR_DB_PASSWORD"],
        database=os.environ.get("SOR_DB_NAME", "sor"),
        charset="utf8mb4",
    )
    with cn:
        with cn.cursor() as c:
            c.execute("SELECT name FROM dns_zones WHERE kind='forward' "
                      "AND authority='sor_rendered' AND deleted_at IS NULL LIMIT 1")
            domain = c.fetchone()[0]
            c.execute("SELECT host, ip, role FROM v_dns_hosts")
            hosts = {h: {"ip": ip, "role": role} for h, ip, role in c.fetchall()}
            c.execute("SELECT label, ip FROM v_dns_ifaces")
            ifaces = dict(c.fetchall())
            c.execute("SELECT alias, target_host FROM v_dns_cnames "
                      "WHERE target_host IS NOT NULL")
            cnames = dict(c.fetchall())
    return domain, hosts, ifaces, cnames
