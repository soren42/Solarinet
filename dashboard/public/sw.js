"use strict";

const CACHE_NAME = "solarinet-pwa-v1";
const SHELL_ASSETS = [
  "/", "/index.html", "/manifest.webmanifest", "/styles.css", "/icon.svg",
  "/fonts/plex.css", "/fonts/ibm-plex-sans-var.woff2", "/fonts/ibm-plex-mono-400.woff2",
  "/fonts/ibm-plex-mono-500.woff2", "/fonts/ibm-plex-mono-600.woff2", "/fonts/ibm-plex-mono-700.woff2",
  "/vendor/react.production.min.js", "/vendor/react-dom.production.min.js", "/vendor/babel.min.js",
  "/data.jsx", "/api.jsx", "/icons.jsx", "/components.jsx", "/screens.jsx", "/layout.jsx",
  "/screens2.jsx", "/screens3.jsx", "/screens4.jsx", "/screens5.jsx", "/screens6.jsx", "/screens7.jsx", "/app.jsx",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(CACHE_NAME).then((cache) => Promise.all(
    SHELL_ASSETS.map((asset) => cache.add(asset).catch(() => undefined))
  )).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (event) => {
  event.waitUntil(caches.keys().then((names) => Promise.all(
    names.filter((name) => name !== CACHE_NAME).map((name) => caches.delete(name))
  )).then(() => self.clients.claim()));
});

self.addEventListener("fetch", (event) => {
  const request = event.request;
  const url = new URL(request.url);
  if (request.method !== "GET" || url.origin !== self.location.origin) return;
  if (url.pathname.indexOf("/api/") === 0) {
    // Authenticated API responses must never enter a browser cache.
    event.respondWith(fetch(request));
    return;
  }
  event.respondWith(caches.match(request).then((cached) => cached || fetch(request).then((response) => {
    if (response.ok && response.type === "basic") {
      caches.open(CACHE_NAME).then((cache) => cache.put(request, response.clone()));
    }
    return response;
  }).catch(() => request.mode === "navigate" ? caches.match("/index.html") : Response.error())));
});

self.addEventListener("push", (event) => {
  let payload = {};
  try { payload = event.data ? event.data.json() : {}; } catch (_) {
    try { payload = JSON.parse(event.data ? event.data.text() : "{}"); } catch (_) { payload = {}; }
  }
  const severity = String(payload.severity || "info").toLowerCase();
  event.waitUntil(self.registration.showNotification(payload.title || "SolariNet", {
    body: payload.body || "",
    icon: "/icon.svg",
    badge: "/icon.svg",
    tag: payload.tag || "solarinet-alert",
    data: { url: payload.url || "/" },
    requireInteraction: severity === "crit",
  }));
});

self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  const target = (event.notification.data && event.notification.data.url) || "/";
  // An ack action is reserved for a future authenticated in-app acknowledgement;
  // meanwhile it follows the same safe focus/open behavior as the notification.
  event.waitUntil(clients.matchAll({ type: "window", includeUncontrolled: true }).then((clientsList) => {
    for (const client of clientsList) {
      if ("focus" in client) return client.focus();
    }
    return clients.openWindow(target);
  }));
});
