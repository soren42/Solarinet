# PHP critical security update disposition

**Verified:** 2026-10-23 UTC  
**Scope:** PHP runtimes on SolariNet infrastructure hosts, associated containers, and common remotely exposed web/FastCGI ports.

## Advisory and fixed versions

The PHP project released security fixes on 2026-10-21 for CVE-2026-109618 and CVE-2026-109619, critical unauthenticated remote-code-execution vulnerabilities affecting PHP installations that serve web requests. Upstream fixed releases are PHP 8.5.12, 8.4.27, 8.3.36, and 8.2.35. Ubuntu 26.04 backports the PHP 8.5 fix as `8.5.4-0ubuntu1.5`.

Sources: [PHP security release](https://www.php.net/?=PHPE9568F35-D428-11d2-A769-00AA001ACF42), [PHP 8.5.12 announcement](https://news-web.php.net/php.announce/513), and [Ubuntu USN-8948-1](https://ubuntu.com/security/notices/USN-8948-1).

## Observed runtimes and disposition

| Host | Observed state | Disposition |
|---|---|---|
| Xenon | Ubuntu 26.04 packages `php8.5-cli`, `php8.5-fpm`, `php8.5-common`, and `php8.5-mysql` are all `8.5.4-0ubuntu1.5`. PHP-FPM is active; its master process started 2026-10-22 23:49 UTC, after the package installation recorded on 2026-10-22. | Canonical fixed backport is installed and loaded. No further package change or restart was needed. |
| Cesium | openSUSE packages `php8-cli`, `php8`, `php8-pdo`, `php8-mysql`, `php8-ctype`, and `php8-openssl` are `8.5.12-4.1`. A development PHP built-in server runs on loopback port 8080 and uses `/usr/bin/php`. | Upstream fixed release is installed. The active process uses the installed executable. |
| Nitrogen | Debian 13 `php8.4-cli` and `php8.4-common` are `8.4.27-1~deb13u1`. No PHP-FPM package, PHP-FPM service, Apache service, FastCGI socket, or PHP process was found; Nginx is active. | Fixed upstream version is installed for CLI use; no serving PHP runtime was found to restart. |
| Carbon | Host has no PHP runtime; the only PHP-named package is `php-crypt-gpg`. PHP was absent from each running container checked (Siyuan, Jellyfin, mail sorter, EasyNVR, RTSP, Plex, RTSP tools, Ollama, MySpeed, and RTSP lighttpd). | No PHP serving runtime found. |
| Amino | No PHP host runtime or PHP-FPM service found. Running Home Assistant, n8n, and MySpeed containers had no PHP executable. | No PHP serving runtime found. |
| Benzene | No PHP host runtime. The running RabbitMQ, MariaDB, and Mosquitto containers had no PHP executable. TCP 9001 belongs to Mosquitto. | No PHP serving runtime found. |
| Radium | No PHP runtime or PHP-FPM service found; only `php-crypt-gpg` is installed. TCP 9000 belongs to a Java process. | No PHP serving runtime found. |
| Chlorine | No PHP runtime or PHP-FPM service found; only `php-crypt-gpg` is installed. | No PHP serving runtime found. |

## Runtime recheck — 2026-10-23 08:55 UTC

Read-only checks reconfirmed Xenon's PHP 8.5 packages at Ubuntu's fixed
`8.5.4-0ubuntu1.5` backport, with PHP-FPM and Apache active. On Cesium, the
installed PHP packages remain `8.5.12-4.1`; two development servers are active
on loopback: the scanner listener at `127.0.0.1:8080` and the staged dashboard
at `127.0.0.1:17702`. Both processes resolve to the installed `/usr/bin/php`.
PHP-FPM, Apache, and Nginx services are inactive there. This clarifies that the
Cesium development processes are separate from a system PHP-FPM deployment.

The upstream security notice still identifies both CVEs as critical
unauthenticated RCE issues and recommends updating to the fixed release; the
Ubuntu 26.04 package versions above match USN-8948-1. No package installation or
service restart was needed. Inaccessible/offline devices listed below remain
`UNVERIFIED`.

## Network check and limits

A TCP scan of 22 inventoried infrastructure addresses checked ports 80, 443, 8080, 9000, 9001, and 9003. The open ports found were consistent with the hosts above; no PHP-FPM port was identified. Xenon's public 8080 listener belongs to Nginx; the development PHP listener itself binds only to loopback. The scan does not detect loopback-only or Unix-socket PHP-FPM services, so host package/service inspection was also used wherever SSH was available.

Several inventoried devices could not be verified over SSH because they were offline or unreachable, their hostname did not resolve, or the configured key was rejected: Steel, Silicon, Scandium, Helium, Mercury, Tungsten, Astatine, Quanta, Hydrogen, Sodium, and Valence. The scan did not identify an open PHP-FPM port on these hosts, but their local packages and container contents remain **UNVERIFIED**. No PHP update can be claimed for those devices without access.

## Result

Every PHP runtime found on an accessible host is already at a fixed release. No patch installation was required during this audit. Xenon's active PHP-FPM process is running after the fixed Ubuntu package installation; Cesium's active development server runs PHP 8.5.12; Nitrogen has the fixed CLI runtime and no PHP web server. No vulnerable PHP runtime was identified. Offline and inaccessible inventory devices remain the outstanding verification boundary.

## PHP 8.5 session-progress follow-up — 2026-10-23 17:03 UTC

The PHP Foundation's 23 October informational advisory recommends
`session.upload_progress.enabled=0` until its next regular bugfix release, but
states that applications which do not change `session.serialize_handler` at
runtime can keep upload progress enabled. It describes the unusual trigger as
configuring `php_binary` or `php_serialize`, switching to `php` before
`session_start()`, and leaving upload progress enabled.

Fresh read-only checks found one PHP process on Cesium: the staged SolariNet
dashboard's PHP 8.5.12 built-in server on `127.0.0.1:17701`. Its effective
settings are `session.serialize_handler=php`,
`session.upload_progress.enabled=On`, and `fatal_error_backtraces=On`. The
installed dashboard PHP source contains no runtime serializer change. Xenon's
PHP 8.5 FPM reports upload progress `Off`; Nitrogen is PHP 8.4.27 CLI with no
PHP-serving process, serializer `php`, and no runtime serializer change in the
project source. The scanner-ingest app on port 8765 is Python, not PHP.

No extra restart or INI change was made: the supported PHP CVE fixes are
loaded, and the advisory's additional trigger is absent from the deployed
dashboard code. Reassess if code begins changing the serializer at runtime or
when the next PHP bugfix release is available. The official advisory is at
[php.net](https://www.php.net/archive/2026.php#2026-10-23-1).
