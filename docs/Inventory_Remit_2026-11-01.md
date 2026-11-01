# Inventory & Labels — Remit Re-baseline (2026-11-01)

Status: **APPROVED 2026-11-01** (Jason) with the decisions recorded in §6.
Inputs: Jason's stated intent (2026-11-01, after using Tags & Scan); Luna (gpt-6-luna)
read-only gap analysis of `main` (no live DB queried). Observed vs inferred is marked.

## 1. Why

The shipped feature (migrations 014/015, `inventory.php`, `inv_codes.php`, `Label.php`,
Tags & Scan in `screens8.jsx`) is a **network-asset tagging layer** over the hardware
catalog. The intended product is a **technology inventory**: hardware, peripherals,
components, software/licences and accessories, driven by a barcode scanner but fully usable
without one.

## 2. Remit (requirements + acceptance)

| # | Requirement | Accepted when |
|---|---|---|
| R1 | Scan a commercial UPC/EAN/SKU → web lookup → product type created (or matched) → create N items | Scanning an unknown retail barcode shows a looked-up candidate (name, brand, model, image/category where available); confirm creates the product type once (deduped by barcode); "add items" creates N items with location in one step. A barcode already on a product type goes straight to "add items". No match → manual form pre-filled with the barcode. |
| R2 | Manual entry for non-scannable things | Product types and items can be created by hand, including software (licence/seat/expiry fields) and things with no make/model. |
| R3 | Scan a SolariNet QR/barcode → load the record → edit or delete | Scanning any system code (item, location, project) opens that record with Edit and Delete (soft-delete, audited) in place. |
| R4 | Hierarchical locations: **Property → Floor → Room → Section → Storage unit** | Levels are enforced on create/move; sibling names are unique; moves can't create cycles; full path shown everywhere ("Painted Fern › 2nd › Office › North wall › Bin 3"); locations are editable, movable and mergeable in the UI. |
| R5 | Labels for the three thermal printers (MakeID EP53, MakeID E1, Nemonic MIP-201W) | Four label templates — barcode, QR, asset tag, full inventory entry — rendered at each printer's verified DPI/stock size; printable via the chosen transport (§4.6). |
| R6 | Searchable, editable, indexable without scanner or printer | One search across product types, items, locations, codes and receipts; every record reachable and editable from the iPad UI. |

Platform: iPad Safari first, iPhone second. Scanner = USB/BT HID keyboard wedge (already how the scan field works).

## 3. Gap summary (from the analysis)

| # | Today | Key gap |
|---|---|---|
| R1 | PARTIAL — unknown codes open a receive form; model chosen by hand | **No product lookup at all**; no dedupe by barcode |
| R2 | PARTIAL — "New model"/"New component" forms | Make/model hardware catalog; software and model-less items don't fit |
| R3 | PARTIAL — registered codes resolve to a card | Cards offer print/move only — **no edit/delete** |
| R4 | PARTIAL — generic `parent_id` tree, 13 kinds | No level enforcement, no `section`, **sibling uniqueness was dropped in migration 001** (how two "Painted Fern" roots exist — inferred), move skips the cycle guard, no edit UI |
| R5 | PARTIAL — PNG/PDF renderer, 3 profiles | **No printer transport**; no full-entry template; EP53 profile says 203 dpi, MakeID's manual says 300 |
| R6 | PARTIAL — component search on Inventory screen | No unified search; locations create-only; codes UI scan-oriented |

## 4. Proposed design

### 4.1 Data model — broaden the catalog, don't fork it
Because the inventory is almost entirely technology, **one catalog**, not a separate household module:

- **`product_types`** (new): `kind` (hardware · peripheral · component · software · accessory · consumable · other), name, brand, model (both optional), category, specs JSON, notes, image URL, lookup provenance.
- **`product_barcodes`** (new): GTIN/UPC/EAN/SKU → product type, unique per live code (the dedupe key for R1).
- **`hardware_models`** stays as the IT/provisioning extension (arch etc.), linked 1:1 by `product_type_id`; existing rows are backfilled into `product_types`, preserving IDs and links.
- **Items**: `hardware_units` gains `product_type_id` and remains the item table (serial, asset tag, location, quantity, status, cost, receipt), so existing receipts, projects and barcodes keep working. Software items add licence fields: seats, expiry, assigned-to, and a **reference** to where the licence key lives (keys themselves are secrets — not stored in the SoR).
- Barcode `subject_table` validation extended to product types.

### 4.2 Locations
Add levels `property` and `section`; map existing `site`→property, `cabinet/shelf/drawer/bin/container`→storage unit. Enforce allowed parent→child transitions (Property→Floor→Room→Section→Storage unit, with Section optional), live sibling-name uniqueness (restored as a constraint), cycle-safe move, and a merge action (moves children + items, retires the duplicate). Full-path rendering from one helper.

**Storage units are working containers, not fixed furniture.** Jason has hundreds of bags,
crates, boxes and trays that collect a project's parts, travel with it through its lifecycle,
and are repurposed after deployment. So:

- Adding locations at any level is a first-class UI action, with **bulk create** for storage
  units ("12 × crate, CR-001…CR-012") and labels printed in the same step.
- Container **type** (bag, crate, box, tray, bin, …) is an operator-editable list, not a code enum.
- A storage unit can be **assigned to a project**, moved anywhere (including off-site/in transit),
  and **emptied and released** for reuse — its code and label stay with the physical container.
- Scanning a container shows its contents and project; items can be scanned into/out of it.

### 4.3 Scan dispatch (one field, keyboard wedge)
1. Exact registry match → open the record (R3).
2. System-format code not registered (`AKO-`/`LOC-`/`PRJ-`/`SI-`) → "unknown SolariNet code" (never sent to the web).
3. Valid GTIN (UPC-A 12, EAN-13, EAN-8, GTIN-14, check digit verified) → `product_barcodes` match → add items; else web lookup (R1).
4. Anything else → manual product form with the code attached as SKU.

### 4.4 Product lookup (server-side PHP, never from the browser)
Provider adapter with a cache table keyed by normalized GTIN; results shown as a **candidate the operator confirms**. Start with **UPCitemdb free tier** (no key, 100 lookups/day) with **Open Products Facts** as fallback (free, needs a User-Agent, 15 reads/min). No paid source. When neither returns a match, the candidate form offers a **"Search the web"** link (opens a Google search for the code in a new tab) so the operator can fill the form by hand — no search API involved. Rate limits and coverage are per the providers' published docs — **UNVERIFIED** against your products until we test a sample.

### 4.5 Search & editing
One search endpoint over product types, items, locations, codes and receipts (MariaDB FULLTEXT + exact-code match); results open editable record views. Locations get a tree browser with edit/move/merge.

### 4.6 Printing
Templates: barcode-only, QR-only, asset tag, full inventory entry (name, type, location path, serial, code). Printer profiles corrected from a **physical test print** on each printer.

Transport (decided): labels are **images** sized for each printer — copy/paste, share sheet or
save — printed from the vendor app on the iPad/iPhone. No direct-print bridge. Exact image-import
behaviour per vendor app is UNVERIFIED until the physical test prints.

## 5. Phasing (each phase shippable, cross-lab reviewed)

| Phase | Delivers | Requirements |
|---|---|---|
| I1 | Catalog: `product_types` + barcodes + backfill; locations levels/uniqueness/move/merge; storage-unit bulk create, types list, project assignment/release; merge the Painted Fern duplicate | R2, R4 |
| I2 | Scan dispatch + record view with edit/delete for items, locations, projects | R3 |
| I3 | Product lookup + "scan → confirm → add N items" flow | R1 |
| I4 | Unified search + editable browse views | R6 |
| I5 | Label templates + verified printer profiles + share-to-vendor-app | R5 |

## 6. Decisions (Jason, 2026-11-01)

1. **Data model:** one broadened catalog (`product_types` over `hardware_models`). — accepted
2. **Lookup:** free sources only (UPCitemdb + Open Products Facts), confirm-before-save, plus a manual "Search the web" fallback link. — accepted
3. **Locations:** Property / Floor / Room / Section / Storage unit, flexible: the operator adds entries at any level from the UI; storage units are numerous, reusable, project-assignable containers (§4.2). Section optional; sibling names unique.
4. **Painted Fern duplicates (9103 / 9104):** merge into one Property — accepted; what hangs off each is shown before the merge runs.
5. **Printing:** images to copy/paste or print from the interface device; no direct-print bridge. — accepted
6. **Software licences:** reference only (where the licence lives); no keys or licence text stored. — accepted
