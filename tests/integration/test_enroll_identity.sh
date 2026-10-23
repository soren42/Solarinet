#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

bash "$repo_root/deploy/enrollment/solari-enroll.sh" \
  --role monitor \
  --fqdn beryl.akoria.net \
  --csr-only \
  --conf-dir "$tmp_dir"

subject="$(openssl req -in "$tmp_dir/node.csr" -noout -subject -nameopt RFC2253)"
extensions="$(openssl req -in "$tmp_dir/node.csr" -noout -text | sed -n '/X509v3 Subject Alternative Name/{n;p;}')"
grep -Fq 'CN=monitor.beryl.akoria.net' <<<"$subject" || {
  echo "enrollment CSR CN does not use the server's role.fqdn contract: $subject" >&2
  exit 1
}
grep -Fq 'DNS:beryl.akoria.net' <<<"$extensions" || {
  echo "enrollment CSR is missing the enrolled DNS SAN: $extensions" >&2
  exit 1
}
if grep -Fq 'CN=monitor:beryl.akoria.net' <<<"$subject"; then
  echo "enrollment CSR still uses the rejected role:fqdn form" >&2
  exit 1
fi
echo "enrollment CSR identity contract passed"
