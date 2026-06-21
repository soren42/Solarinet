/* ============================================================
   SolariNet — service worker (Handoff §8.4)

   Caches the app shell + vendored libs so an operator retains the
   last C2 view during a server blip. No build step; plain SW.

   Strategy:
     - app shell + vendored React/Babel + fonts/icons/styles + the
       .jsx sources: cache-first (precached on install, served offline).
       This is what makes the C2 keep painting when the server is down.
     - /api/* reads: network-first with a cache fallback, so a live
       server always wins but the last successful payload survives a
       blip (the adapter's offline fixture is the deeper fallback).
     - everything else: network, falling back to cache.

   Bump CACHE_VERSION on any shell/vendor change to evict the old set.
   ============================================================ */
"use strict";

const CACHE_VERSION = "solari-v2";
const SHELL_CACHE = CACHE_VERSION + "-shell";
const API_CACHE = CACHE_VERSION + "-api";

// App shell: everything index.html pulls in to render the last view.
// Paths are relative to the SW scope (dashboard/public/).
const SHELL_ASSETS = [
  "./",
  "index.html",
  "manifest.webmanifest",
  "styles.css",
  // self-hosted fonts (on-prem / air-gapped — §8.1)
  "fonts/plex.css",
  "fonts/ibm-plex-sans-var.woff2",
  "fonts/ibm-plex-mono-400.woff2",
  "fonts/ibm-plex-mono-500.woff2",
  "fonts/ibm-plex-mono-600.woff2",
  "fonts/ibm-plex-mono-700.woff2",
  // app source (transpiled in-browser by Babel)
  "data.jsx",
  "api.jsx",
  "icons.jsx",
  "components.jsx",
  "screens.jsx",
  "screens2.jsx",
  "screens3.jsx",
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
          return cache.add(url).catch(function (e) {
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
        keys.filter(function (k) { return k.indexOf(CACHE_VERSION) !== 0; })
            .map(function (k) { return caches.delete(k); })
      );
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

  // API reads: network-first, fall back to last good cached response.
  if (url.pathname.indexOf("/api/") === 0) {
    event.respondWith(networkFirst(req));
    return;
  }

  // Shell / assets: cache-first, fall back to network, then to index.html
  // (so a deep route still paints the SPA shell offline).
  event.respondWith(cacheFirst(req));
});

function networkFirst(req) {
  return caches.open(API_CACHE).then(function (cache) {
    return fetch(req).then(function (resp) {
      if (resp && resp.ok) cache.put(req, resp.clone());
      return resp;
    }).catch(function () {
      return cache.match(req).then(function (hit) {
        return hit || new Response(
          JSON.stringify({ ok: false, error: { code: "OFFLINE", message: "no network and no cached response" } }),
          { status: 503, headers: { "Content-Type": "application/json" } }
        );
      });
    });
  });
}

function cacheFirst(req) {
  return caches.match(req).then(function (hit) {
    if (hit) return hit;
    return fetch(req).then(function (resp) {
      if (resp && resp.ok && resp.type === "basic") {
        var copy = resp.clone();
        caches.open(SHELL_CACHE).then(function (cache) { cache.put(req, copy); });
      }
      return resp;
    }).catch(function () {
      // navigation requests fall back to the cached shell
      if (req.mode === "navigate") return caches.match("index.html");
      return Response.error();
    });
  });
}
