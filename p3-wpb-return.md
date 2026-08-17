# P3 WP-B return — firmware WiFi device glue

## Design decisions

- **Poll mode:** linked `pico_cyw43_arch_lwip_poll`. The firmware already has a
  single 40 ms control/render tick, so `cyw43_arch_poll()` once per tick keeps
  all raw-lwIP callbacks on that thread and avoids IRQ/thread synchronization.
  Join, DHCP, SNTP, DNS, and TLS are initiated asynchronously; their device
  adapter timeouts are bounded at 15 seconds and completion is fed back to the
  frozen `PanelNetFsm`.
- **TLS:** lwIP `altcp_tls` uses the stored root CA plus client private key and
  leaf/intermediate chain. Because lwIP's 2.2 helper parses only one DER object
  but accepts multiple PEM certificates, the bounded concatenated client DER
  chain is converted into a 6144-byte static PEM scratch buffer, passed into
  the TLS config, then zeroized. `mbedtls_ssl_set_hostname()` supplies SNI and
  hostname verification. `MBEDTLS_SSL_VERIFY_REQUIRED` enforces the chain.
- **SAN and EKU:** the post-handshake callback runs before `LINKED`. It rejects
  nonzero verify flags, requires an exact case-insensitive DNS SAN equal to the
  provisioned `serverHost` (therefore refusing mbedTLS 2.x CN fallback), and
  walks the peer leaf's EKU sequence for `MBEDTLS_OID_SERVER_AUTH`. Failure
  aborts synchronously and enters the FSM's bounded backoff.
- **Time:** SNTP uses item 10 when present and `serverHost` otherwise. Its result
  must be later than `SOLARI_BUILD_EPOCH`, starts/updates RP2350 AON time, and is
  fed to the FSM. mbedTLS's platform time callback reads that AON clock, so
  certificate validity checks and the FSM use the same powered-lifetime time.
  There is no bypass.
- **RX:** TLS pbuf payloads feed a dedicated `PanelParser` with the same
  `onFrame` callback as USB. The callback tags transport origin, routes WiFi
  snapshots through `panelLinkAcceptSnapshot(..., PANEL_LINK_WIFI, ...)`, and
  sends WiFi PROVISION frames to `panelProvHandle(..., onWifi=true, ...)`.
- **TX mux:** encoded panel frames use TLS only while the FSM is `LINKED` and
  the TLS PCB is up; every other state writes USB stdout. It never writes both
  transports for one frame.
- **Arbitration:** every CRC-valid USB callback classifies the five D14 known
  host frame types. The frozen FSM immediately closes WiFi on a qualifying
  frame. New TLS connections switch/reset WiFi sequence state; all non-LINKED
  states select serial. TinyUSB CDC disconnect is debounced for 100 ms before
  bypassing the 15-second silence delay.
- **Crypto seam:** device callbacks use mbedTLS 2.x
  `mbedtls_x509_crt_parse_der`, five-argument `mbedtls_pk_parse_key`, and
  `mbedtls_pk_check_pair`. Concatenated client certificates are validated one
  DER object at a time; the leaf is used for key matching. Private key contexts
  are freed (and therefore zeroized) on every path. No credential material is
  logged.

## Files touched

- `status-panel/firmware/panelNet.c` — device adapter, pure helpers, TLS/SNTP,
  provisioning crypto implementation, RX/TX, synchronous close.
- `status-panel/firmware/panelNet.h` — SDK-free public seam.
- `status-panel/firmware/main.c` — FSM instantiation, parser/transport routing,
  TX mux, crypto seam, CDC debounce, provisioning state updates, tick polling.
- `status-panel/firmware/lwipopts.h` — poll/raw API, DHCP/DNS/SNTP/altcp TLS,
  bounded pools/windows, required TLS authentication.
- `status-panel/firmware/mbedtls_config.h` — mbedTLS 2.28 TLS 1.2 client/X.509,
  AEAD-only profile, AON platform time, Pico hardware entropy, buffer bounds.
- `status-panel/firmware/CMakeLists.txt` — device source/define and Pico network,
  SNTP, AON, and mbedTLS libraries. Board selection, build epoch, and image guard
  remain unchanged.
- `status-panel/firmware/test/panelNetTest.c` — platform-free contract helpers.
- `status-panel/firmware/test/Makefile` — adds the helper suite.

## Resource/config rationale

- lwIP heap is 24 KiB; pool is 12 x 1536-byte pbufs; TCP PCB/segment/altcp
  counts are 4/16/4; TCP RX/TX windows are 5840 bytes. This supports one live
  TLS client plus DNS/SNTP while bounding reconnect allocation pressure.
- mbedTLS record content is capped at 4096 bytes and MPI objects at 512 bytes.
  TLS 1.0/1.1, server mode, filesystem/network helpers, session tickets/cache,
  write-side X.509/PEM, CRL/CSR parsing, and CBC are disabled. TLS 1.2 AEAD,
  client X.509, PEM parsing for the bounded client chain, Pico hardware entropy,
  hostname/SNI, and time validation remain enabled.
- Application-owned network storage is static. The only scratch containing a
  transformed credential is the bounded client-cert PEM buffer, which is
  zeroized immediately after TLS config creation. mbedTLS/lwIP internal dynamic
  allocation remains bounded by the above configuration.

## Verification

- `make -C status-panel/firmware/test`: **PASS**, 10 test binaries.
- New `panelNetTest`: **15 assertions passed** under C11
  `-Wall -Wextra -Werror`.
- Existing sanitizer/state/render suites: **PASS**; framebuffer parity reports
  **107 passed, 0 failed**.
- `git diff --check`: **PASS**.

## UNVERIFIED

- Pico SDK 2.1.1 cross-configure, compile, link, UF2 size, and two-build binary
  reproducibility were not run, per assignment; the caller must run them on the
  separate Pico build host.
- All device-only API integration remains unexercised here: unconditional CYW43
  initialization, asynchronous join/DHCP/DNS, SNTP-to-AON time, lwIP raw
  callbacks, TinyUSB CDC disconnect behavior, and 40 ms poll/tick timing.
- Hardware mTLS against the real chlorine chain remains unverified, including
  full client-chain presentation, SNI/SAN hostname rejection, serverAuth EKU
  rejection, expired/not-yet-valid certificates, server restart, and TLS write
  backpressure.
- AP-loss/reconnect fault matrix, wrong PSK, DHCP/DNS/SNTP timeouts, synchronous
  abort behavior, malformed store recovery, RAM/stack high-water, one-hour AP
  bounce soak, and secret-log grep audit remain bench acceptance work.
- RP2350 AON retention across powered USB/WiFi transitions and CDC's exact
  TinyUSB line-state behavior remain hardware checks.
