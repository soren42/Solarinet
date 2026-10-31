# Provisioning UI — Images & Software Stacks (design notes)

`Status: DRAFT · 2026-10-31 · owner: Jason C. Kay (PM) · scribe: Lead (Opus 5.5)`

Closes the long-standing gap: netboot/PXE/TFTP and Ansible exist and are live,
but there is no UI to warehouse, manage, maintain, and deploy OS images, and no
frontend for composing architecture-independent software stacks.

Operator sketch: `IMG_1662.png` (T3 attachment, 2026-10-31; not committed).

---

## 1. What already exists (observed 2026-10-31)

| Piece | Location | State |
|---|---|---|
| Netboot | benzene 10.5.2.50 `/srv/solari-provision/` | LIVE — UniFi DHCP next-server → benzene; `solari-tftp` active, nginx :8080 active, `/ipxe/boot.ipxe` → 200; `solari-netboot` (proxyDHCP) inactive by design |
| iPXE loaders | `deploy/fleet/netboot/build-ipxe-loaders.sh` | Chain script must be embedded (UniFi advertises one filename) |
| Unattended installers | `deploy/fleet/netboot/configs/` | openSUSE AutoYaST, Debian preseed, Ubuntu autoinstall + `installers/solari-firstboot.sh` |
| Per-host boot profile | `deploy/fleet/netboot/ipxe/mac-profile.ipxe.tmpl` | MAC-keyed |
| Ansible | `deploy/fleet/ansible/` | Roles: `base-os`, `common-stack`, `dev-tools` (zsh/p10k/starship), `directory-join`, `shared-home`, `solarinet-client`; single package list in `group_vars/all.yml` |
| Bridge | `deploy/fleet/fleet-provision.sh`, `dashboard/api/routes/provisioning.php` | Operator-gated `FLEET_PROVISION` / `FLEET_IMAGE` |
| UI | Provisioning screen | Only a "Provision a new system" panel |
| Never exercised | — | A real bare-metal wipe + install |

## 2. Operator sketch (as drawn)

- **Two panels.** *Images* (left) and *Software* (right); empty header band on top.
- **Images.** Accordion by distro: Debian, SUSE, Fedora Core, Ubuntu. Each distro
  expands to arch rows (ARM, x86-64, x86-32, RISC-V). Every row has
  **deploy / edit / update / delete**.
- **Software.** Stacks (Core, Web, AI) with a checkbox each, plus **edit / delete**.
- **Implied flow (inferred).** Tick stacks, then press deploy on an image row.

## 3. Agreed revisions (operator approved 2026-10-31)

1. **Target selector in the header band.** Pick a machine from SoR inventory.
   - It shows the machine's MAC and arch (from its hardware model).
   - Image rows whose arch doesn't match grey out.
   - A single **Deploy** sends *target + image + checked stacks*.
   - Reason: page-global checkboxes leave it unclear what a deploy includes.
2. **Stacks apply to existing machines; one Deploy for the whole batch.**
   - A mode toggle on the target chooses **Reimage** (wipe + install, then
     stacks at first boot) or **Apply stacks only** (Ansible over SSH, no
     reinstall). Applying to live machines is the main value of Ansible here.
   - Several stacks are ticked at once and pushed as **one ordered run**.
     Dependencies are pulled in automatically, Core is always first, and the
     run order is shown before you confirm.
   - The Software pane has **no per-stack action buttons** beyond edit and
     delete. A single **Deploy** in the sticky bottom bar sends
     *target + image (reimage mode) + all selected stacks*.
     (Revised 2026-10-31 per operator; supersedes per-stack "Apply to target".)
3. **Release level between distro and arch.**
   - Examples: Debian 12 vs 13; Tumbleweed vs Leap.
   - Each arch row shows version, fetched-at date, and checksum status.
   - **Update** = re-fetch from upstream. Stale images are visibly flagged.
4. **Show only real distro × arch combinations, and label how each boots.**
   - Boot methods: iPXE / Pi-native netboot / write-to-media.
   - Split **ARM** into `arm64` / `armhf`; Pis do not use the iPXE path.
   - **x86-32**: mostly empty upstream. Recent Debian, Ubuntu and Fedora dropped
     i386/i686 installers (*inferred; verify per release before building the row*).
   - **RISC-V**: Debian 13 riscv64 is an official port. Netboot on actual
     hardware is *UNVERIFIED*.
   - **"Fedora Core" → "Fedora".** Fedora CoreOS is a different, immutable
     product that installs via Ignition. Pick one explicitly.
5. **Stack editor is where arch-independence comes from.**
   - A stack is a list of *logical* packages, with a per-distro name map wherever
     names differ (e.g. SUSE `apache2` + `php8` vs Debian `apache2` + `php`).
   - **Item types** (operator, 2026-10-31: at least native packages, shell
     scripts, and source builds):
     | Type | What it is |
     |---|---|
     | `pkg` | Distro-native package, with a name per family |
     | `repo` | Third-party repository plus signing key, added before `pkg` items |
     | `artifact` | Upstream release binary or tarball, per arch, sha256-pinned (AI CLIs, T3 Code) |
     | `script` | Download/install shell script. Body is inline, or a remote URL with a mandatory sha256. Has an idempotency guard (`creates` / `check` command / version regex) |
     | `build` | Source build (cmake / make / autotools / meson / custom). Pinned git ref or checksummed tarball; build deps resolve through the package map. Every phase, install included, runs unprivileged into a DESTDIR stage; root only copies the staged prefix (no symlink traversal, setuid dropped). Root-time install logic must be a `script` item |
     | `role` | Existing Ansible role |

   - **Software pool (operator-approved 2026-10-31, before 029 went live).**
     Every installable item is defined **once** in a shared pool: its type,
     per-family names, spec, pins and per-arch artifacts. A stack is an ordered
     list of *references* to pool entries plus its stack dependencies.
     - The pool is the family name map, which retires the "Core doubles as the
       map" workaround.
     - Duplicate definitions are impossible: there is one live entry per
       logical name.
     - A pin is bumped once and applies to every stack that uses it.
     - Pool entries carry `conflicts_with`. The generator rejects a selection
       containing conflicting entries before anything runs.
     - An entry shared by two selected stacks is installed once, at its
       first position in run order.
     - UI: the stack editor picks entries from the pool, and a Software pool
       view manages the entries (P4).

     Default order within a stack is repo → pkg → artifact → build → script → role,
     and an explicit item order overrides it. An unpinned remote script is
     rejected; there is no blind `curl | sh`.
   - Stacks may depend on others (AI → Core).
   - **Core is always included** (locked on).
   - Stacks are stored as data in the SoR. The backend renders them to Ansible
     vars; YAML is never edited in the browser. This replaces the single
     `group_vars/all.yml` list.
6. **Safety.**
   - Delete requires confirmation.
   - Deploy and apply are audit-logged.
   - Both use the same operator gate as the existing provisioning panel.

## 4. Phase plan (scoped 2026-10-31)

Mockup: `docs/mockups/provisioning-images-stacks.html` (static, sample data,
no network calls). Open it in a browser and click through it before P1 starts.

| Phase | Deliverable | Exit check |
|---|---|---|
| **P1 · Data** | SoR migration 029 (as built): `prov_images`, `prov_software` (pool: one live row per logical name, typed + validated spec), `prov_software_artifacts`, `prov_software_requires`, `prov_software_conflicts`, `prov_stacks`, `prov_stack_members`, `prov_stack_deps`, `prov_runs` (audit). Seeded from today's `group_vars/all.yml` + `solari_pkg_map`. | Migration applies on a scratch DB (never `solarinet`/`sor`); seed reproduces today's package list |
| **P2 · Generator** | Stack closure → ordered Ansible vars + role list per target family/arch. Unmapped item = hard error naming item + family. | Golden-file tests per family; render for steel/Tumbleweed matches hand-written vars |
| **P3 · Bridge + API** | New operator-gated verbs on the existing queue: `IMAGE_REFRESH` (wraps `fetch-images.sh`, verifies checksum, regenerates boot menu), `PROV_STAGE` (per-MAC profile + first-boot stacks), `STACK_APPLY` (dry-run diff, then apply). Routes under `provisioning.php`, behind default-deny + CSRF. | Verb dispatch tests; no PHP path reaches the host except via the queue |
| **P4 · UI** | Mockup → dashboard screen (vanilla JSX, no build step). Retire `catalog.json`; fix the stale "Live PXE activates once…" note. | Operator click-through on the live dashboard |
| **P5 · Review** | Cross-lab review of P1–P4 (Luna / gpt-6-luna). | Findings resolved or explicitly deferred |
| **P6 · Bare metal** | steel (ZimaBlade, x86_64, Debian 12) → openSUSE Tumbleweed + Core, staged through the new UI. Clear the stale `steel` known_hosts entry, enroll the agent. | Machine boots Tumbleweed, agent reports, Core verified on-host |
| **P7 · DNS role** | Port the BIND secondary helper to SUSE layout as the `dns-secondary` role; apply via Apply-stacks-only. Do **not** restart steel's old docker bind9 (stale zones). | AXFR from primary, serial match, sorsync registration |

P6 is the first-ever real wipe + install and is destructive. Operator granted
standing authorization on 2026-10-31 for development and deployment of every
phase, provided each one passes cross-lab review and the plan is followed. P6
is announced before the wipe but does not wait for a further go. A material
deviation from this plan goes back to the operator.

## 4a. Deferred (operator, 2026-10-31)

- **Containers (Podman Quadlet) as a stack item type.** Stretch goal, taken up
  once P1–P7 are finalized. Until then a container is deployed via a `role`.
- **Language package managers** (pipx / cargo / npm -g / go install). No
  dedicated type; the operator avoids them. Use a `script` item with a `creates`
  guard when one is unavoidable.

## 5. Open questions

- Fedora vs Fedora CoreOS (item 4).
- Does x86-32 stay as a row at all?
- Multi-target deploy: one machine at a time, or batches?
