#!/bin/bash
# Enable the PHP->host ctl boundary on xenon (Task #1 / F-02):
#   - php-fpm pool `solarinet` runs as www-solari (was jason)
#   - ctl socket moves to /run/solari (0751) as 0660 jason:solari-ctl
#   - server.conf [ctl] enforcePeer=true dashboardUid=www-solari socketGid=solari-ctl
#   - dashboard DB user solari_dash: per-table DML, NO ctlRequest
# Server stays User=jason (operator uid). Run as root. Rollback: rollback-boundary.sh
set -euo pipefail
umask 077
STAMP=$(date +%Y%m%d-%H%M%S)
BK=/root/boundary-backup-$STAMP
RUN=/home/jason/Code/Solarinet/run
POOL=/etc/php/8.5/fpm/pool.d/solarinet.conf
DASH=/var/lib/solarinet-dashboard
mkdir -p "$BK"
cp -a "$POOL" "$RUN/server.conf" "$BK/"
echo "$BK" > /root/boundary-backup-latest

# 1. identities
install -d -m 0755 /etc/sysusers.d /etc/tmpfiles.d
cat > /etc/sysusers.d/solarinet-dashboard.conf <<'EOF'
u      www-solari  -      "SolariNet dashboard (php-fpm)"    /nonexistent    /usr/sbin/nologin
g      solari-ctl  -      -
m      www-solari  solari-ctl
EOF
systemd-sysusers /etc/sysusers.d/solarinet-dashboard.conf
id www-solari

# 2. dashboard state (copy; originals left in place for rollback)
install -d -m 0750 -o www-solari -g www-solari "$DASH"
install -d -m 0700 -o www-solari -g www-solari "$DASH/sessions" "$DASH/auth-rate"
install -d -m 0750 -o www-solari -g www-solari "$DASH/uploads"
chown -R www-solari:www-solari "$DASH/uploads"
cp -a "$RUN/sessions/." "$DASH/sessions/"
cp -a "$RUN/auth-rate/." "$DASH/auth-rate/" 2>/dev/null || true
install -m 0600 -o www-solari -g www-solari /home/jason/Code/Solarinet/solari-auth.json "$DASH/solari-auth.json"
install -m 0644 -o root -g root "$RUN/pki/ca.pem" "$DASH/ca.pem"
chown -R www-solari:www-solari "$DASH/sessions" "$DASH/auth-rate"
cat > /etc/tmpfiles.d/solarinet-dashboard.conf <<'EOF'
d /var/lib/solarinet-dashboard          0750 www-solari www-solari - -
d /var/lib/solarinet-dashboard/sessions 0700 www-solari www-solari - -
d /var/lib/solarinet-dashboard/auth-rate 0700 www-solari www-solari - -
d /var/lib/solarinet-dashboard/uploads  0750 www-solari www-solari - -
z /var/lib/solarinet-dashboard/solari-auth.json 0600 www-solari www-solari - -
EOF

# 3. least-privilege DB user (no ctlRequest)
install -d -m 0700 /etc/solarinet
if [ ! -s /etc/solarinet/dashboard-db.pass ]; then
  openssl rand -base64 33 | tr -d '/+=\n' | head -c 40 > /etc/solarinet/dashboard-db.pass
  chmod 0600 /etc/solarinet/dashboard-db.pass
fi
python3 - <<'EOF'
import subprocess
pw=open('/etc/solarinet/dashboard-db.pass').read().strip()
q=lambda s: subprocess.run(['mariadb','-N','-e',s],check=True,capture_output=True,text=True).stdout
sql=["CREATE USER IF NOT EXISTS 'solari_dash'@'127.0.0.1' IDENTIFIED BY '%s'"%pw,
     "ALTER USER 'solari_dash'@'127.0.0.1' IDENTIFIED BY '%s'"%pw]
for t,ty in [l.split('\t') for l in q("SELECT table_name,table_type FROM information_schema.tables WHERE table_schema='solarinet'").split('\n') if l]:
    if t=='ctlRequest': continue
    priv='SELECT' if ty=='VIEW' else 'SELECT, INSERT, UPDATE, DELETE'
    sql.append("GRANT %s ON solarinet.`%s` TO 'solari_dash'@'127.0.0.1'"%(priv,t))
p=subprocess.run(['mariadb'],input=';\n'.join(sql)+';\nFLUSH PRIVILEGES;\n',text=True,capture_output=True)
if p.returncode: raise SystemExit('grant failed: '+p.stderr.replace(pw,'***'))
leak=q("SELECT COUNT(*) FROM information_schema.table_privileges WHERE grantee LIKE \"'solari_dash'@%\" AND table_schema='solarinet' AND table_name='ctlRequest'").strip()
if leak!='0': raise SystemExit('solari_dash has ctlRequest privileges')
print('solari_dash grants ok (%d statements, ctlRequest excluded)'%(len(sql)-2))
EOF

# 4. server: socket under /run/solari, [ctl] enforcement
install -d -m 0755 /etc/systemd/system/solarinet-server.service.d
cat > /etc/systemd/system/solarinet-server.service.d/20-ctl-boundary.conf <<'EOF'
[Service]
SupplementaryGroups=solari-ctl
RuntimeDirectory=solari
RuntimeDirectoryMode=0751
EOF
python3 - "$RUN/server.conf" <<'EOF'
import re,sys
p=sys.argv[1]; s=open(p).read()
s=re.sub(r'(?m)^ctlSocket\s*=.*$','ctlSocket  = /run/solari/solariCtl.sock',s)
s=re.sub(r'(?ms)^\[ctl\].*?(?=^\[|\Z)','',s)
s=s.rstrip('\n')+'\n\n[ctl]\nenforcePeer  = true\ndashboardUid = www-solari\nsocketGid    = solari-ctl\n'
open(p,'w').write(s)
EOF

# 5. php-fpm pool as www-solari
python3 - "$POOL" <<'EOF'
import re,sys
p=sys.argv[1]; s=open(p).read()
pw=open('/etc/solarinet/dashboard-db.pass').read().strip()
D='/var/lib/solarinet-dashboard'
rep={r'^user\s*=.*$':'user = www-solari', r'^group\s*=.*$':'group = www-solari',
     r'^env\[SOLARI_CTL_SOCK\]\s*=.*$':'env[SOLARI_CTL_SOCK] = /run/solari/solariCtl.sock',
     r'^env\[SOLARI_AUTH_FILE\]\s*=.*$':'env[SOLARI_AUTH_FILE] = %s/solari-auth.json'%D,
     r'^php_admin_value\[session\.save_path\]\s*=.*$':'php_admin_value[session.save_path] = %s/sessions'%D,
     r'^env\[SOLARINET_CA_PEM\]\s*=.*$':'env[SOLARINET_CA_PEM] = %s/ca.pem'%D,
     r'^env\[SOLARI_AUTH_RATE_DIR\]\s*=.*$':'env[SOLARI_AUTH_RATE_DIR] = %s/auth-rate'%D,
     r'^env\[SOLARI_DB_USER\]\s*=.*$':'env[SOLARI_DB_USER] = solari_dash',
     r'^env\[SOLARI_DB_PASS\]\s*=.*$':'env[SOLARI_DB_PASS] = '+pw}
for k,v in rep.items():
    s,n=re.subn(k,lambda m:v,s,flags=re.M)
    if n!=1: raise SystemExit('pool: pattern %s matched %d'%(k.split('=')[0],n))
if 'SOLARI_UPLOAD_DIR' not in s: s=s.rstrip('\n')+'\nenv[SOLARI_UPLOAD_DIR] = %s/uploads\n'%D
open(p,'w').write(s)
EOF
php-fpm8.5 -t 2>&1 | tail -1

# 6. activate
systemctl daemon-reload
systemctl restart solarinet-server
for i in $(seq 1 20); do [ -S /run/solari/solariCtl.sock ] && break; sleep 0.5; done
stat -c '%a %U:%G %n' /run/solari /run/solari/solariCtl.sock
systemctl reload php8.5-fpm
sleep 2
echo "cutover applied; backup in $BK"
