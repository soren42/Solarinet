# SolariNet dashboard — intranet deployment

Sanitized templates for the dashboard tier (PHP API + SPA) behind HTTPS on a
single intranet host. Secrets (DB password, credential file, private keys) are
**never** in these files or in git — they live under `run/` (gitignored) and in
the php-fpm pool config you fill in at install time.

Stack: **MariaDB** ← **solariServer** (SCP + `solariCtl` bridge) ← **php-fpm**
pool (API, runs as the repo owner) ← **Apache** vhost (HTTPS, serves the SPA and
proxies `/api/`).

## 1. Database
```sh
sudo mariadb <<'SQL'
CREATE DATABASE IF NOT EXISTS solarinet CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS 'solari'@'127.0.0.1' IDENTIFIED BY 'CHANGE_ME';
GRANT SELECT,INSERT,UPDATE,DELETE ON solarinet.* TO 'solari'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
sudo mariadb solarinet < db/schema.sql
sudo mariadb solarinet < db/migrations/002_c2_capabilities.sql
sudo mariadb solarinet < db/migrations/003_config_engine.sql
# record the password for the server + fpm:
printf 'export SOLARI_DB_PASS=CHANGE_ME\n' > run/db.env && chmod 600 run/db.env
```

## 2. TLS PKI (internal CA + server/dashboard certs)
Generate an internal CA and issue: a SCP server cert (`server.*`) and a web cert
(`dashboard.*`, SANs covering the hostname/IP). Stage the web cert where Apache
reads it:
```sh
sudo mkdir -p /etc/ssl/solarinet
sudo install -m644 run/pki/dashboard.pem /etc/ssl/solarinet/dashboard.pem
sudo install -m640 run/pki/dashboard.key /etc/ssl/solarinet/dashboard.key
```

## 3. Admin credential
```sh
php dashboard/api/tools/solari-passwd.php --user admin --random   # prints the password once
```

## 4. SCP server (systemd)
```sh
printf 'SOLARI_DB_PASS=CHANGE_ME\n' > run/server.env && chmod 600 run/server.env
sudo cp deploy/dashboard/solarinet-server.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now solarinet-server
```

## 5. php-fpm pool
```sh
sudo cp deploy/dashboard/php-fpm-solarinet.conf /etc/php/8.4/fpm/pool.d/solarinet.conf
sudoedit /etc/php/8.4/fpm/pool.d/solarinet.conf   # set env[SOLARI_DB_PASS]
sudo chmod 640 /etc/php/8.4/fpm/pool.d/solarinet.conf
mkdir -p run/sessions && chmod 700 run/sessions
sudo php-fpm8.4 -t && sudo systemctl reload php8.4-fpm
```

## 6. SPA + Apache vhost
```sh
sudo mkdir -p /var/www/solarinet
sudo cp -r dashboard/public/. /var/www/solarinet/ && sudo chown -R www-data:www-data /var/www/solarinet
sudo a2enmod ssl proxy proxy_fcgi headers
sudo cp deploy/dashboard/apache-solarinet.conf /etc/apache2/sites-available/solarinet.conf
sudo a2ensite solarinet && sudo apache2ctl configtest && sudo systemctl reload apache2
```

Browse to `https://<host>:9443/` and sign in. Re-run step 6's `cp` after any SPA
change (Apache serves a copy). API (PHP) edits are live from the repo.

## Gotchas
- **`<LocationMatch "^/api/">` needs the trailing slash** — `^/api` also matches
  the SPA's `/api.jsx` adapter and would route it to PHP, breaking the app.
- The php-fpm pool runs as the **repo owner** so PHP can read `solari-auth.json`
  (0600) and connect to the jason-owned `solariCtl` socket.
- The service worker serves app code **network-first**; a redeploy is picked up
  on reload. Bump `CACHE_VERSION` in `sw.js` only on vendored-asset changes.
