#!/usr/bin/env node
/* Exercise the Inventory I1 WP5 location UI against a mocked WP4 API:
   hierarchy/breadcrumb, legal create levels, edit 409 reload, move picker,
   merge preview/confirm, bulk wizard, container project release, and the
   container-type admin. Run: node tests/dashboard/test_inventory_locations_ui.js */
"use strict";
const fs = require("fs");
const vm = require("vm");
const path = require("path");
const assert = require("assert");
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
    const key = frame.path + ":effect:" + frame.index++;
    if (!effects.has(key)) { effects.add(key); effect(); }
  };
  React.__enter = function (next) { const old = frame; frame = next; return old; };
  React.__exit = function (old) { frame = old; };
  React.__forget = function (fragment) { effects.forEach((k) => { if (k.indexOf(fragment) >= 0) effects.delete(k); }); };
  return React;
}
function render(React, element, store, pathName) {
  if (element == null || element === false || element === true) return null;
  if (Array.isArray(element)) return element.map((e, i) => render(React, e, store, pathName + "/" + i)).filter(Boolean);
  if (typeof element !== "object" || !element.element) return String(element);
  if (typeof element.type === "function") {
    const name = pathName + "/" + (element.type.name || "anonymous");
    const old = React.__enter({ store, path: name, index: 0 });
    let output;
    try { output = element.type(element.props); } finally { React.__exit(old); }
    return render(React, output, store, name);
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
async function ticks(n) { for (let i = 0; i < (n || 12); i++) await Promise.resolve(); }
// vm-realm objects and arrays have foreign prototypes; compare their JSON shape.
function same(actual, expected, message) { assert.deepStrictEqual(JSON.parse(JSON.stringify(actual)), expected, message); }
function apiError(code, message) { const e = new Error(message); e.code = code; return e; }

const STAMP = "2026-11-03T10:00:00Z";
const LOCATIONS = [
  { id: 1, parentId: null, name: "Home", locType: "property", updatedAt: STAMP },
  { id: 2, parentId: 1, name: "Ground", locType: "floor", updatedAt: STAMP },
  { id: 3, parentId: 2, name: "Workshop", locType: "room", updatedAt: STAMP },
  { id: 4, parentId: 3, name: "North wall", locType: "section", updatedAt: STAMP },
  { id: 5, parentId: 4, name: "Cabinet A", locType: "storage_unit", containerTypeId: 11, assignedProjectId: 21, updatedAt: STAMP },
  { id: 6, parentId: 5, name: "Drawer 1", code: "D1", locType: "storage_unit", updatedAt: STAMP },
  { id: 7, parentId: 2, name: "Office", locType: "room", updatedAt: STAMP },
  { id: 8, parentId: 7, name: "Cabinet B", locType: "storage_unit", updatedAt: STAMP },
  { id: 9, parentId: null, name: "Studio", locType: "property", updatedAt: STAMP },
  { id: 10, parentId: 9, name: "Loft", locType: "floor", updatedAt: STAMP },
];

async function main() {
  const calls = [];
  const plan = {};   // method → array of queued results ({error} | value)
  function mock(name, fallback) {
    return function () {
      const args = Array.prototype.slice.call(arguments);
      calls.push({ name, args });
      const q = plan[name] || [];
      const next = q.length ? q.shift() : (typeof fallback === "function" ? fallback.apply(null, args) : fallback);
      return next && next.error ? Promise.reject(next.error) : Promise.resolve(next);
    };
  }
  const snapshot = {
    models: [{ id: 1, make: "Ubiquiti", model: "Ubiquiti U7 Pro", hwType: "ap" }],
    units: [{ id: 1, modelId: 1, locationId: 6, assetTag: "AKO-0001", stockStatus: "in_stock" }],
    locations: LOCATIONS, receipts: [], allocations: [],
    projects: [{ id: 21, name: "Lab rebuild", status: "active" }, { id: 22, name: "Garden", status: "planning" }],
  };
  const React = reactShim();
  const env = {
    console, Promise, Date, Number, Math, JSON, Object, Array, Set, String, RegExp, Error,
    addEventListener() {}, removeEventListener() {},
    SOLARI: {
      source: "live", fmt: { usd: (x) => String(x) }, inventory: snapshot,
      api: {
        inventory: mock("inventory", () => snapshot),
        refresh: () => Promise.resolve(),
        createLocation: mock("createLocation", (body) => ({ id: 99, name: body.name })),
        getLocation: mock("getLocation"),
        updateLocation: mock("updateLocation", (id) => ({ id })),
        moveLocation: mock("moveLocation", (id, parentId) => ({ id, parent_id: parentId })),
        locationMergePreview: mock("locationMergePreview"),
        mergeLocation: mock("mergeLocation", (id, targetId) => ({ source_id: id, target_id: targetId, merged: true })),
        bulkStorageUnits: mock("bulkStorageUnits", (body) => ({ ids: [], count: body.count })),
        setLocationProject: mock("setLocationProject", (id, projectId) => ({ id, project_id: projectId })),
        containerTypes: mock("containerTypes", () => [
          { id: 11, name: "Cabinet", created_at: "2026-11-01 09:00:00", updated_at: "2026-11-01 09:00:00", deleted_at: null },
          { id: 12, name: "Old bin", created_at: "2026-10-01 09:00:00", updated_at: "2026-11-02 00:00:00", deleted_at: "2026-11-02 00:00:00" },
        ]),
        createContainerType: mock("createContainerType", (name) => ({ id: 13, name })),
        renameContainerType: mock("renameContainerType", (id, name) => ({ id, name })),
        retireContainerType: mock("retireContainerType", (id) => ({ id, status: "deleted" })),
      },
    },
    Icon: () => null, DangerModal: () => null, __solariToast: () => {},
  };
  env.window = env; env.React = React;
  vm.createContext(env);
  vm.runInContext(fs.readFileSync(path.join(PUB, "vendor/babel.min.js"), "utf8"), env, { filename: "babel.min.js" });
  const source = fs.readFileSync(path.join(PUB, "screens4.jsx"), "utf8");
  vm.runInContext(env.Babel.transform(source, { presets: ["react"], filename: "screens4.jsx" }).code, env, { filename: "screens4.jsx" });

  const store = {};
  let tree = null;
  const draw = () => { tree = render(React, React.createElement(env.Inventory, { onNavigate() {} }), store, "root"); return tree; };
  // Sheets keep shim state by component path; forget it when one closes so a
  // reopened sheet starts fresh, as React would remount it.
  const forget = (name) => { Object.keys(store).forEach((k) => { if (k.indexOf("/" + name) >= 0) delete store[k]; }); React.__forget("/" + name); };
  const buttons = (label, exact) => all(tree, (n) => n.tag === "button" && (exact ? text(n).trim() === label : text(n).indexOf(label) >= 0));
  const click = async (label, exact, index) => {
    const b = buttons(label, exact)[index || 0];
    assert(b, "missing button: " + label);
    assert(!b.props.disabled, "button is disabled: " + label);
    const event = { preventDefault() {}, stopPropagation() {} };
    if (b.props.onClick) b.props.onClick(event);
    else {
      // A submit button with no handler submits its enclosing form, as a browser would.
      const form = all(tree, (n) => n.tag === "form" && all(n, (c) => c === b).length)[0];
      assert(form && b.props.type === "submit", "button has no action: " + label);
      form.props.onSubmit(event);
    }
    await ticks(); draw();
  };
  const input = (pred) => all(tree, (n) => (n.tag === "input" || n.tag === "textarea" || n.tag === "select") && pred(n))[0];
  const labelled = (label) => {
    const lab = all(tree, (n) => n.tag === "label" && n.children.some((c) => c.tag === "span" && text(c) === label))[0];
    assert(lab, "missing field: " + label);
    return all(lab, (n) => n.tag === "input" || n.tag === "select" || n.tag === "textarea")[0];
  };
  const type = async (field, value) => { field.props.onChange({ target: { value } }); await ticks(2); draw(); };
  const alertText = () => all(tree, (n) => n.props && n.props.role === "alert").map(text).join(" | ");
  const sheetTitle = () => all(tree, (n) => n.props && n.props.role === "dialog").map((d) => d.props["aria-label"])[0] || null;
  const callsOf = (name) => calls.filter((c) => c.name === name);

  draw(); await ticks(); draw();
  await click("Locations", true);
  assert(callsOf("containerTypes").length === 1 && callsOf("containerTypes")[0].args[0] === true, "container types load with retired rows for the admin");

  // 1. hierarchy: roots open by default; search jumps to a deep node and opens its path.
  assert(buttons("Home").length && buttons("Ground").length, "top-level property and its floor are listed");
  assert(!buttons("Drawer 1").length, "deep storage units start collapsed");
  await type(input((n) => n.props["aria-label"] === "Find a location by name or code"), "d1");
  await click("Drawer 1");
  const crumbs = all(tree, (n) => n.tag === "nav" && n.props["aria-label"] === "Location path")[0];
  assert(crumbs, "detail shows a breadcrumb");
  same(all(crumbs, (n) => n.tag === "li").map(text), ["All locations", "Home", "Ground", "Workshop", "North wall", "Cabinet A", "Drawer 1"]);
  assert(all(tree, (n) => n.props && n.props.className === "inv-loc-row on").length === 1, "selected node is highlighted in the opened tree");
  assert(all(tree, (n) => n.tag === "li" && text(n).indexOf("Ubiquiti U7 Pro") === 0).length === 1, "components show the 035 display name, not Brand Brand model");

  // 2. create: only legal levels under a property; a root create is property-only.
  await click("Home", true, 0);
  await click("Add inside");
  assert.strictEqual(sheetTitle(), "Add inside Home");
  same(all(labelled("Level"), (n) => n.tag === "option").map(text), ["Floor", "Storage unit"]);
  await type(labelled("Name"), "Upstairs");
  await click("Add location");
  const created = callsOf("createLocation")[0].args[0];
  same(created, { name: "Upstairs", loc_type: "floor", code: null, notes: null, parent_id: 1 });
  assert.strictEqual(sheetTitle(), null, "create sheet closes after success");
  forget("LocationFormSheet");
  await click("New property");
  assert(all(tree, (n) => n.props && n.props.className === "inv-field__static" && text(n).indexOf("Property") === 0).length, "root create is fixed to property");
  await click("Cancel", true); forget("LocationFormSheet");

  // 3. move: room picker lists floors only, hides own subtree, flags the current parent.
  await type(input((n) => n.props["aria-label"] === "Find a location by name or code"), "workshop");
  await click("Workshop");
  await click("Move", true);
  const radios = () => all(tree, (n) => n.props && n.props.role === "radio");
  same(radios().map((r) => text(r)), ["Home › Groundcurrent", "Studio › LoftFloor"]);
  assert(radios()[0].props.disabled, "current parent cannot be chosen");
  radios()[1].props.onClick(); draw();
  await click("Move into Loft");
  same(callsOf("moveLocation")[0].args, [3, 10]);
  assert(callsOf("inventory").length >= 2, "screen re-reads inventory after a write");
  forget("MoveLocationSheet");
  await type(input((n) => n.props["aria-label"] === "Find a location by name or code"), "cabinet a");
  await click("Cabinet A");
  await click("Move", true);
  const storageTargets = radios().map((r) => text(r));
  assert(!storageTargets.some((t) => /Cabinet A|Drawer 1/.test(t.replace(/^.*›\s*/, ""))) , "storage unit move hides itself and its children");
  assert(storageTargets.length === 8 && /North wallcurrent$/.test(storageTargets[5]), "storage units may move under any other location (current parent shown, disabled): " + storageTargets.join(", "));
  plan.moveLocation = [{ error: apiError("location_cycle", "Move would create a cycle.") }];
  radios()[0].props.onClick(); draw();
  await click("Move into");
  assert(/inside itself/.test(alertText()), "cycle error is explained");
  await click("Cancel", true); forget("MoveLocationSheet");

  // 4. edit: 409 → "changed elsewhere", reload fresh row, retry with its updated_at.
  plan.updateLocation = [{ error: apiError("conflict", "Location changed since it was read.") }];
  plan.getLocation = [{ id: 5, parentId: 4, name: "Cabinet A2", locType: "storage_unit", containerTypeId: 11, assignedProjectId: 21, updatedAt: "2026-11-03T11:00:00Z" }];
  await click("Edit", true);
  await click("Save changes");
  assert(/changed elsewhere/.test(alertText()), "stale edit says it changed elsewhere");
  assert.strictEqual(labelled("Name").props.value, "Cabinet A2", "form reloads the latest version");
  await click("Save changes");
  const retry = callsOf("updateLocation")[1].args;
  assert.strictEqual(retry[0], 5);
  assert.strictEqual(retry[1].updated_at, "2026-11-03T11:00:00Z", "retry uses the reloaded lock value");
  assert(!("container_type_id" in retry[1]) && !("parent_id" in retry[1]), "unchanged container type and parent are not re-sent");
  forget("LocationFormSheet");

  // 5. project: releasing a non-empty container is explained, not swallowed.
  await click("Project", true);
  plan.setLocationProject = [{ error: apiError("container_not_empty", "Container has live children or hardware units.") }];
  await click("Release", true);
  assert(/still holds/.test(alertText()), "container_not_empty is human-readable");
  same(callsOf("setLocationProject")[0].args, [5, null]);
  await click("Cancel", true); forget("ContainerProjectSheet");

  // 6. merge: same-level targets outside the subtree; preview summary; fingerprint; re-preview on conflict.
  await type(input((n) => n.props["aria-label"] === "Find a location by name or code"), "cabinet b");
  await click("Cabinet B");
  await click("Merge…");
  same(radios().map((r) => text(r)), ["Home › Ground › Workshop › North wall › Cabinet A", "Home › Ground › Workshop › North wall › Cabinet A › Drawer 1 D1"]);
  radios()[0].props.onClick(); draw();
  const fp = "a".repeat(64);
  const preview = { source: { id: 8, loc_type: "storage_unit", assigned_project_id: 22 }, target: { id: 5, loc_type: "storage_unit", assigned_project_id: null },
    counts: { "hardware_units.location_id": 2, "locations.parent_id": 0, "barcode_scans": 1, "custom.loc_id": 1 }, fingerprint: fp };
  plan.locationMergePreview = [preview, preview];
  await click("Preview merge");
  same(callsOf("locationMergePreview")[0].args, [8, 5]);
  const summary = text(all(tree, (n) => n.props && n.props.className === "inv-merge")[0]);
  ["Merged away", "Cabinet B", "Kept", "Components (move to the target)2", "Scan history (stays with the source)1", "custom.loc_id1", "soft-deleted, not erased", "takes over the project"].forEach((s) => assert(summary.indexOf(s) >= 0, "merge summary missing: " + s));
  assert(summary.indexOf("Child locations") < 0, "zero-count classes are omitted");
  plan.mergeLocation = [{ error: apiError("merge_conflict", "Location references changed after preview.") }];
  await click("Merge into Cabinet A");
  assert(/Preview the merge again/.test(alertText()) && buttons("Preview merge").length, "merge_conflict sends the operator back to preview");
  await click("Preview merge");
  await click("Merge into Cabinet A");
  same(callsOf("mergeLocation")[1].args, [8, 5, fp], "merge confirms with the preview fingerprint");
  forget("MergeLocationSheet");

  // 7. bulk wizard: numbered + codes preview, validation, all-or-none failure, pasted names.
  await type(input((n) => n.props["aria-label"] === "Find a location by name or code"), "workshop");
  await click("Workshop");
  await click("Add storage units");
  await type(labelled("How many"), "501");
  assert(/Count must be/.test(text(all(tree, (n) => n.props && /inv-bulk-errors/.test(n.props.className || ""))[0])), "count over 500 is rejected before submit");
  assert(buttons("Create ")[0].props.disabled, "create disabled while invalid");
  await type(labelled("How many"), "3");
  await type(labelled("Name prefix"), "  Bin-");
  await type(labelled("Code prefix (optional)"), "B");
  const previewRows = () => all(tree, (n) => n.tag === "ol" && n.props["aria-label"] === "Generated storage units").flatMap((o) => o.children).map(text);
  same(previewRows(), ["Bin-1B1", "Bin-2B2", "Bin-3B3"]);
  same(all(labelled("Container type (optional)"), (n) => n.tag === "option").map(text), ["— none —", "Cabinet"], "retired types leave pick lists");
  plan.bulkStorageUnits = [{ error: apiError("duplicate_location", "Location code B1 already exists.") }];
  await click("Create 3 storage units");
  assert(/Nothing was created/.test(alertText()), "failed batch says nothing was created");
  same(callsOf("bulkStorageUnits")[0].args[0], { parent_id: 3, count: 3, prefix: "Bin-", start: 1, code_prefix: "B", code_start: 1 });
  await click("Paste names");
  await type(labelled("Names, one per line (up to 500)"), "Red\nBlue\n\nred");
  assert(/appears twice/.test(text(all(tree, (n) => n.props && /inv-bulk-errors/.test(n.props.className || ""))[0])), "case-insensitive duplicate names are caught");
  await type(labelled("Names, one per line (up to 500)"), "Red\nBlue");
  await type(labelled("Code prefix (optional)"), "");
  await click("Create 2 storage units");
  same(callsOf("bulkStorageUnits")[1].args[0], { parent_id: 3, count: 2, names: ["Red", "Blue"] });
  forget("BulkStorageSheet");

  // 8. container types admin.
  await click("Container types", true);
  const typeNames = () => all(tree, (n) => n.props && n.props.className === "inv-ctype__text").map((n) => text(n.children[0]));
  same(typeNames(), ["Cabinet"], "retired types hidden by default");
  await click("Include retired");
  same(typeNames(), ["Cabinet", "Old bin"]);
  await click("Rename", true);
  await type(labelled("Rename Cabinet"), "Cabinets");
  await click("Save", true);
  same(callsOf("renameContainerType")[0].args, [11, "Cabinets", "2026-11-01 09:00:00"]);
  await click("Retire", true);
  assert(/1 container keeps this type/.test(text(all(tree, (n) => n.props && n.props.className === "inv-ctype__confirm")[0])), "retire confirm says the type stays on its containers");
  await click("Retire", true);
  same(callsOf("retireContainerType")[0].args, [11]);
  await type(labelled("New container type"), "Tray");
  await click("Add type");
  same(callsOf("createContainerType")[0].args, ["Tray"]);

  console.log("inventory location hierarchy, create/edit/move/merge/bulk/project, and container-type flows passed");
}
main().catch((e) => { console.error(e && e.stack || e); process.exitCode = 1; });
