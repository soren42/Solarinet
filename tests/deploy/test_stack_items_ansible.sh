#!/usr/bin/env bash
# ===========================================================================
# test_stack_items_ansible.sh — stackgen → stack-items role, end to end,
# unprivileged, on localhost, inside a throwaway temp dir.
#
#   1. syntax-check playbooks/site.yml with and without a render;
#   2. render a synthetic stack (artifact, custom build from a sha256-pinned
#      tarball, inline script with creates+check+version_check, role) with
#      deploy/fleet/stackgen.py for THIS host's family/arch;
#   3. run the real stack-items role against localhost (connection local,
#      become disabled, all paths under the temp dir) and assert the effects;
#   4. run it again and require changed=0 (guards make it idempotent);
#   5. version_check mismatch re-runs; tampered sha256 is refused;
#   6. review regressions: a hand-edited render with a path-escaping version
#      is refused by the role before anything is created, and a guard check
#      is executed as argv (shell metacharacters are inert).
#
# Not exercised here (need root / network): pkg and repo steps.
# Requires ansible-playbook on PATH (or ~/.local/bin); skips with exit 0 if
# absent unless STACK_ITEMS_REQUIRE_ANSIBLE=1.
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ANS="$ROOT/deploy/fleet/ansible"
export PATH="$HOME/.local/bin:$PATH"
if ! command -v ansible-playbook >/dev/null 2>&1; then
  if [ "${STACK_ITEMS_REQUIRE_ANSIBLE:-0}" = "1" ]; then
    echo "FAIL: ansible-playbook not found" >&2; exit 1
  fi
  echo "SKIP: ansible-playbook not found"; exit 0
fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
# Isolate from the operator's ~/.netrc (Ansible's url layer parses it).
: > "$T/netrc"; export NETRC="$T/netrc"
pass=0
ok() { pass=$((pass + 1)); echo "ok - $*"; }
die() { echo "FAIL: $*" >&2; exit 1; }

# ---- 1. syntax checks -------------------------------------------------------
( cd "$ANS" && ansible-playbook -i localhost, playbooks/site.yml --syntax-check >/dev/null ) \
  || die "site.yml syntax-check (legacy)"
ok "site.yml syntax-check (legacy)"
python3 "$ROOT/deploy/fleet/stackgen.py" --from-json "$ROOT/tests/deploy/fixtures/stacks_seed.json" \
  --family suse --arch x86_64 --stack dev-tools > "$T/seed.json"
( cd "$ANS" && ansible-playbook -i localhost, playbooks/site.yml --syntax-check -e "@$T/seed.json" >/dev/null ) \
  || die "site.yml syntax-check (stack render)"
ok "site.yml syntax-check (stack render)"

# ---- 2. synthetic definitions + render --------------------------------------
case "$(. /etc/os-release; echo "${ID} ${ID_LIKE:-}")" in
  *suse*) FAMILY=suse ;; *debian*|*ubuntu*) FAMILY=debian ;; *fedora*|*rhel*) FAMILY=fedora ;;
  *) echo "SKIP: unrecognised local OS family"; exit 0 ;;
esac
case "$(uname -m)" in
  x86_64) ARCH=x86_64 ;; aarch64) ARCH=arm64 ;; armv7l|armv6l) ARCH=armhf ;;
  riscv64) ARCH=riscv64 ;; i386|i686) ARCH=i386 ;; *) echo "SKIP: arch"; exit 0 ;;
esac

mkdir -p "$T/src/hello-1.0" "$T/roles/smoke-role/tasks" "$T/prefix" "$T/out" "$T/gitsrc"
# Git source: pin the FIRST commit; a later commit on the branch must not be built.
git -C "$T/gitsrc" init -q
printf 'pinned\n' > "$T/gitsrc/marker"
git -C "$T/gitsrc" add marker
git -C "$T/gitsrc" -c user.name=t -c user.email=t@invalid commit -q -m pinned
GIT_PIN="$(git -C "$T/gitsrc" rev-parse HEAD)"
printf 'moved\n' > "$T/gitsrc/marker"
git -C "$T/gitsrc" -c user.name=t -c user.email=t@invalid commit -q -am moved
printf '#!/bin/sh\necho hello\n' > "$T/src/hello-1.0/hello.sh"
tar -C "$T/src" -czf "$T/hello-1.0.tar.gz" hello-1.0
printf 'artifact-payload\n' > "$T/tool.bin"
TAR_SHA="$(sha256sum "$T/hello-1.0.tar.gz" | cut -d' ' -f1)"
ART_SHA="$(sha256sum "$T/tool.bin" | cut -d' ' -f1)"
cat > "$T/roles/smoke-role/tasks/main.yml" <<EOF
---
- name: smoke role marker
  ansible.builtin.copy:
    content: "role ran\n"
    dest: "$T/out/role.txt"
    mode: "0644"
EOF

T="$T" TAR_SHA="$TAR_SHA" ART_SHA="$ART_SHA" GIT_PIN="$GIT_PIN" python3 - > "$T/defs.json" <<'PY'
import json, os
t = os.environ["T"]
same = {"suse": "=", "debian": "=", "fedora": "="}
print(json.dumps({"stacks": [{"slug": "smoke", "items": [
    {"logical_name": "smoke role", "item_type": "role", "names": same, "ansible_role": "smoke-role"},
    {"logical_name": "marker script", "item_type": "script", "names": same, "version_pin": "2.1",
     "spec": {"body": f'echo "2.1" > {t}/out/version.txt\necho "$GREETING" > {t}/out/greeting.txt',
              "env": {"GREETING": "hi from env"}, "run_as": "admin",
              "creates": f"{t}/out/greeting.txt", "check": ["cat", f"{t}/out/version.txt"],
              "version_check": r"^([0-9.]+)$"}},
    {"logical_name": "hello", "item_type": "build", "names": same, "version_pin": "1.0",
     "spec": {"source": {"tarball": f"file://{t}/hello-1.0.tar.gz", "sha256": os.environ["TAR_SHA"]},
              "system": "custom", "prefix": f"{t}/prefix",
              "commands": 'mkdir -p "$PREFIX/bin" && cp hello.sh "$PREFIX/bin/hello" && chmod 0755 "$PREFIX/bin/hello"',
              "creates": f"{t}/prefix/bin/hello"}},
    {"logical_name": "gitthing", "item_type": "build", "names": same, "version_pin": os.environ["GIT_PIN"],
     "spec": {"source": {"git": f"file://{t}/gitsrc", "tag": "display-only"}, "system": "custom",
              "prefix": f"{t}/prefix", "commands": 'cp marker "$PREFIX/git-marker"',
              "creates": f"{t}/prefix/git-marker", "check": ["cat", f"{t}/prefix/git-marker"]}},
    {"logical_name": "tool", "item_type": "artifact", "names": same, "version_pin": "3.0",
     "artifacts": {"any": {"url": f"file://{t}/tool.bin", "sha256": os.environ["ART_SHA"]}}},
]}]}))
PY
python3 "$ROOT/deploy/fleet/stackgen.py" --from-json "$T/defs.json" --family "$FAMILY" --arch "$ARCH" \
  --stack smoke > "$T/render.json"
python3 - "$T/render.json" <<'PY' || die "render step order"
import json, sys
r = json.load(open(sys.argv[1]))
assert [s["type"] for s in r["solari_stack_steps"]] == ["artifact", "build", "build", "script", "role"], r["solari_stack_steps"]
PY
ok "render: artifact → build ×2 → script → role"

cat > "$T/play.yml" <<EOF
---
- hosts: localhost
  connection: local
  gather_facts: true
  roles:
    - role: stack-items
EOF
run_play() {
  ANSIBLE_ROLES_PATH="$ANS/roles:$T/roles" ANSIBLE_STDOUT_CALLBACK=default ANSIBLE_NOCOLOR=1 \
    ANSIBLE_LOCALHOST_WARNING=0 ANSIBLE_INVENTORY_UNPARSED_WARNING=0 \
    ansible-playbook -i localhost, "$T/play.yml" -e "@$T/render.json" \
      -e ansible_become=false -e "solari_stack_admin_user=$(id -un)" \
      -e "solari_stack_artifact_root=$T/opt" -e "solari_stack_work_root=$T/work" > "$T/run$1.log" 2>&1 \
    || { cat "$T/run$1.log" >&2; die "stack-items run $1"; }
}

# ---- 3. first run: effects --------------------------------------------------
run_play 1
[ "$(cat "$T/out/greeting.txt")" = "hi from env" ] || die "script env/body"
[ "$("$T/prefix/bin/hello")" = "hello" ] || die "custom build install"
cmp -s "$T/tool.bin" "$T/opt/smoke--tool/3.0/tool.bin" || die "artifact download"
[ -f "$T/out/role.txt" ] || die "role step"
[ "$(cat "$T/prefix/git-marker")" = "pinned" ] || die "git build did not use the pinned commit"
ok "first run applied artifact, builds (tarball + git at pinned commit, not branch tip), script (env), role"

# ---- 4. second run: idempotent ----------------------------------------------
run_play 2
grep -Eq 'localhost +: ok=[0-9]+ +changed=0 ' "$T/run2.log" || { cat "$T/run2.log" >&2; die "second run not idempotent"; }
ok "second run changed=0"

# ---- 5. guard: version mismatch re-runs the script ---------------------------
echo "1.9" > "$T/out/version.txt"
run_play 3
[ "$(cat "$T/out/version.txt")" = "2.1" ] || die "version_check did not trigger re-run"
ok "version_check mismatch re-ran the script"

# ---- 6. tampered artifact is refused -----------------------------------------
printf 'tampered\n' > "$T/tool.bin"
rm -rf "$T/opt"
if ANSIBLE_ROLES_PATH="$ANS/roles:$T/roles" ANSIBLE_LOCALHOST_WARNING=0 ANSIBLE_INVENTORY_UNPARSED_WARNING=0 \
   ansible-playbook -i localhost, "$T/play.yml" -e "@$T/render.json" -e ansible_become=false \
   -e "solari_stack_admin_user=$(id -un)" -e "solari_stack_artifact_root=$T/opt" \
   -e "solari_stack_work_root=$T/work" > "$T/run4.log" 2>&1; then
  die "tampered artifact was accepted"
fi
grep -qi 'checksum' "$T/run4.log" || { cat "$T/run4.log" >&2; die "tampered artifact failed for another reason"; }
ok "sha256 mismatch refused"

# ---- 7. review regression: path-escaping version refused by the role --------
printf 'artifact-payload\n' > "$T/tool.bin"
python3 - "$T/render.json" "$T/render-escape.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
r["solari_stack_artifacts"][0]["version"] = "../../escaped"
r["solari_stack_steps"] = [{"type": "artifact", "index": 0}]
json.dump(r, open(sys.argv[2], "w"))
PY
rm -rf "$T/opt" "$T/escaped"
if ANSIBLE_ROLES_PATH="$ANS/roles:$T/roles" ANSIBLE_LOCALHOST_WARNING=0 ANSIBLE_INVENTORY_UNPARSED_WARNING=0 \
   ansible-playbook -i localhost, "$T/play.yml" -e "@$T/render-escape.json" -e ansible_become=false \
   -e "solari_stack_admin_user=$(id -un)" -e "solari_stack_artifact_root=$T/opt/art" \
   -e "solari_stack_work_root=$T/work" > "$T/run5.log" 2>&1; then
  die "path-escaping version was accepted"
fi
grep -q 'unsafe id/version' "$T/run5.log" || { cat "$T/run5.log" >&2; die "escape refused for another reason"; }
[ ! -e "$T/escaped" ] && [ ! -e "$T/opt/escaped" ] || die "a path outside the artifact root was created"
[ -z "$(ls -A "$T/opt/art" 2>/dev/null)" ] || die "something was created under the artifact root before the refusal"
ok "role refuses a path-escaping version before creating anything"

# ---- 8. review regression: guard check is argv, not a shell -------------------
python3 - "$T/render.json" "$T/render-inject.json" "$T" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); t = sys.argv[3]
s = r["solari_stack_scripts"][0]
s["check"] = ["cat", f"{t}/out/version.txt; touch {t}/out/pwned"]
s["creates"] = None
r["solari_stack_steps"] = [{"type": "script", "index": 0}]
json.dump(r, open(sys.argv[2], "w"))
PY
ANSIBLE_ROLES_PATH="$ANS/roles:$T/roles" ANSIBLE_LOCALHOST_WARNING=0 ANSIBLE_INVENTORY_UNPARSED_WARNING=0 \
  ansible-playbook -i localhost, "$T/play.yml" -e "@$T/render-inject.json" -e ansible_become=false \
  -e "solari_stack_admin_user=$(id -un)" -e "solari_stack_artifact_root=$T/opt" \
  -e "solari_stack_work_root=$T/work" > "$T/run6.log" 2>&1 || true
[ ! -e "$T/out/pwned" ] || die "guard check was interpreted by a shell"
ok "guard check metacharacters are inert (argv, no shell)"

echo "PASS: $pass checks"
