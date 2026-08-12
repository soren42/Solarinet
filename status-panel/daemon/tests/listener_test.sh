#!/bin/sh
set -eu
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/solari-panel-listener.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
umask 077

openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=test-root -keyout "$test_dir/root.key" -out "$test_dir/root.pem" >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=bad-root -keyout "$test_dir/badroot.key" -out "$test_dir/badroot.pem" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout "$test_dir/server.key" -out "$test_dir/server.csr" >/dev/null 2>&1
printf '%s\n' 'subjectAltName=DNS:localhost' 'extendedKeyUsage=serverAuth' >"$test_dir/server.ext"
openssl x509 -req -in "$test_dir/server.csr" -CA "$test_dir/root.pem" -CAkey "$test_dir/root.key" -set_serial 10 -days 2 -extfile "$test_dir/server.ext" -out "$test_dir/server.pem" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -subj '/CN=Panel Display Name' -keyout "$test_dir/client.key" -out "$test_dir/client.csr" >/dev/null 2>&1
printf '%s\n' 'subjectAltName=DNS:panel-01.panel.akoria.net' 'extendedKeyUsage=clientAuth' >"$test_dir/good.ext"
printf '%s\n' 'subjectAltName=DNS:wrong.panel.akoria.net' 'extendedKeyUsage=clientAuth' >"$test_dir/wrongsan.ext"
printf '%s\n' 'subjectAltName=DNS:panel-01.panel.akoria.net' 'extendedKeyUsage=serverAuth' >"$test_dir/wrongeku.ext"
openssl x509 -req -in "$test_dir/client.csr" -CA "$test_dir/root.pem" -CAkey "$test_dir/root.key" -set_serial 100 -days 2 -extfile "$test_dir/good.ext" -out "$test_dir/good.pem" >/dev/null 2>&1
openssl x509 -req -in "$test_dir/client.csr" -CA "$test_dir/root.pem" -CAkey "$test_dir/root.key" -set_serial 101 -days 2 -extfile "$test_dir/wrongsan.ext" -out "$test_dir/wrongsan.pem" >/dev/null 2>&1
openssl x509 -req -in "$test_dir/client.csr" -CA "$test_dir/root.pem" -CAkey "$test_dir/root.key" -set_serial 102 -days 2 -extfile "$test_dir/wrongeku.ext" -out "$test_dir/wrongeku.pem" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -subj /CN=wrong-ca -keyout "$test_dir/badclient.key" -out "$test_dir/bad.csr" >/dev/null 2>&1
openssl x509 -req -in "$test_dir/bad.csr" -CA "$test_dir/badroot.pem" -CAkey "$test_dir/badroot.key" -set_serial 103 -days 2 -extfile "$test_dir/good.ext" -out "$test_dir/bad.pem" >/dev/null 2>&1
touch "$test_dir/index.txt"
printf '1000\n' >"$test_dir/serial"
mkdir "$test_dir/newcerts"
printf '%s\n' '[ca]' 'default_ca=issuer' '[issuer]' "database=$test_dir/index.txt" "new_certs_dir=$test_dir/newcerts" "certificate=$test_dir/root.pem" "private_key=$test_dir/root.key" "serial=$test_dir/serial" 'default_md=sha256' 'policy=policy' '[policy]' 'commonName=supplied' >"$test_dir/ca.conf"
openssl ca -batch -config "$test_dir/ca.conf" -in "$test_dir/client.csr" -startdate 20200101000000Z -enddate 20200102000000Z -extfile "$test_dir/good.ext" -out "$test_dir/expired.pem" >/dev/null 2>&1
printf '64\n' >"$test_dir/denylist"
chmod 600 "$test_dir"/*.key
chmod 644 "$test_dir"/*.pem
./tests/listener_test "$test_dir"
