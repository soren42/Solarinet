# Akoria service-directory landing page

Static start page linking the core service interfaces (SolariNet, Forgejo,
Keycloak/SSO, Directory, UniFi, CA). Self-contained HTML, theme-aware, no build.

## Deploy (xenon)
Served at `https://akoria.net/` and `https://xenon.akoria.net/` from the
`akoria.net` vhost docroot:

    sudo cp deploy/landing/index.html /var/www/html/index.html
    sudo chmod 644 /var/www/html/index.html

The bare apex `akoria.net` resolves to xenon via `APEX_A` in
`netdb/gen-zones.py` (rendered into the BIND zone by the `sor-apply-dns` daemon).

## Adding a service
Copy an `<a class="card">` block, set `href` / name / host. Recolor a whole
category via `style="--ac:var(--c-net|c-dev|c-id|c-sec)"`.
