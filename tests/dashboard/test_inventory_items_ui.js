#!/usr/bin/env node
/* Exercise the Inventory I1 close-out item flows (migration 037) against a
   mocked API: Components creates and edits items by product type (retired
   types are not offered, a retired current type is kept), software items
   carry licence seats/expiry/reference and an installed-on item, licence keys
   are refused before any call, a retype away from software clears the licence,
   API errors stay in the sheet, and the Catalog "Add item(s)" sheet creates N
   items (serial/asset tag only for one), or one consumable with a quantity.
   Run: node tests/dashboard/test_inventory_items_ui.js */
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

const STAMP = "2026-11-03 10:00:00.000000";
const PI = { id: 1, kind: "hardware", name: "Raspberry Pi 400", brand: "Raspberry Pi", model: "400", category: "SBC", specs: null, notes: null, image_url: null, lookup_source: "migration:033", updated_at: STAMP };
const OFFICE = { id: 3, kind: "software", name: "Office 2024", brand: "Microsoft", model: null, category: null, specs: null, notes: null, image_url: null, lookup_source: null, updated_at: STAMP };
const TIES = { id: 4, kind: "consumable", name: "Cable ties", brand: null, model: null, category: null, specs: null, notes: null, image_url: null, lookup_source: null, updated_at: STAMP };
const page = (items) => ({ items, page: { limit: 500, offset: 0, total: items.length, hasMore: false, nextOffset: null } });

async function main() {
  const calls = [];
  const plan = {};
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
    models: [{ id: 5, make: "Raspberry Pi", model: "400", hwType: "sbc" }],
    units: [
      { id: 10, modelId: 5, productTypeId: 1, productTypeName: "Raspberry Pi 400", productTypeKind: "hardware", serial: "SN1", assetTag: null, locationId: 2, quantity: 1, stockStatus: "in_stock" },
      { id: 11, modelId: null, productTypeId: 3, productTypeName: "Office 2024", productTypeKind: "software", serial: null, locationId: null, quantity: 1, stockStatus: "in_stock",
        licenceSeats: 5, licenceExpiresOn: "2027-01-31", licenceReference: "Vault: Office", installedOnUnitId: 10, installedOnName: "Raspberry Pi 400" },
      { id: 12, modelId: null, productTypeId: 9, productTypeName: "Old switch", productTypeKind: "hardware", productTypeRetired: true, serial: "OLD-1", locationId: null, quantity: 1, stockStatus: "in_stock" },
    ],
    locations: [{ id: 1, parentId: null, name: "Home", locType: "property" }, { id: 2, parentId: 1, name: "Rack A", code: "R1", locType: "storage_unit" }],
    receipts: [], allocations: [], projects: [],
  };
  const React = reactShim();
  const env = {
    console, Promise, Date, Number, Math, JSON, Object, Array, Set, String, RegExp, Error,
    addEventListener() {}, removeEventListener() {},
    setTimeout: () => 0, clearTimeout: () => {},
    SOLARI: {
      source: "live", fmt: { usd: (x) => String(x) }, inventory: snapshot,
      api: {
        inventory: mock("inventory", () => snapshot),
        refresh: () => Promise.resolve(),
        containerTypes: mock("containerTypes", () => []),
        productTypes: mock("productTypes", () => page([PI, OFFICE, TIES])),
        getProductType: mock("getProductType", (id) => Object.assign({}, id === 1 ? PI : id === 3 ? OFFICE : TIES, { barcodes: [] })),
        createUnit: mock("createUnit", (body) => (body.count != null ? { items: [], count: body.count } : Object.assign({ id: 99 }, body))),
        updateUnit: mock("updateUnit", (id, body) => Object.assign({ id }, body)),
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
  const forget = (name) => { Object.keys(store).forEach((k) => { if (k.indexOf("/" + name) >= 0) delete store[k]; }); React.__forget("/" + name); };
  const buttons = (label, exact) => all(tree, (n) => n.tag === "button" && (exact ? text(n).trim() === label : text(n).indexOf(label) >= 0));
  const click = async (label, exact, index) => {
    const b = buttons(label, exact)[index || 0];
    assert(b, "missing button: " + label);
    assert(!b.props.disabled, "button is disabled: " + label);
    b.props.onClick({ preventDefault() {}, stopPropagation() {} });
    await ticks(); draw();
  };
  const group = (label) => all(tree, (n) => n.props && n.props.role === "radiogroup" && n.props["aria-label"] === label)[0];
  const options = (label) => all(group(label), (n) => n.tag === "button").map(text);
  const pick = async (groupLabel, label) => {
    const g = group(groupLabel);
    assert(g, "missing pick list: " + groupLabel);
    const b = all(g, (n) => n.tag === "button" && text(n).indexOf(label) >= 0)[0];
    assert(b, "missing choice " + label + " in " + groupLabel);
    b.props.onClick(); await ticks(); draw();
  };
  const labelled = (label) => {
    const lab = all(tree, (n) => n.tag === "label" && n.children.some((c) => c.tag === "span" && text(c) === label))[0];
    assert(lab, "missing field: " + label);
    return all(lab, (n) => n.tag === "input" || n.tag === "select" || n.tag === "textarea")[0];
  };
  const hasField = (label) => all(tree, (n) => n.tag === "label" && n.children.some((c) => c.tag === "span" && text(c) === label)).length > 0;
  const type = async (label, value) => { labelled(label).props.onChange({ target: { value } }); await ticks(2); draw(); };
  const alertText = () => all(tree, (n) => n.props && n.props.role === "alert").map(text).join(" | ");
  const sheetTitle = () => all(tree, (n) => n.props && n.props.role === "dialog").map((d) => d.props["aria-label"])[0] || null;
  const callsOf = (name) => calls.filter((c) => c.name === name);
  // Opening the item sheet starts the product-type load; let it land.
  const openRow = async (needle) => {
    const row = all(tree, (n) => n.tag === "tr" && n.props.onClick && text(n).indexOf(needle) >= 0)[0];
    assert(row, "missing component row: " + needle);
    row.props.onClick(); await ticks(); draw(); await ticks(); draw();
  };
  const newItem = async () => { await click("New item", true); await ticks(); draw(); };

  draw(); await ticks(); draw();
  // 1. the list names items by product type and summarises software licences.
  const body = text(tree);
  assert(body.indexOf("Raspberry Pi 400") >= 0 && body.indexOf("Office 2024") >= 0 && body.indexOf("Old switch") >= 0, "items read by product type name");
  assert(body.indexOf("5 seats · expires 2027-01-31 · on Raspberry Pi 400 · Vault: Office") >= 0, "software rows summarise the licence");
  assert(body.indexOf("Software") >= 0, "the kind is shown under the name");

  // 2. new hardware item by product type; retired types are not offered.
  await newItem();
  assert.strictEqual(sheetTitle(), "New item");
  same(callsOf("productTypes")[0].args, [{ limit: 500, offset: 0 }]);
  const offered = options("Product type");
  assert(offered.some((o) => o.indexOf("Raspberry Pi 400") === 0) && offered.some((o) => o.indexOf("Office 2024") === 0), "live types are offered");
  assert(!offered.some((o) => o.indexOf("Old switch") >= 0), "retired types are not offered");
  await click("Add item", true);
  assert(/pick a product type/.test(alertText()) && !callsOf("createUnit").length, "a type is required before any call");
  await pick("Product type", "Raspberry Pi 400");
  assert(!all(tree, (n) => n.tag === "legend" && text(n) === "Licence").length, "hardware has no licence fields");
  await type("Serial", " SN2 ");
  await pick("Location", "Home › Rack A R1");
  await click("Add item", true);
  same(callsOf("createUnit")[0].args, [{ serial: "SN2", asset_tag: null, location_id: 2, acquired_on: null, quantity: 1, unit_cost_cents: null, stock_status: "in_stock", receipt_id: null, notes: null,
    licence_seats: null, licence_expires_on: null, licence_reference: null, installed_on_unit_id: null, product_type_id: 1 }]);
  assert.strictEqual(sheetTitle(), null, "the sheet closes after saving");
  assert(callsOf("inventory").length >= 2, "saving re-reads the inventory");
  forget("UnitModal");

  // 3. software item: licence fields, installed-on offers only non-software items, keys refused.
  await newItem();
  await pick("Product type", "Office 2024");
  assert(all(tree, (n) => n.tag === "legend" && text(n) === "Licence").length, "software shows licence fields");
  const hosts = options("Installed on");
  assert(hosts.some((o) => o.indexOf("Raspberry Pi 400") === 0) && hosts.some((o) => o.indexOf("Old switch") === 0), "hardware items can host software");
  assert(!hosts.some((o) => o.indexOf("Office 2024") === 0), "software cannot host software");
  await type("Licence reference", "ABCDE-12345-FGHIJ-67890-KLMNO");
  await click("Add item", true);
  assert(/never the key/.test(alertText()) && callsOf("createUnit").length === 1, "a product key is refused before any call");
  await type("Licence reference", "api token in vault");
  await click("Add item", true);
  assert(/never the key/.test(alertText()) && callsOf("createUnit").length === 1, "secret words are refused before any call");
  await type("Licence reference", "1Password: Office family");
  await type("Seats", "0");
  await click("Add item", true);
  assert(/Seats must be/.test(alertText()) && callsOf("createUnit").length === 1, "seats must be positive");
  await type("Seats", "3");
  await type("Expires on", "2027-06-30");
  await pick("Installed on", "Raspberry Pi 400");
  plan.createUnit = [{ error: apiError("bad_request", "installed_on_unit_id must reference another live, non-software item.") }];
  await click("Add item", true);
  assert(/non-software item/.test(alertText()) && sheetTitle() === "New item", "API errors stay in the open sheet");
  await click("Add item", true);
  same(callsOf("createUnit")[2].args[0], { serial: null, asset_tag: null, location_id: null, acquired_on: null, quantity: 1, unit_cost_cents: null, stock_status: "in_stock", receipt_id: null, notes: null,
    licence_seats: 3, licence_expires_on: "2027-06-30", licence_reference: "1Password: Office family", installed_on_unit_id: 10, product_type_id: 3 });
  forget("UnitModal");

  // 4. edit: unchanged type is not resent; a stale/locked save reports "try again".
  await openRow("SN1");
  assert.strictEqual(sheetTitle(), "Edit item");
  assert(text(group("Product type")).indexOf("Raspberry Pi 400") >= 0 && all(group("Product type"), (n) => n.props && n.props["aria-checked"] === true).map(text)[0].indexOf("Raspberry Pi 400") === 0, "the current type is selected");
  await type("Serial", "SN1-B");
  plan.updateUnit = [{ error: apiError("conflict", "Another change was in progress; try again.") }];
  await click("Save changes", true);
  assert(/try again/.test(alertText()), "a lock conflict asks to try again");
  await click("Save changes", true);
  const edit1 = callsOf("updateUnit")[1].args;
  assert.strictEqual(edit1[0], 10);
  assert(!("product_type_id" in edit1[1]) && edit1[1].serial === "SN1-B" && edit1[1].licence_seats === null, "an unchanged type is not resent");
  forget("UnitModal");

  // 5. retype software → hardware clears the licence; the licence fields go away.
  await openRow("Vault: Office");
  assert.strictEqual(labelled("Seats").props.value, "5");
  assert.strictEqual(labelled("Licence reference").props.value, "Vault: Office");
  await pick("Product type", "Raspberry Pi 400");
  assert(!hasField("Seats"), "licence fields hide for hardware");
  await click("Save changes", true);
  const edit2 = callsOf("updateUnit")[2].args;
  assert.strictEqual(edit2[0], 11);
  same([edit2[1].product_type_id, edit2[1].licence_seats, edit2[1].licence_expires_on, edit2[1].licence_reference, edit2[1].installed_on_unit_id], [1, null, null, null, null]);
  forget("UnitModal");

  // 6. an item on a retired type keeps it unless a live type is chosen.
  await openRow("OLD-1");
  assert(/Old switch is retired/.test(text(tree)), "a retired current type is explained");
  assert(options("Product type").some((o) => o.indexOf("Old switch") === 0 && o.indexOf("retired") > 0), "the retired current type stays selectable");
  await click("Save changes", true);
  assert(!("product_type_id" in callsOf("updateUnit")[3].args[1]), "keeping a retired type does not resend it");
  forget("UnitModal");

  // 7. Catalog → Add item(s): N items, serial only for one, batch cap, notice.
  await click("Catalog", true);
  await ticks(); draw();
  await click("Raspberry Pi 400");
  assert(/Items \(1\)/.test(text(tree)), "the type lists its recorded items");
  await click("Add item(s)", true);
  assert.strictEqual(sheetTitle(), "Add Raspberry Pi 400");
  assert(hasField("Serial (optional)"), "one item offers a serial");
  await type("How many items", "3");
  assert(!hasField("Serial (optional)") && !hasField("Asset tag (optional)"), "several items offer no serial or asset tag");
  assert(/Adds 3 separate items/.test(text(tree)));
  await type("How many items", "51");
  await click("Add 51 items", true);
  assert(/up to 50/.test(alertText()) && callsOf("createUnit").length === 3, "the batch cap is checked first");
  await type("How many items", "3");
  await pick("Location", "Home › Rack A R1");
  await type("Notes (optional)", "spares");
  await click("Add 3 items", true);
  same(callsOf("createUnit")[3].args, [{ product_type_id: 1, location_id: 2, notes: "spares", count: 3 }]);
  assert(/Added 3 items of Raspberry Pi 400/.test(text(tree)), "the result is confirmed");
  forget("AddItemsSheet");

  // 8. consumable: one item carrying the quantity.
  plan.productTypes = [page([TIES])];
  await click("Consumable", true);
  await click("Cable ties");
  await click("Add item(s)", true);
  await type("Quantity", "100");
  assert(hasField("Serial (optional)") && /counted on one item/.test(text(tree)), "a consumable is one item");
  await click("Add item", true);
  same(callsOf("createUnit")[4].args, [{ product_type_id: 4, location_id: null, notes: null, count: 1, quantity: 100, serial: null, asset_tag: null }]);
  forget("AddItemsSheet");

  // 9. software from the catalog: licence fields and installed-on.
  plan.productTypes = [page([OFFICE])];
  await click("Software", true);
  await click("Office 2024");
  await click("Add item(s)", true);
  await type("Seats", "10");
  await type("Licence reference", "Order 2026-114");
  await pick("Installed on", "Old switch");
  await click("Add item", true);
  same(callsOf("createUnit")[5].args, [{ product_type_id: 3, location_id: null, notes: null, licence_seats: 10, licence_expires_on: null, licence_reference: "Order 2026-114", installed_on_unit_id: 12, count: 1, serial: null, asset_tag: null }]);

  console.log("inventory item create/edit by product type, software licence, retired-type and catalog add-items flows passed");
}
main().catch((e) => { console.error(e && e.stack || e); process.exitCode = 1; });
