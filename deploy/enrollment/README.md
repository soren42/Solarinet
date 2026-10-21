# SolariNet node enrollment

Enrollment uses an operator-created, 256-bit bearer token. The server stores
only its SHA-256 digest, binds it to one host and role, and expires unsubmitted
tokens after 15 minutes by default. Operators deliver the token to the target
host through a trusted channel.

## Issue a token

Run as an OS account allowed to use the `solariCtl` operator socket:

```sh
./build/solariCtlClient --sock /run/solari/solariCtl.sock enroll-token \
  --op jason --role client --host node.akoria.net --ip 10.0.0.50
```

The command prints the token once, plus its enrollment ID and expiry time. Keep
the output private and deliver the token out of band. Optional `--ttl SEC`
accepts 60–86400 seconds; the default is 900. Issuing the token requires a named
operator and runs directly through the operator socket. It cannot be submitted
to the dashboard's privileged request queue.

## Enroll the node

On the target, run:

```sh
sudo deploy/enrollment/solari-enroll.sh \
  --role client --fqdn node.akoria.net --token '<token>'
```

The script keeps the private key on the target and submits its CSR over HTTPS to
`POST /api/enrollments`. The C service validates the token digest, expiry,
prebound host and role, then stores the CSR as pending. An operator approves the
pending request in the dashboard. The target can then rerun the same command
with the same token and key; the server returns the signed certificate and CA
certificate until the target acknowledges installation at
`POST /api/enrollments/ack`.

Before installation, the script verifies both PEM files, verifies the leaf
against the returned CA, and confirms the certificate public key matches the
local private key. It installs the CA first and the leaf certificate last, then
acknowledges the token so the server revokes it. If validation fails, installed
files remain untouched. A forced key replacement is staged and committed only
with a matching certificate; failed forced enrollment retains the prior key,
leaf, and CA. If installation succeeded but acknowledgement failed,
retry with `--resume` and the same token. Resume verifies the installed leaf
against its CA and private key, then requires the server to return the identical
leaf before retrying acknowledgement; it never replaces the installed identity.
`--insecure` disables HTTPS certificate verification and is only for controlled
bootstrap troubleshooting.

## Verification and current limits

The local installer suite covers valid pickup, untrusted issuers, key mismatch,
successful forced renewal, failed forced renewal preserving the complete prior
identity, and acknowledgement recovery. Token submission, expiry, pickup, and
acknowledgement are covered separately against a disposable MariaDB schema.
`tests/dashboard/test_enrollments_http.py` exercises HTTPS → the real PHP front
controller/routes → the real `SolariCtl` framing/parser → a temporary
Unix-socket protocol fixture. It covers validation, pending and approved reply
shapes, incomplete certificate material, and acknowledgement.
`tests/integration/test_enrollment_e2e.py` exercises the real TLS-enabled C
server, its Unix control socket, PHP API, migrated disposable MariaDB schema,
and temporary CA together. It verifies HTTPS token submission, pending-to-
approved transition, signed certificate pickup and chain validation, leaf
fingerprint/serial persistence, acknowledgement, and token/CSR cleanup. The
full-server CI job runs this integration test; a local run passed on 2026-10-20
against an isolated MariaDB 11.8 instance. Production-host enrollment and
cross-lab security review remain unverified. Enrollment-token pickup uses the
same bearer until installation acknowledgement; protect it as a credential
throughout that interval.
