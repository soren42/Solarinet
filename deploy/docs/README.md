# SolariNet documentation portal

`index.html` is the portal page for the three system manuals in
`docs/manuals/` (User, Developer, API & Expansion). Each manual is one
self-contained HTML file plus a US Letter PDF rendered with
`tools/docs/render-pdf.sh`.

## Publish (xenon)

From a clean checkout of `main`:

    sudo deploy/docs/install-docs.sh            # → /var/www/html/docs/

The script refuses a dirty docs tree, missing files or non-Letter PDF pages,
stages into the docroot, writes `manifest.json` (revision, date, page counts)
and swaps the directory in. The portal reads the manifest for its footer and
page counts. The service directory (`deploy/landing/index.html`) links it as
**SolariNet Documentation** at `https://akoria.net/docs/`.

Never edit `/var/www/html/docs/` by hand; change the repository and republish.
