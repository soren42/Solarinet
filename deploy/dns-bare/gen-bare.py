#!/usr/bin/env python3
"""gen-bare.py — emit single-label (NetBIOS/bare-name) master zones for every
akoria.net host + CNAME alias, so dumb SMB/legacy clients that query "NAS-X"
(no akoria.net suffix) resolve — the behavior Pi-hole's dnsmasq gave us before.

Reads the rendered forward zone (db.akoria.net), resolves A + CNAME->A, and
writes db.bare-<name> + named.conf.bare into OUTDIR. Zone file path in the
config is fixed at /etc/bind/zones/ (same on xenon and inside steel's container).
Usage: gen-bare.py <src db.akoria.net> <outdir>
"""
import re, sys, os
src, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
A, C = {}, {}
for line in open(src):
    m = re.match(r'^(\S+)\s+IN\s+A\s+(\d+\.\d+\.\d+\.\d+)', line)
    if m: A[m.group(1).lower()] = m.group(2)
    m = re.match(r'^(\S+)\s+IN\s+CNAME\s+(\S+)', line)
    if m: C[m.group(1).lower()] = m.group(2).rstrip('.').split('.')[0].lower()
names = {}
names.update(A)
for alias, tgt in C.items():
    if tgt in A: names[alias] = A[tgt]          # resolve CNAME -> target IP
SOA = ("$TTL 300\n@ IN SOA xenon.akoria.net. hostmaster.akoria.net. "
       "( 2026071001 3600 600 604800 300 )\n@ IN NS xenon.akoria.net.\n")
stanzas = []
for name in sorted(names):
    if name in ('@', '*'): continue
    open(os.path.join(outdir, f"db.bare-{name}"), "w").write(SOA + f"@ IN A {names[name]}\n")
    stanzas.append(f'zone "{name}" {{ type master; file "/etc/bind/zones/db.bare-{name}"; }};')
open(os.path.join(outdir, "named.conf.bare"), "w").write(
    "// single-label bare-name zones (NetBIOS SMB clients) — see deploy/dns-bare/\n"
    + "\n".join(stanzas) + "\n")
print(f"bare zones: {len(stanzas)} names ({len(A)} A + {len(names)-len(A)} CNAME-derived)")
