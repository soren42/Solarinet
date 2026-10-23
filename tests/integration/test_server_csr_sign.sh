#!/usr/bin/env bash
set -euo pipefail

signer="${1:?path to test_server_csr_sign required}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

openssl ecparam -name prime256v1 -genkey -noout -out "$tmp_dir/ca.key"
openssl req -x509 -new -sha256 -key "$tmp_dir/ca.key" -days 2 \
  -subj '/CN=SolariNet CSR Test CA' \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,keyCertSign,cRLSign' \
  -out "$tmp_dir/ca.pem"

bash "$repo_root/deploy/enrollment/solari-enroll.sh" \
  --role monitor --fqdn beryl.akoria.net --csr-only --conf-dir "$tmp_dir/node"

metadata="$("$signer" "$tmp_dir/ca.pem" "$tmp_dir/ca.key" "$tmp_dir/node/node.csr" \
  monitor.beryl.akoria.net beryl.akoria.net "$tmp_dir/leaf.pem" "$tmp_dir/sign.sock")"
openssl verify -CAfile "$tmp_dir/ca.pem" "$tmp_dir/leaf.pem"
actual_fp="$(openssl x509 -in "$tmp_dir/leaf.pem" -outform DER | openssl dgst -sha256 | awk '{print $NF}')"
actual_serial="$(openssl x509 -in "$tmp_dir/leaf.pem" -noout -serial | cut -d= -f2)"
grep -Fq "fp=${actual_fp}" <<<"$metadata"
actual_ca_fp="$(openssl x509 -in "$tmp_dir/ca.pem" -outform DER | openssl dgst -sha256 | awk '{print $NF}')"
grep -Fq "caFp=${actual_ca_fp}" <<<"$metadata"
grep -Fqi "serial=${actual_serial}" <<<"$metadata"
subject="$(openssl x509 -in "$tmp_dir/leaf.pem" -noout -subject -nameopt RFC2253)"
san="$(openssl x509 -in "$tmp_dir/leaf.pem" -noout -ext subjectAltName)"
grep -Fq 'CN=monitor.beryl.akoria.net' <<<"$subject"
grep -Fq 'DNS:beryl.akoria.net' <<<"$san"

# Reissuing the same CSR must produce a distinct certificate serial.
"$signer" "$tmp_dir/ca.pem" "$tmp_dir/ca.key" "$tmp_dir/node/node.csr" \
  monitor.beryl.akoria.net beryl.akoria.net "$tmp_dir/leaf-new.pem" \
  "$tmp_dir/sign-new.sock"
serial_old="$(openssl x509 -in "$tmp_dir/leaf.pem" -noout -serial)"
serial_new="$(openssl x509 -in "$tmp_dir/leaf-new.pem" -noout -serial)"
test "$serial_old" != "$serial_new"
openssl verify -CAfile "$tmp_dir/ca.pem" "$tmp_dir/leaf-new.pem"

if "$signer" "$tmp_dir/ca.pem" "$tmp_dir/ca.key" "$tmp_dir/node/node.csr" \
  client.beryl.akoria.net beryl.akoria.net "$tmp_dir/wrong-cn.pem" \
  "$tmp_dir/cn.sock"; then
  echo 'enrollment signer accepted a CSR with the wrong CN' >&2
  exit 1
fi
test ! -e "$tmp_dir/wrong-cn.pem"

if "$signer" "$tmp_dir/ca.pem" "$tmp_dir/ca.key" "$tmp_dir/node/node.csr" \
  monitor.beryl.akoria.net other.akoria.net "$tmp_dir/wrong-san.pem" \
  "$tmp_dir/san.sock"; then
  echo 'enrollment signer accepted a CSR with the wrong DNS SAN' >&2
  exit 1
fi
test ! -e "$tmp_dir/wrong-san.pem"

echo 'enrollment certificate identity, unique serials, and SAN preservation passed'
