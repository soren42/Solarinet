# solariPanel daemon

`solariPanel` holds an in-memory HTTPS session, polls `/api/panel`, preserves the
last valid snapshot through API outages, and sends framed snapshots to the
Galactic Unicorn USB CDC device. Build with `make`; run the portable codec test
with `make test`.

Install the binary as `/usr/local/bin/solariPanel`, configuration as
`/etc/solari-panel/solari-panel.conf`, password as a `0600` file, the dashboard
CA as `/etc/solari-panel/ca.pem`, and the service unit in systemd. The default
serial pattern is `/dev/serial/by-id/*`, avoiding unstable ttyACM numbering.

The intended vendored dependency is upstream cJSON **v1.7.19** (`cJSON.c` and
`cJSON.h`). This sandbox could not resolve `raw.githubusercontent.com`; its
`vendor/cJSON.h` is therefore a deliberately documented shim to Debian's
`libcjson-dev`, and the Makefile links `-lcjson`. Before deployment replace the
shim with those two upstream v1.7.19 files and change the Makefile to compile
`vendor/cJSON.c` instead of linking the system library.
