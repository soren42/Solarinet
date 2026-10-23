# PHP2Shell remediation record — 2026-10-23

**Status:** known PHP runtimes are on fixed vendor/upstream builds. Xenon's FPM
runtime also has the temporary PHP 8.5 session upload-progress mitigation.
Hosts that could not be reached are listed as unverified; this record does not
claim complete network-wide coverage.

## Advisory and fixed builds

PHP published fixes for CVE-2026-109618 and CVE-2026-109619 on 2026-10-21.
The upstream fixed versions are PHP 8.5.12, 8.4.27, 8.3.36, and 8.2.35. Ubuntu
backports the fixes without changing the upstream version component: its
Ubuntu 26.04 fixed package is `8.5.4-0ubuntu1.5`. The official advisory from
2026-10-23 additionally recommends disabling session upload progress on PHP
8.5 until a regular bugfix release addresses CVE-2026-115560.

Sources: [PHP security advisory](https://www.php.net/archive/2026.php#2026-10-21-5),
[Ubuntu USN-8948-1](https://ubuntu.com/security/notices/USN-8948-1),
[PHP session upload-progress notice](https://www.php.net/archive/2026.php#2026-10-23-1).

## Observed runtime inventory

| Host | Observed runtime | Finding and action |
|---|---|---|
| `xenon` | Ubuntu 26.04; `php8.5-cli`, `php8.5-common`, `php8.5-fpm` at `8.5.4-0ubuntu1.5`; FPM active | Ubuntu's fixed build was already installed. FPM had restarted after its package install. Installed `/etc/php/8.5/fpm/conf.d/99-solarinet-security.ini` from `deploy/dashboard/hardened/php-security-overrides.ini`; `php-fpm8.5 -t` passed, FPM is active, and FPM reports `session.upload_progress.enabled => Off`. |
| `cesium` | openSUSE Tumbleweed; PHP 8.5.12 | Upstream fixed version already installed. Three PHP built-in servers were observed, all bound to loopback (ports 17701, 17702, and 8080). No PHP-FPM unit was present. No package or process changes made. |
| `nitrogen` | Debian 13; PHP 8.4.27 | Upstream fixed version already installed. No PHP-FPM unit or PHP process was observed; nginx was running. No package or process changes made. |
| `benzene`, `chlorine`, `radium`, `amino` | SSH reachable | No PHP CLI or PHP-FPM service observed. |

The repository declares the SolariNet dashboard on Xenon and its hardened FPM
pool in `deploy/dashboard/`. The current code search found no runtime change to
`session.serialize_handler` or use of session upload-progress. The session
upload-progress override is deployed on Xenon; no override was pushed to the
active loopback development servers on Cesium, which were already on PHP 8.5.12.
Reassess those development processes when the upstream follow-up release is
available, or if their code/configuration begins using the affected lifecycle.

## Xenon verification

- `php-fpm8.5 -t`: successful.
- `systemctl is-active php8.5-fpm`: `active` after restart.
- `php-fpm8.5 -i`: `session.upload_progress.enabled => Off => Off`.
- Local HTTPS request to `dashboard.akoria.net/`: HTTP 200.
- Local HTTPS request to `dashboard.akoria.net/api/auth/config`: HTTP 200.
- Ubuntu package changelog contains CVE-2026-109618 and CVE-2026-109619 fixes.

The CLI version string on Xenon remains `8.5.4`; use the Ubuntu package
revision, not that upstream-only string, to judge patch status.

## Coverage limits

These systems were not verified over SSH during this pass: `silicon`, `quanta`,
`tungsten`, `hydrogen`, `astatine`, `steel`, `scandium`, `radio`, and `quartz`
timed out; `valence` rejected the available SSH key; `helium` and `mercury` did
not resolve by their configured names. Their PHP status remains unknown. The
host list is based on `netdb/akoria-hosts.yml`; unmanaged VMs or containers not
listed there are outside the observed inventory.

No PHP package installation was needed on the reachable hosts because their
installed builds were already patched. The only live change was Xenon's FPM
configuration override and restart. No DNS, DHCP, firewall, or switch state was
changed. No repository commit or push was made.
