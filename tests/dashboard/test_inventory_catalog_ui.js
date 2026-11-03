#!/usr/bin/env node
/* Exercise the Inventory I1 WP5 product catalog UI against a mocked WP2 API:
   list/search/kind filter, empty state with no seeding, display names for bare
   models, create/edit (changed fields only, 409 reload, spec rows keep JSON
   types, secret-like spec names refused), retire, barcode attach/list/remove,
   and the exact-code lookup that only prefills (D3).
   Run: node tests/dashboard/test_inventory_catalog_ui.js */
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

const STAMP = "2026-11-03 10:00:00";
const PI = { id: 1, kind: "hardware", name: "400", brand: "Raspberry Pi", model: "400", category: "SBC", specs: { ram: "4 GB", cores: 4, gpio: { pins: 40 } }, notes: null, image_url: null, lookup_source: "migration:033", lookup_ref: null, updated_at: STAMP };
const U7 = { id: 2, kind: "hardware", name: "Ubiquiti U7 Pro", brand: "Ubiquiti", model: "U7 Pro", category: null, specs: null, notes: null, image_url: null, lookup_source: null, updated_at: STAMP };
const OFFICE = { id: 3, kind: "software", name: "Office 2024", brand: "Microsoft", model: null, category: null, specs: [], notes: null, image_url: null, lookup_source: null, updated_at: STAMP };
const page = (items, total, hasMore, nextOffset) => ({ items, page: { limit: 50, offset: 0, total, hasMore: !!hasMore, nextOffset: nextOffset == null ? null : nextOffset } });

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
  const detail = (t, barcodes) => Object.assign({}, t, { barcodes: barcodes || [] });
  const snapshot = { models: [], units: [], locations: [{ id: 1, parentId: null, name: "Home", locType: "property" }], receipts: [], allocations: [], projects: [] };
  const React = reactShim();
  const timers = [];
  const env = {
    console, Promise, Date, Number, Math, JSON, Object, Array, Set, String, RegExp, Error,
    addEventListener() {}, removeEventListener() {},
    setTimeout: (fn) => { timers.push(fn); return timers.length; }, clearTimeout: (id) => { timers[id - 1] = null; },
    SOLARI: {
      source: "live", fmt: { usd: (x) => String(x) }, inventory: snapshot,
      api: {
        inventory: mock("inventory", () => snapshot),
        refresh: () => Promise.resolve(),
        containerTypes: mock("containerTypes", () => []),
        updateLocation: mock("updateLocation"),
        productTypes: mock("productTypes", () => page([PI, U7], 3, true, 2)),
        getProductType: mock("getProductType", (id) => detail(id === 1 ? PI : id === 3 ? OFFICE : U7, id === 1 ? [{ id: 7, code: "0123", code_type: "sku" }] : [])),
        createProductType: mock("createProductType", (body) => Object.assign({ id: 40 }, body)),
        updateProductType: mock("updateProductType", (id, body) => Object.assign({}, PI, body, { id })),
        retireProductType: mock("retireProductType", (id) => ({ id, status: "deleted" })),
        lookupProductBarcode: mock("lookupProductBarcode", () => ({ found: false, productType: null })),
        addProductBarcode: mock("addProductBarcode", (typeId, code, codeType) => ({ id: 70, product_type_id: typeId, code, code_type: codeType })),
        removeProductBarcode: mock("removeProductBarcode", (id) => ({ id, status: "deleted" })),
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
    const event = { preventDefault() {}, stopPropagation() {} };
    if (b.props.onClick) b.props.onClick(event);
    else {
      const form = all(tree, (n) => n.tag === "form" && all(n, (c) => c === b).length)[0];
      assert(form && b.props.type === "submit", "button has no action: " + label);
      form.props.onSubmit(event);
    }
    await ticks(); draw();
  };
  const byAria = (label) => { const f = all(tree, (n) => n.props && n.props["aria-label"] === label)[0]; assert(f, "missing control: " + label); return f; };
  const labelled = (label) => {
    const lab = all(tree, (n) => n.tag === "label" && n.children.some((c) => c.tag === "span" && text(c) === label))[0];
    assert(lab, "missing field: " + label);
    return all(lab, (n) => n.tag === "input" || n.tag === "select" || n.tag === "textarea")[0];
  };
  const type = async (field, value) => { field.props.onChange({ target: { value } }); await ticks(2); draw(); };
  // Enter in a field submits its form, as a keyboard-wedge scanner does.
  const enter = async (field) => {
    const form = all(tree, (n) => n.tag === "form" && all(n, (c) => c === field).length)[0];
    assert(form, "field is not in a form");
    form.props.onSubmit({ preventDefault() {} }); await ticks(); draw();
  };
  const alertText = () => all(tree, (n) => n.props && n.props.role === "alert").map(text).join(" | ");
  const statusText = () => all(tree, (n) => n.props && n.props.role === "status").map(text).join(" | ");
  const sheetTitle = () => all(tree, (n) => n.props && n.props.role === "dialog").map((d) => d.props["aria-label"])[0] || null;
  const callsOf = (name) => calls.filter((c) => c.name === name);
  const listNames = () => all(tree, (n) => n.tag === "button" && /inv-cat-row/.test(n.props.className || "")).map((b) => text(all(b, (c) => c.props && c.props.className === "inv-loc-pick__name")[0]));
  const h2 = () => all(tree, (n) => n.tag === "h2").map(text)[0];

  draw(); await ticks(); draw();
  // 1. empty catalog: honest empty state, nothing seeded.
  plan.productTypes = [page([], 0)];
  await click("Catalog", true);
  await ticks(); draw();   // the tab loads from its mount effect
  same(callsOf("productTypes")[0].args, [{ q: "", kind: "", limit: 50, offset: 0 }]);
  assert(all(tree, (n) => n.tag === "h3" && text(n) === "No product types yet").length, "empty catalog explains itself");
  assert(!callsOf("createProductType").length && !callsOf("addProductBarcode").length, "empty state never seeds data");

  // 2. list: display names (bare "400" reads with its brand), byline, kind tag, paging.
  await click("All kinds", true);
  same(listNames(), ["Raspberry Pi 400", "Ubiquiti U7 Pro"]);
  assert(all(tree, (n) => n.props && n.props.className === "muted" && text(n) === "Raspberry Pi · 400 · SBC").length, "brand/model/category are secondary text");
  plan.productTypes = [page([OFFICE], 3, false, null)];
  await click("Show more (2 of 3)");
  same(callsOf("productTypes").slice(-1)[0].args[0], { q: "", kind: "", limit: 50, offset: 2 });
  same(listNames(), ["Raspberry Pi 400", "Ubiquiti U7 Pro", "Office 2024"]);
  assert(!buttons("Show more").length, "pager hides when the last page is in");

  // 3. kind filter and search (debounced typing, Enter searches now).
  plan.productTypes = [page([OFFICE], 1)];
  await click("Software", true);
  same(callsOf("productTypes").slice(-1)[0].args[0], { q: "", kind: "software", limit: 50, offset: 0 });
  same(listNames(), ["Office 2024"]);
  plan.productTypes = [page([], 0)];
  const search = byAria("Find a product type by name, brand, model or category");
  await type(search, "visio");
  assert(timers.filter(Boolean).length === 1, "typing schedules one debounced search");
  await enter(byAria("Find a product type by name, brand, model or category"));
  same(callsOf("productTypes").slice(-1)[0].args[0], { q: "visio", kind: "software", limit: 50, offset: 0 });
  assert(timers.every((t) => t === null), "Enter cancels the pending debounce");
  assert(all(tree, (n) => n.props && n.props.className === "inv-loc__none" && /No product types match “visio” in Software/.test(text(n))).length, "filtered empty result names the filter");
  await type(byAria("Find a product type by name, brand, model or category"), "");
  timers.filter(Boolean).forEach((fn) => fn()); timers.length = 0; await ticks(); draw();
  await click("All kinds", true);

  // 4. detail: specs as rows, barcodes listed, legacy link noted.
  await click("Raspberry Pi 400");
  same(callsOf("getProductType").slice(-1)[0].args, [1]);
  assert.strictEqual(h2(), "Raspberry Pi 400");
  const specs = all(tree, (n) => n.tag === "dl" && n.props.className === "inv-cat__specs")[0];
  same(all(specs, (n) => n.tag === "div").map(text), ["ram4 GB", "cores4", "gpio{\"pins\":40}"]);
  assert(byAria("Codes for Raspberry Pi 400") && text(byAria("Codes for Raspberry Pi 400")).indexOf("0123") >= 0, "live barcodes are listed");
  assert(/Linked to a component model/.test(text(tree)), "legacy-linked type says edits reach the model");

  // 5. edit: unchanged → no call; changed fields only + updated_at; JSON types kept.
  await click("Edit", true);
  assert.strictEqual(sheetTitle(), "Edit Raspberry Pi 400");
  await click("Save changes");
  assert.strictEqual(callsOf("updateProductType").length, 0, "an unchanged edit sends nothing");
  assert.strictEqual(sheetTitle(), null); forget("ProductTypeSheet");
  await click("Edit", true);
  assert(/Lists show this as “Raspberry Pi 400”/.test(text(tree)), "a bare-model name explains how it reads");
  await click("Use “Raspberry Pi 400”");
  assert.strictEqual(labelled("Display name").props.value, "Raspberry Pi 400", "one tap adopts the readable name (not saved yet)");
  assert.strictEqual(callsOf("updateProductType").length, 0, "adopting the name commits nothing");
  await type(labelled("Display name"), "Pi 400 keyboard");
  await type(byAria("Spec 2 value"), "8");
  plan.updateProductType = [{ error: apiError("conflict", "Product type changed since it was read.") }];
  plan.getProductType = [detail(Object.assign({}, PI, { name: "Pi 400 kit", updated_at: "2026-11-03 11:00:00" }))];
  await click("Save changes");
  same(callsOf("updateProductType")[0].args, [1, { name: "Pi 400 keyboard", specs: { ram: "4 GB", cores: 8, gpio: { pins: 40 } }, updated_at: STAMP }]);
  assert(/changed elsewhere/.test(alertText()), "stale edit says it changed elsewhere");
  assert.strictEqual(labelled("Display name").props.value, "Pi 400 kit", "form reloads the latest version");
  assert.strictEqual(byAria("Spec 2 value").props.value, "4", "reloaded specs replace the stale edit");
  await type(byAria("Spec 3 value"), "{\"pins\":26}");
  await click("Add spec");
  await type(byAria("Spec 4 name"), "licence_key");
  await type(byAria("Spec 4 value"), "XXXX");
  await click("Save changes");
  assert(/look like secrets/.test(alertText()) && callsOf("updateProductType").length === 1, "secret-like spec names are refused before any call");
  byAria("Remove spec licence_key").props.onClick(); await ticks(); draw();
  await click("Save changes");
  same(callsOf("updateProductType")[1].args, [1, { specs: { ram: "4 GB", cores: 4, gpio: { pins: 26 } }, updated_at: "2026-11-03 11:00:00" }]);
  assert.strictEqual(sheetTitle(), null); forget("ProductTypeSheet");
  assert(callsOf("inventory").length >= 2, "an edit re-reads inventory (legacy model labels may change)");

  // 6. barcodes: GTIN check digit, attach, duplicate explained, remove with confirm.
  await type(labelled("Attach a code"), "4006381333932");
  await type(labelled("Code type"), "gtin");
  await click("Attach", true);
  assert(/valid GTIN/.test(alertText()) && !callsOf("addProductBarcode").length, "bad GTIN check digit is caught client-side");
  await type(labelled("Attach a code"), " 4006381333931 ");
  await click("Attach", true);
  same(callsOf("addProductBarcode")[0].args, [1, "4006381333931", "gtin"]);
  plan.addProductBarcode = [{ error: apiError("duplicate_barcode", "That barcode is already attached to a live product type.") }];
  plan.lookupProductBarcode = [{ found: true, productType: U7, barcode: { id: 9, code: "SKU-U7", code_type: "sku" } }];
  await type(labelled("Attach a code"), "sku-u7");
  await type(labelled("Code type"), "sku");
  await click("Attach", true);
  assert(/already attached to “Ubiquiti U7 Pro”/.test(alertText()), "duplicate code names the type that owns it");
  await click("Remove", true);
  assert(byAria("Confirm removing 0123"), "remove asks first");
  assert(!callsOf("removeProductBarcode").length);
  await click("Remove", true, 0);
  same(callsOf("removeProductBarcode")[0].args, [7]);

  // 7. lookup: wedge Enter → exact match jumps to the type; a miss only prefills (D3).
  plan.lookupProductBarcode = [{ found: true, productType: U7, barcode: { id: 9, code: "SKU-U7", code_type: "sku" } }];
  await type(byAria("Scan or type a barcode"), " sku-u7 ");
  await enter(byAria("Scan or type a barcode"));
  same(callsOf("lookupProductBarcode").slice(-1)[0].args, ["SKU-U7"]);
  assert.strictEqual(h2(), "Ubiquiti U7 Pro", "a found code opens its type");
  assert.strictEqual(byAria("Scan or type a barcode").props.value, "", "scan box clears for the next scan");
  const before = calls.length;
  await type(byAria("Scan or type a barcode"), "4006381333931");
  await enter(byAria("Scan or type a barcode"));
  assert(/No product type has the code 4006381333931/.test(statusText()), "a miss is reported");
  assert(calls.slice(before).every((c) => c.name === "lookupProductBarcode"), "a miss writes nothing");
  await click("Use for Ubiquiti U7 Pro");
  assert.strictEqual(labelled("Attach a code").props.value, "4006381333931");
  assert.strictEqual(labelled("Code type").props.value, "gtin", "a valid GTIN is recognised");
  assert.strictEqual(callsOf("addProductBarcode").length, 2, "prefill alone attaches nothing");
  await click("New product type with this code");
  assert.strictEqual(sheetTitle(), "New product type");
  assert.strictEqual(labelled("Barcode (optional)").props.value, "4006381333931");
  await click("Cancel", true); forget("ProductTypeSheet");
  assert(!callsOf("createProductType").length, "cancelling the prefilled sheet creates nothing");

  // 8. create: name from brand+model when blank, image URL checked, barcode sent.
  await click("New product type with this code");
  await type(labelled("Brand"), "Raspberry Pi");
  await type(labelled("Model"), "5");
  assert(/Will be saved as “Raspberry Pi 5”/.test(text(tree)), "blank display name previews the derived name");
  await type(labelled("Image link (optional)"), "javascript:alert(1)");
  await click("Add product type");
  assert(/http/.test(alertText()) && !callsOf("createProductType").length, "non-http image links are refused");
  await type(labelled("Image link (optional)"), "");
  await type(labelled("Kind"), "component");
  await click("Add product type");
  same(callsOf("createProductType")[0].args[0], { kind: "component", name: "Raspberry Pi 5", brand: "Raspberry Pi", model: "5", category: null, image_url: null, notes: null, barcode: { code: "4006381333931", code_type: "gtin" } });
  same(callsOf("getProductType").slice(-1)[0].args, [40], "the new type opens");
  forget("ProductTypeSheet");

  // 9. software licence guidance; no secret fields offered.
  plan.productTypes = [page([OFFICE], 1)];
  await click("Software", true);
  await click("Office 2024");
  assert(/Licence keys are never stored/.test(text(tree)), "software types say keys are never stored");
  assert(all(tree, (n) => n.tag === "dl" && n.props.className === "inv-cat__specs").length === 0 && /No specifications recorded/.test(text(tree)), "PHP's empty [] specs render as none");

  // 10. retire (D5: allowed in use): confirm, DELETE, list reload, detail cleared.
  const listLoads = callsOf("productTypes").length;
  await click("Retire…");
  assert(/Items already recorded keep this type; it leaves pick lists/.test(text(byAria("Confirm retiring Office 2024"))));
  await click("Retire product type");
  same(callsOf("retireProductType")[0].args, [3]);
  assert(callsOf("productTypes").length > listLoads, "retire reloads the list (retired types drop out)");
  assert(all(tree, (n) => n.tag === "h3" && text(n) === "Choose a product type").length, "detail clears after retire");

  // 11. existing tabs still render.
  await click("Locations", true);
  assert(buttons("Home").length, "locations tab still renders");
  await click("Components", true);
  assert(/components/i.test(text(tree)), "components tab still renders");

  console.log("inventory catalog list/search/filter, create/edit/retire, barcode attach/remove and lookup-prefill flows passed");
}
main().catch((e) => { console.error(e && e.stack || e); process.exitCode = 1; });
