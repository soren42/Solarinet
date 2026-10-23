# solariPanel daemon

`solariPanel` holds an in-memory HTTPS session, polls `/api/panel`, preserves the
last valid snapshot through API outages, and sends framed snapshots to the
Galactic Unicorn USB CDC device. Build with `make`; run the portable codec test
with `make test`.

Install the binary as `/usr/local/bin/solariPanel`, configuration as
`/etc/solari-panel/solari-panel.conf`, password as a `0600` file, the dashboard
CA as `/etc/solari-panel/ca.pem`, and the service unit in systemd. The default
serial pattern is `/dev/serial/by-id/*`, avoiding unstable ttyACM numbering.

The daemon vendors upstream cJSON **v1.7.19** (`vendor/cJSON.c` and
`vendor/cJSON.h`); the Makefile compiles those files directly. This keeps the
daemon build independent of a distribution-provided cJSON development package.
