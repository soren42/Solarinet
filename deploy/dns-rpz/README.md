# Akoria response-policy zone

`gen-rpz.sh` builds the RPZ data on Xenon. The primary zone declaration lives
in `named.conf.rpz`; include that file once from Xenon's `named.conf.local`.
The primary authorizes the reserved ZimaBlade address (`10.0.0.11`) and the
Khadas VIM4 (`10.7.0.10`) for zone transfer and NOTIFY. The same declaration
is retained in `deploy/dns-release/xenon.json.example` for the managed release
path.

Khadas enables `response-policy { zone "rpz.akoria"; };` in its isolated BIND
options. Install and validate the primary ACL before refreshing the Khadas zone
list; an authoritative SOA on the primary does not prove AXFR is allowed.
Verify SOA serial parity on Xenon and Khadas after the transfer. Steel is the
inventory-labeled ZimaBlade; it remains offline until console redeployment.
