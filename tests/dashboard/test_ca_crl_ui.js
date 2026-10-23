#!/usr/bin/env node
"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const root = path.join(__dirname, "..", "..");
const pub = path.join(root, "dashboard", "public");
const babelSandbox = { console, process, setTimeout, clearTimeout };
babelSandbox.window = babelSandbox;
babelSandbox.self = babelSandbox;
vm.createContext(babelSandbox);
vm.runInContext(fs.readFileSync(path.join(pub, "vendor/babel.min.js"), "utf8"), babelSandbox);

const React = {
  Fragment: "fragment",
  createElement(type, props) {
    const children = Array.prototype.slice.call(arguments, 2);
    return { type, props: Object.assign({}, props || {}, {
      children: children.length === 1 ? children[0] : children,
    }) };
  },
  useState(value) { return [typeof value === "function" ? value() : value, function () {}]; },
  useEffect() {},
};

function fixture(overrides) {
  return Object.assign({
    stepCa: { available: true, status: "ok", url: "https://chlorine.akoria.net" },
    roots: [], provisioners: [], nodeCerts: [], warnDays: 60,
    crl: {
      available: true,
      scope: "Client and monitor reporting agents only",
      generations: [{
        issuerFp: "a".repeat(64), crlNumber: "12", crlFp: "f".repeat(64),
        generatedAt: "2026-10-23T12:00:00Z", nextUpdate: "2026-10-24T12:00:00Z",
        expired: false, peerCount: 4, excludedPeerCount: 2,
        unknownIssuerCount: 1, otherIssuerCount: 1,
        excludedPeers: [
          { nodeId: "6", hostFqdn: "unknown-issuer.akoria.net", reason: "unknown_issuer" },
          { nodeId: "7", hostFqdn: "different-issuer.akoria.net", reason: "different_issuer" },
        ],
        counts: { current: 1, stale: 1, pending: 1, offline: 1, expired: 0 },
        peers: [
          { nodeId: "1", hostFqdn: "current.akoria.net", role: "client", status: "current", acknowledgedNumber: "12", lastSeenAt: "2026-10-23T12:00:00Z" },
          { nodeId: "2", hostFqdn: "stale.akoria.net", role: "monitor", status: "stale", acknowledgedNumber: "11", lastSeenAt: "2026-10-23T12:00:00Z" },
          { nodeId: "3", hostFqdn: "pending.akoria.net", role: "client", status: "pending", lastSeenAt: "2026-10-23T12:00:00Z" },
          { nodeId: "4", hostFqdn: "offline.akoria.net", role: "monitor", status: "offline", lastSeenAt: null },
        ],
      }],
    },
  }, overrides || {});
}

const sandbox = {
  console, React, ReactDOM: { createRoot() { return { render() {} }; } },
  Icon() { return null; }, SOLARI: { ca: fixture(), source: "api" },
  Date, Math, Number, String, Object, Array, JSON,
};
sandbox.window = sandbox;
vm.createContext(sandbox);
const source = fs.readFileSync(path.join(pub, "screens6.jsx"), "utf8");
const instrumented = source.replace(/\}\)\(\);\s*$/, "window.__CaScreen = CaScreen;})();");
assert.notStrictEqual(instrumented, source, "Certificates screen is instrumented");
vm.runInContext(babelSandbox.Babel.transform(instrumented, { presets: ["react"], filename: "screens6.jsx" }).code, sandbox);

function renderText(node) {
  if (node == null || typeof node === "boolean") return "";
  if (Array.isArray(node)) return node.map(renderText).join(" ");
  if (typeof node !== "object") return String(node);
  if (typeof node.type === "function") return renderText(node.type(node.props || {}));
  return renderText(node.props && node.props.children);
}

function findElements(node, tag, out = []) {
  if (node == null || typeof node !== "object") return out;
  if (Array.isArray(node)) {
    for (const child of node) findElements(child, tag, out);
    return out;
  }
  if (typeof node.type === "function") {
    findElements(node.type(node.props || {}), tag, out);
    return out;
  }
  if (node.type === tag) out.push(node);
  findElements(node.props && node.props.children, tag, out);
  return out;
}

const text = renderText(sandbox.__CaScreen({}));
for (const label of ["CRL distribution", "current", "stale", "pending", "offline",
  "Client + monitor · matching current issuer", "Agents without a recorded matching current leaf issuer are listed separately; server peers and other services on a host are outside this status view."]) {
  assert(text.includes(label), `Certificates screen exposes ${label}; rendered: ${text}`);
}
assert(/CRL\s+#\s+12/.test(text), "generation number is shown");
const rows = findElements(sandbox.__CaScreen({}), "tr");
const rowText = (host) => rows.map(renderText).find((value) => value.includes(host)) || "";
assert(rowText("current.akoria.net").includes("Current") && rowText("current.akoria.net").includes("# 12"), `current host row carries its exact receipt status: ${JSON.stringify(rows.map(renderText))}`);
assert(rowText("stale.akoria.net").includes("Stale receipt") && rowText("stale.akoria.net").includes("# 11"), "stale host row carries its stale receipt status");
assert(rowText("pending.akoria.net").includes("No receipt"), "pending host row explains the missing receipt");
assert(rowText("offline.akoria.net").includes("Offline"), "offline host row carries its offline status");
assert(/\b1\s+unknown/.test(text) && /\b1\s+different issuer/.test(text) && text.includes("unknown-issuer.akoria.net") && text.includes("different-issuer.akoria.net"), "unknown and different issuer agents remain individually visible");

const expiredFixture = fixture();
expiredFixture.crl.generations[0].expired = true;
expiredFixture.crl.generations[0].counts = { current: 0, stale: 1, pending: 1, offline: 1, expired: 1 };
expiredFixture.crl.generations[0].peers[0].status = "expired";
expiredFixture.crl.generations[0].peers[0].acknowledgementCurrent = false;
sandbox.SOLARI.ca = expiredFixture;
const expiredText = renderText(sandbox.__CaScreen({}));
assert(expiredText.includes("Expired generation") && expiredText.includes("This CRL is expired or its expiry is unavailable"), "an expired or incomplete generation has its own status and warning");

sandbox.SOLARI.ca = fixture({ crl: { available: false, reason: "CRL acknowledgement data unavailable", generations: [] } });
assert(renderText(sandbox.__CaScreen({})).includes("Acknowledgement data unavailable"), "migration/API outage has an explicit state");
sandbox.SOLARI.ca = fixture({ crl: { available: true, generations: [] } });
assert(renderText(sandbox.__CaScreen({})).includes("No CRL generation recorded"), "empty generation state explains why a ledger alone is insufficient");

console.log("CRL Certificates screen states and peer disclosure verified");
