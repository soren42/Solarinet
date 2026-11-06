/* ============================================================
   SolariNet — service worker (Handoff §8.4)

   Caches the app shell + vendored libs so an operator retains the
   last C2 view during a server blip. No build step; plain SW.

   Strategy:
     - app code (index.html, *.jsx, styles.css): NETWORK-FIRST with a cache
       fallback. The intranet server is normally up, so an operator always
       gets the freshly deployed code; the cache only serves during a server
       blip (so the C2 keeps painting offline). This avoids the stale-bundle
       trap where a redeploy was masked by a cache-first shell.
     - vendored libs + fonts + icons: the same. Nothing is served from cache
       while the server answers, so a deploy cannot be masked.
     - /api/* reads: network only. Authenticated data must never be served from
       a prior session's cache or presented as fresh during a network outage.
     - compiled JSX: source-hash CacheStorage entries survive worker upgrades;
       the page loader removes stale transforms as each source changes.
     - release.txt: network only. It exists to detect a new deploy.
     - everything else: network, falling back to cache.

   Bump CACHE_VERSION when this worker or SHELL_ASSETS changes. The JSX transform
   cache is retained because its keys include a source digest and preset.
   ============================================================ */
"use strict";

// Bump the version to evict API responses cached by earlier service workers.
const CACHE_VERSION = "solari-v24";
const SHELL_CACHE = CACHE_VERSION + "-shell";
const TRANSFORM_CACHE = "solari-jsx-transform-v2";

// App shell: everything index.html pulls in to render the last view.
// Paths are relative to the SW scope (dashboard/public/).
const SHELL_ASSETS = [
  "./",
  "index.html",
  "boot.js?v=20261024d",
  "transform-worker.js?v=20261024d",
  "manifest.webmanifest",
  "styles.css",
  // self-hosted fonts (on-prem / air-gapped — §8.1)
  "fonts/plex.css",
  "fonts/ibm-plex-sans-var.woff2",
  "fonts/ibm-plex-mono-400.woff2",
  "fonts/ibm-plex-mono-500.woff2",
  "fonts/ibm-plex-mono-600.woff2",
  "fonts/ibm-plex-mono-700.woff2",
  // editable app source; compiled responses live in TRANSFORM_CACHE
  "data.jsx",
  "api.jsx",
  "icons.jsx",
  "components.jsx",
  "screens.jsx",
  "layout.jsx",
  "screens2.jsx",
  "screens3.jsx",
  "screens4.jsx",
  "screens5.jsx",
  "screens6.jsx",
  "screens7.jsx",
  "screens8.jsx",
  "screens-panel.jsx",
  "screens-rackwire.jsx",
  "screens-prov.jsx",
  "app.jsx",
  // vendored libs (on-prem / air-gapped — §8.1)
  "vendor/react.production.min.js",
  "vendor/react-dom.production.min.js",
  "vendor/babel.min.js",
  // icons
  "icons/icon-192.png",
  "icons/icon-512.png",
  "icons/icon-maskable-512.png",
];

// ---- install: precache the shell ------------------------------------
self.addEventListener("install", function (event) {
  event.waitUntil(
    caches.open(SHELL_CACHE).then(function (cache) {
      // addAll is atomic; tolerate a missing optional asset by adding individually.
      return Promise.all(
        SHELL_ASSETS.map(function (url) {
          return cache.add(new Request(url, { cache: "no-store" })).catch(function (e) {
            console.warn("[sw] precache skipped:", url, e && e.message);
          });
        })
      );
    }).then(function () { return self.skipWaiting(); })
  );
});

// ---- activate: drop stale cache versions ----------------------------
self.addEventListener("activate", function (event) {
  event.waitUntil(
    caches.keys().then(function (keys) {
      return Promise.all(
        keys.filter(function (k) { return k.indexOf(CACHE_VERSION) !== 0 && k !== TRANSFORM_CACHE; })
            .map(function (k) { return caches.delete(k); })
      );
    }).catch(function (error) {
      // Cache enumeration/cleanup failures are non-fatal. Keeping stale shell
      // entries is safe because shell reads are network-first.
      console.warn("[sw] cache activation cleanup incomplete; retaining caches:", error && error.message);
    }).then(function () { return self.clients.claim(); })
  );
});

// ---- fetch: route by request kind -----------------------------------
self.addEventListener("fetch", function (event) {
  const req = event.request;
  if (req.method !== "GET") return; // never cache mutations (POST control plane)

  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return; // let cross-origin pass through

  // SSE stream must never be cached/buffered.
  if (url.pathname.indexOf("/api/stream") === 0) return;

  // API reads are session-bound and time-sensitive. Never cache or replay them;
  // let a failed fetch reach api.jsx so the UI can label the view offline.
  if (url.pathname.indexOf("/api/") === 0 || url.pathname === "/release.txt") {
    event.respondWith(fetch(req, { cache: "no-store" }));
    return;
  }

  // Everything else: network-first so a redeploy is picked up immediately when
  // online; the cached copy is the offline fallback.
  event.respondWith(networkFirst(req, SHELL_CACHE));
});

function fetchFresh(req) {
  // Giving a navigation request init options would change its mode, so it
  // relies on the server's no-store instead.
  return req.mode === "navigate" ? fetch(req) : fetch(req, { cache: "no-store" });
}

// Applied one at a time in arrival order, so an older response cannot land on
// top of a newer one for the same path.
var refreshQueue = Promise.resolve();

function networkFirst(req, cacheName) {
  return caches.open(cacheName).then(function (cache) {
    return fetchFresh(req).then(function (resp) {
      if (resp && resp.ok && resp.type === "basic") {
        // Offline reads ignore the query string, so a copy under an older
        // deploy token must not outlive this one.
        var copy = resp.clone();
        refreshQueue = refreshQueue.then(function () {
          return cache.delete(req, { ignoreSearch: true }).then(function () { return cache.put(req, copy); });
        }).catch(function (e) {
          console.warn("[sw] offline copy not refreshed:", req.url, e && e.message);
        });
      }
      return resp;
    }).catch(function () {
      // The shell precache stores canonical paths while index.html may add
      // deployment query tokens. This shell cache holds one copy per path, so
      // ignoring search here selects the newest fallback.
      return cache.match(req, { ignoreSearch: true }).then(function (hit) {
        if (hit) return hit;
        if (req.mode === "navigate") return caches.match("index.html");
        return new Response(
          JSON.stringify({ ok: false, error: { code: "OFFLINE", message: "no network and no cached response" } }),
          { status: 503, headers: { "Content-Type": "application/json" } }
        );
      });
    });
  });
}
