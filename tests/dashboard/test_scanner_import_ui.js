#!/usr/bin/env node
/* Exercise the scanner import panel's file, mapping, preview, and apply flow. */
"use strict";
const fs = require("fs");
const vm = require("vm");
const path = require("path");
const PUB = path.join(__dirname, "..", "..", "dashboard", "public");

function reactShim() {
  let frame = null;
  const effects = new Set();
  const React = { Fragment: "FRAGMENT" };
  React.createElement = function (type, props) {
    const children = Array.prototype.slice.call(arguments, 2);
    const p = Object.assign({}, props || {});
    if (children.length) p.children = children.length === 1 ? children[0] : children;
    return { element: true, type, props: p };
  };
  React.useState = function (initial) {
    const key = frame.path + ":" + frame.index++;
    if (!(key in frame.store)) frame.store[key] = typeof initial === "function" ? initial() : initial;
    const store = frame.store;
    return [store[key], (value) => { store[key] = typeof value === "function" ? value(store[key]) : value; }];
  };
  React.useEffect = function (effect) {
    if (frame && frame.path.indexOf("EntityLinkEditor") >= 0) {
      const key = frame.path + ":effect";
      if (!effects.has(key)) { effects.add(key); effect(); }
    }
  };
  React.__enter = function (next) { const old = frame; frame = next; return old; };
  React.__exit = function (old) { frame = old; };
  return React;
}
function render(React, element, store, pathName) {
  if (element == null || element === false || element === true) return null;
  if (Array.isArray(element)) return element.map((e, i) => render(React, e, store, pathName + "/" + i)).filter(Boolean);
  if (typeof element !== "object" || !element.element) return String(element);
  if (typeof element.type === "function") {
    const old = React.__enter({ store, path: pathName + "/" + (element.type.name || "anonymous"), index: 0 });
    let output;
    try { output = element.type(element.props); } finally { React.__exit(old); }
    return render(React, output, store, pathName + "/" + (element.type.name || "anonymous"));
  }
  const node = { tag: String(element.type), props: element.props, children: [] };
  if (element.props && element.props.children != null) {
    const children = render(React, element.props.children, store, pathName + ">children");
    node.children = Array.isArray(children) ? children : children == null ? [] : [children];
  }
  return node;
}
function walk(node, fn) {
  if (!node || typeof node !== "object") return;
  if (Array.isArray(node)) return node.forEach((n) => walk(n, fn));
  fn(node); (node.children || []).forEach((n) => walk(n, fn));
}
function all(tree, predicate) { const found = []; walk(tree, (n) => { if (predicate(n)) found.push(n); }); return found; }
function text(node) {
  if (node == null || node === false || node === true) return "";
  if (typeof node === "string" || typeof node === "number") return String(node);
  if (Array.isArray(node)) return node.map(text).join("");
  return (node.children || []).map(text).join("");
}

async function main() {
  const snapshot = {
    schema_version: 1,
    items: [
      { id: "item-1", name: "Switch", manufacturer: "Acme", model: "S1", category: "switch", archived: 0 },
      { id: "item-2", name: "Wireless device", manufacturer: "Acme", model: "W1", category: "Wi-Fi device", archived: 0 },
    ],
    assets: [{ id: "asset-1", item_id: "item-1", tag: "TAG-1", location: "Nook", status: "in_stock", archived: 0 }],
  };
  const calls = [];
  const profileCalls = [];
  const entityLinkCalls = [];
  const entityLinkReads = [];
  const navigation = [];
  const report = { mode: "preview", counts: { "insert.hardware_units": 1 }, warnings: [], fingerprint: "a".repeat(64), locations: { Nook: { id: 9104, name: "Painted Fern" } } };
  const React = reactShim();
  const env = {
    console, Promise, Date, Number, Math, JSON, Object, Array, Set, String,
    SOLARI: { source: "live", fmt: { usd: (x) => String(x) }, inventory: { models: [{ id: 1, make: "Acme", model: "Test switch" }], units: Array.from({ length: 101 }, (_, i) => ({ id: i + 1, model_id: 1, stock_status: "in_stock" })), locations: [{ id: 9103, name: "Painted Fern" }, { id: 9104, name: "Painted Fern" }], receipts: Array.from({ length: 101 }, (_, i) => ({ id: i + 1, vendor: "Receipt " + (i + 1), subtotal_cents: 100 })), projects: Array.from({ length: 101 }, (_, i) => ({ id: i + 1, name: "Project " + (i + 1), status: "active" })), allocations: [] },
      api: {
        inventory: () => Promise.resolve({ models: [{ id: 1, make: "Acme", model: "Test switch", hw_type: "switch" }],
          units: Array.from({ length: 101 }, (_, i) => ({ id: i + 1, model_id: 1, stock_status: "in_stock" })),
          locations: [{ id: 9103, name: "Painted Fern" }, { id: 9104, name: "Painted Fern" }], receipts: [], projects: [], allocations: [] }),
        scannerImport: (mode, body) => { calls.push({ mode, body }); return Promise.resolve(mode === "preview" ? report : { mode: "applied", counts: { "insert.hardware_units": 1 }, warnings: [] }); },
        inventoryEntityLink: (unitId, query) => { entityLinkReads.push({ unitId, query }); return Promise.resolve({
          unit: { id: unitId, assetTag: "SCAN-1", serial: "SER-1" }, entity: null,
          candidates: [{ id: 501, name: "chlorine", entityType: "server", lifecycle: "active" }],
        }); },
        linkInventoryEntity: (unitId, entityId) => { entityLinkCalls.push({ unitId, entityId: Number(entityId) }); return Promise.resolve({ unitId, entityId }); },
        unlinkInventoryEntity: (unitId) => Promise.resolve({ unitId }),
        rackwireLibrary: () => Promise.resolve([]),
        saveRackwireProfile: (id, body) => { profileCalls.push({ id, body }); return Promise.resolve({ id: String(id), groups: body.groups, name: body.name, model: "Test switch", kind: body.kind, drawW: body.drawW }); },
        refresh: () => Promise.resolve(),
      } },
    Icon: () => null, DangerModal: () => null, __solariToast: () => {},
    FileReader: function () { this.readAsText = function () { this.result = JSON.stringify(snapshot); this.onload(); }; },
  };
  env.window = env; env.React = React;
  vm.createContext(env);
  vm.runInContext(fs.readFileSync(path.join(PUB, "vendor/babel.min.js"), "utf8"), env, { filename: "babel.min.js" });
  const source = fs.readFileSync(path.join(PUB, "screens4.jsx"), "utf8");
  vm.runInContext(env.Babel.transform(source, { presets: ["react"], filename: "screens4.jsx" }).code, env, { filename: "screens4.jsx" });
  const store = {};
  const draw = () => render(React, React.createElement(env.Inventory, { onNavigate: (route) => navigation.push(route) }), store, "root");
  let tree = draw();
  const componentRows = () => all(tree, (n) => n.tag === "tbody").flatMap((n) => n.children).filter((n) => n.tag === "tr");
  if (componentRows().length !== 100) throw new Error("component table should render only one 100-row page");
  const nextPage = all(tree, (n) => n.tag === "button" && text(n) === "Next")[0];
  if (!nextPage) throw new Error("component pagination controls are missing");
  nextPage.props.onClick(); tree = draw();
  if (componentRows().length !== 1 || !all(tree, (n) => n.tag === "span" && text(n) === "Page 2 of 2").length) throw new Error("component pagination did not render the final page");
  const previousPage = all(tree, (n) => n.tag === "button" && text(n) === "Previous")[0];
  previousPage.props.onClick(); tree = draw();
  if (componentRows().length !== 100) throw new Error("component pagination did not return to the first page");
  const receiptsTab = all(tree, (n) => n.tag === "button" && text(n) === "Receipts")[0];
  receiptsTab.props.onClick(); tree = draw();
  const receiptCards = () => all(tree, (n) => n.props && n.props.className === "inv-receipt");
  if (receiptCards().length !== 100) throw new Error("receipt list should render only one 100-entry page");
  all(tree, (n) => n.tag === "button" && text(n) === "Next")[0].props.onClick(); tree = draw();
  if (receiptCards().length !== 1) throw new Error("receipt pagination did not render the final page");
  const projectsTab = all(tree, (n) => n.tag === "button" && text(n) === "Projects")[0];
  projectsTab.props.onClick(); tree = draw();
  const projectCards = () => all(tree, (n) => n.tag === "h3" && text(n).startsWith("Project "));
  if (projectCards().length !== 100) throw new Error("project list should render only one 100-entry page");
  all(tree, (n) => n.tag === "button" && text(n) === "Next")[0].props.onClick(); tree = draw();
  if (projectCards().length !== 1) throw new Error("project pagination did not render the final page");
  const open = all(tree, (n) => n.tag === "button" && n.props && n.props["aria-pressed"] === false && text(n).includes("Scanner import"))[0];
  if (!open) throw new Error("scanner import section button is missing");
  open.props.onClick(); tree = draw();
  const file = all(tree, (n) => n.tag === "input" && n.props && n.props.type === "file")[0];
  if (!file) throw new Error("JSON export input is missing");
  file.props.onChange({ target: { files: [{ name: "snapshot.json", size: 1500000 }] } }); tree = draw();
  if (all(tree, (n) => n.tag === "p" && text(n).includes("no larger than")).length) throw new Error("valid scanner export larger than 1 MiB was rejected");
  if (!all(tree, (n) => n.tag === "p" && n.props && n.props.role === "note" && text(n).includes("Some SoR locations share a name" )).length) throw new Error("duplicate-location destination warning is missing");
  const automaticCategory = all(tree, (n) => n.tag === "label" && text(n).startsWith("switch") && all(n, (child) => child.tag === "select").length)[0];
  if (!automaticCategory || !all(automaticCategory, (n) => n.tag === "option" && text(n) === "Automatic · switch").length) throw new Error("recognized categories do not explain their automatic SoR type");
  const categoryLabel = all(tree, (n) => n.tag === "label" && text(n).includes("Wi-Fi device") && all(n, (child) => child.tag === "select").length)[0];
  if (!categoryLabel) throw new Error("unrecognized source category mapping control is missing");
  const category = all(categoryLabel, (n) => n.tag === "select")[0];
  if (!all(category, (n) => n.tag === "option" && text(n) === "Automatic · other").length) throw new Error("unrecognized categories do not explain their fallback SoR type");
  category.props.onChange({ target: { value: "ap" } }); tree = draw();
  const location = all(tree, (n) => n.tag === "select")[0];
  if (!location) throw new Error("location mapping control is missing");
  const locationChoices = all(location, (n) => n.tag === "option").map(text);
  if (!locationChoices.some((label) => label.includes("#9103 · Painted Fern")) || !locationChoices.some((label) => label.includes("#9104 · Painted Fern"))) throw new Error("duplicate location names are not distinguishable by ID");
  location.props.onChange({ target: { value: "9104" } }); tree = draw();
  const preview = all(tree, (n) => n.tag === "button" && text(n).includes("Preview import"))[0];
  if (!preview) throw new Error("preview action is missing");
  preview.props.onClick(); await Promise.resolve(); await Promise.resolve(); tree = draw();
  if (calls.length !== 1 || calls[0].mode !== "preview" || calls[0].body.mapping.locations.Nook !== 9104 || calls[0].body.mapping.categories["Wi-Fi device"] !== "ap") throw new Error("preview did not send the selected location and category mappings");
  if (!all(tree, (n) => n.tag === "strong" && text(n).includes("Resolved locations")).length || !all(tree, (n) => n.tag === "li" && text(n).includes("#9104 · Painted Fern")).length) throw new Error("preview did not show the selected duplicate-name SoR location");
  const apply = all(tree, (n) => n.tag === "button" && text(n).includes("Apply import"))[0];
  if (!apply) throw new Error("apply action is not shown after preview");
  apply.props.onClick(); await Promise.resolve(); await Promise.resolve(); tree = draw();
  if (calls.length !== 2 || calls[1].mode !== "apply" || calls[1].body.expectedFingerprint !== report.fingerprint || !all(tree, (n) => n.tag === "strong" && text(n).includes("Import complete")).length) throw new Error("apply did not send the reviewed fingerprint and complete through the API");
  const configureRackwire = all(tree, (n) => n.tag === "button" && text(n) === "Configure RackWire profiles")[0];
  if (!configureRackwire) throw new Error("completed scanner import does not offer the RackWire profile handoff");
  configureRackwire.props.onClick(); tree = draw();
  if (!all(tree, (n) => n.tag === "h3" && text(n) === "RackWire device profile").length) throw new Error("RackWire profile editor did not open");
  const modelSelect = all(tree, (n) => n.tag === "select" && n.props["aria-label"] === "SoR hardware model")[0];
  if (!modelSelect) throw new Error("RackWire profile editor is missing its SoR model selector");
  modelSelect.props.onChange({ target: { value: "1" } }); tree = draw();
  const portCount = all(tree, (n) => n.tag === "input" && n.props["aria-label"] === "Port group 1 count")[0];
  if (!portCount) throw new Error("RackWire profile editor is missing port count controls");
  portCount.props.onChange({ target: { value: "8" } }); tree = draw();
  const saveProfile = all(tree, (n) => n.tag === "button" && text(n) === "Save RackWire profile")[0];
  if (!saveProfile) throw new Error("RackWire profile save action is missing");
  saveProfile.props.onClick(); await Promise.resolve(); await Promise.resolve(); tree = draw();
  if (profileCalls.length !== 1 || profileCalls[0].id !== 1 || profileCalls[0].body.groups[0].count !== 8 || profileCalls[0].body.groups[0].conn !== "rj45") throw new Error("RackWire profile editor did not save the selected SoR model's connector geometry");
  if (!all(tree, (n) => n.tag === "div" && n.props && n.props.role === "status" && text(n).includes("Profile saved")).length) throw new Error("RackWire profile save success was not announced");
  const openRackwire = all(tree, (n) => n.tag === "button" && text(n) === "Open RackWire planner")[0];
  if (!openRackwire) throw new Error("saved model profile does not offer a direct RackWire handoff");
  openRackwire.props.onClick();
  if (navigation.length !== 1 || navigation[0] !== "rackwire") throw new Error("profile editor handoff did not navigate to RackWire");

  all(tree, (n) => n.tag === "button" && text(n) === "Components")[0].props.onClick(); tree = draw();
  const linkAction = all(tree, (n) => n.tag === "button" && text(n) === "Link entity")[0];
  if (!linkAction) throw new Error("unlinked inventory units do not offer an entity association action");
  linkAction.props.onClick({ stopPropagation() {} }); tree = draw();
  await Promise.resolve(); await Promise.resolve(); tree = draw();
  if (!all(tree, (n) => n.tag === "h3" && text(n) === "Physical identity ↔ SoR entity").length
      || !all(tree, (n) => n.tag === "p" && text(n).includes("never matched automatically")).length
      || !entityLinkReads.some((entry) => entry.unitId === 1)) throw new Error("explicit entity association panel did not explain and load the identity bridge");
  const entitySelect = all(tree, (n) => n.tag === "select" && all(n, (option) => option.tag === "option" && text(option).includes("chlorine")).length)[0];
  if (!entitySelect) throw new Error("entity association did not show named, typed SoR candidates");
  entitySelect.props.onChange({ target: { value: "501" } }); tree = draw();
  const linkForm = all(tree, (n) => n.tag === "form" && n.props.className === "inv-entity-link__form")[0];
  linkForm.props.onSubmit({ preventDefault() {} });
  for (let i = 0; i < 10; i++) await Promise.resolve();
  tree = draw();
  if (entityLinkCalls.length !== 1 || entityLinkCalls[0].unitId !== 1 || entityLinkCalls[0].entityId !== 501
      || !all(tree, (n) => n.tag === "p" && n.props.role === "status" && text(n).includes("association saved")).length) {
    throw new Error("entity association did not save its selected SoR entity and announce success");
  }
  console.log("inventory paging, scanner import review/apply, RackWire profile editing, identity association, and handoff flows passed");
}
main().catch((e) => { console.error(e.message); process.exitCode = 1; });
