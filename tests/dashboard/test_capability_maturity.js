#!/usr/bin/env node
"use strict";

const fs = require("fs");
const vm = require("vm");
const path = require("path");
const assert = require("assert");

const root = path.join(__dirname, "..", "..");
const sandbox = { window: {}, console, BigInt, Math, Number, String, Object, Array, Date, JSON };
vm.runInNewContext(fs.readFileSync(path.join(root, "dashboard/public/data.jsx"), "utf8"), sandbox, {
  filename: "dashboard/public/data.jsx",
});

const manifest = sandbox.window.SOLARI.capabilityMaturity;
const app = fs.readFileSync(path.join(root, "dashboard/public/app.jsx"), "utf8");
const routesInApp = new Set(Array.from(app.matchAll(/route\.name\s*===\s*"([a-z0-9-]+)"/g), (m) => m[1]));
const capabilities = new Map(manifest.capabilities.map((item) => [item.id, item]));
const levels = new Set(["prototype", "lab", "production", "authoritative"]);

assert(routesInApp.size > 0, "app routes are discoverable");
for (const route of routesInApp) {
  const id = manifest.routes[route];
  assert(id, `route ${route} has a maturity mapping`);
  assert(capabilities.has(id), `route ${route} references a declared capability`);
}
for (const [route, id] of Object.entries(manifest.routes)) {
  assert(routesInApp.has(route), `manifest route ${route} exists in app.jsx`);
  assert(capabilities.has(id), `manifest route ${route} references ${id}`);
}
for (const item of manifest.capabilities) {
  assert(levels.has(item.maturity), `${item.id} has a recognized maturity`);
  for (const route of item.routes) assert(routesInApp.has(route), `${item.id} route ${route} exists in app.jsx`);
}

const frozen = [
  ["tab5-approvals", "panel"],
  ["one-button-pxe", "provision"],
  ["arbitrary-mcp-sql", "opie"],
  ["opie-automation", "opie"],
  ["remote-ca-automation", "certificates"],
  ["directory-writes", "settings"],
];
for (const [id, route] of frozen) {
  const item = capabilities.get(id);
  assert(item, `frozen capability ${id} is declared`);
  assert(item.routes.includes(route), `${id} is surfaced on ${route}`);
  assert(item.note, `${id} explains its lab containment`);
  assert(["lab", "prototype"].includes(item.maturity), `${id} is not marked production-ready`);
}
assert.strictEqual(manifest.routes.inventory, "inventory-sor", "inventory page resolves to the authoritative SoR capability");
assert(app.includes("<CapabilityMaturity route={route.name} />"), "every rendered route passes through the maturity banner");

const babelSandbox = { console, process, setTimeout, clearTimeout };
babelSandbox.window = babelSandbox;
babelSandbox.self = babelSandbox;
vm.createContext(babelSandbox);
vm.runInContext(fs.readFileSync(path.join(root, "dashboard/public/vendor/babel.min.js"), "utf8"), babelSandbox);
const react = {
  createElement(type, props) {
    const children = Array.prototype.slice.call(arguments, 2);
    const nextProps = Object.assign({}, props || {});
    if (children.length) nextProps.children = children.length === 1 ? children[0] : children;
    return { $$el: true, type, props: nextProps };
  },
  useState() { throw new Error("the maturity component should not need state"); },
  useEffect() {}, useRef() {}, useCallback(fn) { return fn; },
};
const renderSandbox = {
  console, React: react, ReactDOM: { createRoot() { return { render() {} }; } },
  document: { getElementById() { return {}; } },
};
renderSandbox.window = renderSandbox;
renderSandbox.Icon = function Icon() {};
renderSandbox.SOLARI = sandbox.window.SOLARI;
vm.createContext(renderSandbox);
const instrumentedApp = app.replace(/\}\)\(\);\s*$/, "window.__CapabilityMaturity = CapabilityMaturity;})();");
assert.notStrictEqual(instrumentedApp, app, "maturity renderer can be instrumented for UI assertions");
vm.runInContext(babelSandbox.Babel.transform(instrumentedApp, { presets: ["react"], filename: "app.jsx" }).code, renderSandbox);

function treeText(node) {
  if (node == null || typeof node === "boolean") return "";
  if (Array.isArray(node)) return node.map(treeText).join(" ");
  if (typeof node !== "object") return String(node);
  if (!node.$$el) return "";
  return treeText(node.props && node.props.children);
}
const panelTree = renderSandbox.__CapabilityMaturity({ route: "panel" });
const panelText = treeText(panelTree);
assert(panelText.includes("Lab"), "panel displays its Lab maturity");
assert(panelText.includes("Tab5 approvals"), "panel identifies the frozen approval surface");
assert(panelText.includes("disabled"), "panel explains approval containment");
const inventoryText = treeText(renderSandbox.__CapabilityMaturity({ route: "inventory" }));
assert(inventoryText.includes("Authoritative"), "inventory displays its authoritative maturity");
assert(inventoryText.includes("Source of truth"), "inventory explains what authoritative means");
const opieText = treeText(renderSandbox.__CapabilityMaturity({ route: "opie" }));
assert(opieText.includes("generic SQL tool is removed"), "Opie page describes the contained arbitrary SQL surface accurately");
const missingTree = renderSandbox.__CapabilityMaturity({ route: "undeclared" });
assert.strictEqual(missingTree.props.role, "alert", "undeclared routes fail visibly instead of looking production-ready");

console.log(`Capability maturity manifest: ${routesInApp.size} app routes, ${manifest.capabilities.length} capabilities, ${frozen.length} frozen surfaces verified`);
