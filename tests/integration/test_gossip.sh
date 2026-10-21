#!/usr/bin/env bash
set -euo pipefail
test_bin=${1:?usage: test_gossip.sh /path/to/test_gossip}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj "/CN=SolariNet Gossip Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -keyout "$tmp/ca.key" -out "$tmp/ca.pem" >/dev/null 2>&1
mkdir -p "$tmp/newcerts"
touch "$tmp/index.txt"
printf '1000\n' > "$tmp/serial"
printf '1000\n' > "$tmp/crlnumber"
cat > "$tmp/ca.cnf" <<EOF
[ ca ]
default_ca = issuer
[ issuer ]
dir = $tmp
database = \$dir/index.txt
new_certs_dir = \$dir/newcerts
certificate = \$dir/ca.pem
private_key = \$dir/ca.key
serial = \$dir/serial
crlnumber = \$dir/crlnumber
default_md = sha256
default_days = 1
default_crl_days = 1
policy = policy
x509_extensions = leaf
unique_subject = no
[ policy ]
commonName = supplied
[ leaf ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth,clientAuth
subjectAltName = IP:127.0.0.1,DNS:localhost
EOF
for name in a b revoked; do
  openssl req -new -newkey rsa:2048 -nodes \
    -subj "/CN=monitor.${name}.gossip.test" \
    -keyout "$tmp/${name}.key" -out "$tmp/${name}.csr" >/dev/null 2>&1
  openssl ca -batch -config "$tmp/ca.cnf" -in "$tmp/${name}.csr" \
    -out "$tmp/${name}.pem" >/dev/null 2>&1
done
# Renew the revoked monitor identity with a different key but the same CN.
# Only the original serial is revoked; the replacement must stay valid.
openssl req -new -newkey rsa:2048 -nodes \
  -subj "/CN=monitor.revoked.gossip.test" \
  -keyout "$tmp/replacement.key" -out "$tmp/replacement.csr" >/dev/null 2>&1
openssl ca -batch -config "$tmp/ca.cnf" -in "$tmp/replacement.csr" \
  -out "$tmp/replacement.pem" >/dev/null 2>&1
old_subject=$(openssl x509 -in "$tmp/revoked.pem" -noout -subject -nameopt RFC2253)
replacement_subject=$(openssl x509 -in "$tmp/replacement.pem" -noout -subject -nameopt RFC2253)
old_serial=$(openssl x509 -in "$tmp/revoked.pem" -noout -serial)
replacement_serial=$(openssl x509 -in "$tmp/replacement.pem" -noout -serial)
[[ "$old_subject" == "$replacement_subject" ]]
[[ "$old_serial" != "$replacement_serial" ]]
openssl ca -batch -config "$tmp/ca.cnf" -revoke "$tmp/revoked.pem" >/dev/null 2>&1
openssl ca -batch -config "$tmp/ca.cnf" -gencrl -out "$tmp/crl.pem" >/dev/null 2>&1
cat "$tmp/ca.pem" "$tmp/crl.pem" > "$tmp/trust.pem"
"$test_bin" "$tmp/trust.pem" "$tmp/a.pem" "$tmp/a.key" \
  "$tmp/b.pem" "$tmp/b.key" "$tmp/revoked.pem" "$tmp/revoked.key" \
  "$tmp/replacement.pem" "$tmp/replacement.key"

# The pinned NNG/mbedTLS stack must fail closed when a present CRL is stale or
# malformed. A successful handshake with the otherwise valid A leaf is a test
# failure; rejection during trust-file setup or peer verification is accepted.
openssl ca -batch -config "$tmp/ca.cnf" -gencrl \
  -crl_lastupdate 200101000000Z -crl_nextupdate 200102000000Z \
  -out "$tmp/expired-crl.pem" >/dev/null 2>&1
cat "$tmp/ca.pem" "$tmp/expired-crl.pem" > "$tmp/expired-trust.pem"
"$test_bin" "$tmp/expired-trust.pem" "$tmp/a.pem" "$tmp/a.key" \
  "$tmp/b.pem" "$tmp/b.key" "$tmp/revoked.pem" "$tmp/revoked.key" \
  "$tmp/replacement.pem" "$tmp/replacement.key" \
  expect-invalid-crl

printf '%s\n' '-----BEGIN X509 CRL-----' 'this-is-not-base64' \
  '-----END X509 CRL-----' > "$tmp/malformed-crl.pem"
cat "$tmp/ca.pem" "$tmp/malformed-crl.pem" > "$tmp/malformed-trust.pem"
"$test_bin" "$tmp/malformed-trust.pem" "$tmp/a.pem" "$tmp/a.key" \
  "$tmp/b.pem" "$tmp/b.key" "$tmp/revoked.pem" "$tmp/revoked.key" \
  "$tmp/replacement.pem" "$tmp/replacement.key" \
  expect-invalid-crl

# A CRL from another issuer must not disrupt credentials issued by this CA.
mkdir -p "$tmp/other/newcerts"
touch "$tmp/other/index.txt"
printf '2000\n' > "$tmp/other/serial"
printf '2000\n' > "$tmp/other/crlnumber"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj "/CN=Unrelated Gossip Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -keyout "$tmp/other/ca.key" -out "$tmp/other/ca.pem" >/dev/null 2>&1
cat > "$tmp/other/ca.cnf" <<EOF
[ ca ]
default_ca = issuer
[ issuer ]
dir = $tmp/other
database = \$dir/index.txt
new_certs_dir = \$dir/newcerts
certificate = \$dir/ca.pem
private_key = \$dir/ca.key
serial = \$dir/serial
crlnumber = \$dir/crlnumber
default_md = sha256
default_crl_days = 1
policy = policy
[ policy ]
commonName = supplied
EOF
openssl ca -batch -config "$tmp/other/ca.cnf" -gencrl \
  -out "$tmp/other/crl.pem" >/dev/null 2>&1
cat "$tmp/ca.pem" "$tmp/other/crl.pem" > "$tmp/wrong-issuer-trust.pem"
"$test_bin" "$tmp/wrong-issuer-trust.pem" "$tmp/a.pem" "$tmp/a.key" \
  "$tmp/b.pem" "$tmp/b.key" "$tmp/revoked.pem" "$tmp/revoked.key" \
  "$tmp/replacement.pem" "$tmp/replacement.key" \
  wrong-issuer
