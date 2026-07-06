#!/usr/bin/env python3
"""Hourly akoria.net -> akoria.org host-record reconciler (runs on radium/DC).
AXFRs the authoritative .net zone from xenon (ns1) and ensures each host A record
also exists in AD-managed akoria.org (samba-tool). Idempotent. Deployed at
/usr/local/sbin/ + an hourly systemd timer on radium. Future: source .net from
the central MySQL SoT instead of AXFR (same downstream)."""
import subprocess, re
XENON="10.0.0.20"; DC="localhost"; NET="akoria.net"; ORG="akoria.org"
U="administrator%"+open('/root/.ad-admin-pw').read().strip()
def sh(*a): return subprocess.run(a,capture_output=True,text=True,timeout=30).stdout
def axfr():
    r={}
    for ln in sh('dig','+noall','+answer','@'+XENON,NET,'AXFR').splitlines():
        p=ln.split()
        if len(p)>=5 and p[3]=='A':
            nm=p[0].rstrip('.'); s=nm[:-len(NET)-1] if nm.endswith('.'+NET) else nm
            if s and s!='@': r[s]=p[4]
    return r
def org_ip(n):
    m=re.search(r'A:\s+([\d.]+)', sh('samba-tool','dns','query',DC,ORG,n,'A','-U',U)); return m.group(1) if m else None
c=0
for s,ip in axfr().items():
    cur=org_ip(s)
    if cur==ip: continue
    (sh('samba-tool','dns','update',DC,ORG,s,'A',cur,ip,'-U',U) if cur else sh('samba-tool','dns','add',DC,ORG,s,'A',ip,'-U',U))
    print(f"  {s}.{ORG} -> {ip}"); c+=1
print(f"reconciled {c} record(s)")
