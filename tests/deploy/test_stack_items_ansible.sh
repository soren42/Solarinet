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
#      is executed as argv (shell metacharacters are inert);
#   7. privilege boundary: become is SIMULATED by a test-only become plugin
#      (solari_fakebecome) that tags each command with the user it would have
#      become, plus an `id` shim that reports uid 0 under a "root" tag. Builds
#      record `id -u` + that tag per phase: compile AND install (custom,
#      make, cmake — all DESTDIR-staged) must be the admin user / non-zero
#      uid; no build command runs as root, only the data-only stage_install.py
#      copy does. Forcing ansible_become_user=root (which beats the task
#      keyword) must make compile refuse; a cmake install outside its prefix
#      and a repo field carrying a newline are refused;
#   8. stage_install: an install that ignores DESTDIR (stages nothing) is
#      refused with the "model it as a script" message, and a symlink already
#      in the destination (prefix/lib -> elsewhere) is never followed.
#
# Not exercised here (need root / network): real sudo, pkg and repo steps.
# The cmake legs skip when cmake is absent.
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

mkdir -p "$T/src/hello-1.0" "$T/src/makething-1.0" "$T/src/cmakething-1.0" "$T/roles/smoke-role/tasks" \
  "$T/prefix" "$T/out" "$T/gitsrc" "$T/fakebin" "$T/become_plugins"
HAVE_CMAKE=0; command -v cmake >/dev/null 2>&1 && HAVE_CMAKE=1

# Simulated become: tag every become'd command with the user it would become.
cat > "$T/become_plugins/solari_fakebecome.py" <<'PY'
# Test-only: runs as the invoking user, exporting the target user it WOULD
# have become, so the smoke test can see which phase asked for which user.
import shlex
from ansible.plugins.become import BecomeBase

DOCUMENTATION = """
name: solari_fakebecome
short_description: SolariNet test-only simulated become
description: Runs the command as the invoking user with SOLARI_FAKE_BECOME_USER set.
author: SolariNet tests
options:
  become_user:
    description: User the command would have become.
    default: root
    vars:
      - name: ansible_become_user
    keyword:
      - name: become_user
"""


class BecomeModule(BecomeBase):
    name = "solari_fakebecome"

    def build_become_command(self, cmd, shell):
        super().build_become_command(cmd, shell)
        if not cmd:
            return cmd
        user = shlex.quote(self.get_option("become_user") or "root")
        return "env SOLARI_FAKE_BECOME_USER=%s %s" % (user, self._build_success_command(cmd, shell))
PY
REAL_ID="$(command -v id)"
cat > "$T/fakebin/id" <<EOF
#!/bin/sh
# Under a simulated root become, report uid 0 so "refuse as root" is testable.
if [ "\${SOLARI_FAKE_BECOME_USER:-}" = root ] && [ "\$*" = "-u" ]; then echo 0; exit 0; fi
exec "$REAL_ID" "\$@"
EOF
chmod 0755 "$T/fakebin/id"
# Records "<uid> <simulated become user>" for one phase into the given file.
cat > "$T/proof.sh" <<'EOF'
#!/bin/sh
printf '%s %s\n' "$(id -u)" "${SOLARI_FAKE_BECOME_USER:-none}" > "$1"
EOF
chmod 0755 "$T/proof.sh"

# make: `all` compiles and `install` stages (DESTDIR), both unprivileged.
cat > "$T/src/makething-1.0/Makefile" <<EOF
all:
	$T/proof.sh compile.proof
install:
	mkdir -p \$(DESTDIR)\$(PREFIX)/proof/make
	cp compile.proof \$(DESTDIR)\$(PREFIX)/proof/make/compile
	$T/proof.sh \$(DESTDIR)\$(PREFIX)/proof/make/install
EOF
tar -C "$T/src" -czf "$T/makething-1.0.tar.gz" makething-1.0
# cmake: compile + `cmake --install` (DESTDIR stage) unprivileged, root copies.
cat > "$T/src/cmakething-1.0/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.10)
project(cmakething NONE)
add_custom_target(proof ALL COMMAND $T/proof.sh \${CMAKE_BINARY_DIR}/compile.proof VERBATIM)
install(FILES \${CMAKE_BINARY_DIR}/compile.proof DESTINATION proof/cmake RENAME compile)
install(CODE "execute_process(COMMAND $T/proof.sh \\\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/proof/cmake/stage)")
EOF
tar -C "$T/src" -czf "$T/cmakething-1.0.tar.gz" cmakething-1.0
MAKE_SHA="$(sha256sum "$T/makething-1.0.tar.gz" | cut -d' ' -f1)"
CMAKE_SHA="$(sha256sum "$T/cmakething-1.0.tar.gz" | cut -d' ' -f1)"
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

T="$T" TAR_SHA="$TAR_SHA" ART_SHA="$ART_SHA" GIT_PIN="$GIT_PIN" MAKE_SHA="$MAKE_SHA" CMAKE_SHA="$CMAKE_SHA" \
  HAVE_CMAKE="$HAVE_CMAKE" python3 - > "$T/defs.json" <<'PY'
import json, os
t = os.environ["T"]
same = {"suse": "=", "debian": "=", "fedora": "="}
cmake = [{"logical_name": "cmakething", "item_type": "build", "names": same, "version_pin": "1.0",
          "spec": {"source": {"tarball": f"file://{t}/cmakething-1.0.tar.gz", "sha256": os.environ["CMAKE_SHA"]},
                   "system": "cmake", "prefix": f"{t}/prefix", "creates": f"{t}/prefix/proof/cmake/stage"}}
         ] if os.environ["HAVE_CMAKE"] == "1" else []
software = [
    {"logical_name": "smoke role", "item_type": "role", "names": same, "ansible_role": "smoke-role"},
    {"logical_name": "marker script", "item_type": "script", "names": same, "version_pin": "2.1",
     "spec": {"body": f'echo "2.1" > {t}/out/version.txt\necho "$GREETING" > {t}/out/greeting.txt',
              "env": {"GREETING": "hi from env"}, "run_as": "admin",
              "creates": f"{t}/out/greeting.txt", "check": ["cat", f"{t}/out/version.txt"],
              "version_check": r"^([0-9.]+)$"}},
    {"logical_name": "hello", "item_type": "build", "names": same, "version_pin": "1.0",
     "spec": {"source": {"tarball": f"file://{t}/hello-1.0.tar.gz", "sha256": os.environ["TAR_SHA"]},
              "system": "custom", "prefix": f"{t}/prefix",
              "build": f"{t}/proof.sh compile.proof",
              "install": 'D="$DESTDIR$PREFIX" && mkdir -p "$D/bin" "$D/proof/custom" && cp hello.sh "$D/bin/hello"'
                         ' && chmod 0755 "$D/bin/hello" && cp compile.proof "$D/proof/custom/compile"'
                         f' && {t}/proof.sh "$D/proof/custom/install"',
              "creates": f"{t}/prefix/bin/hello"}},
    {"logical_name": "gitthing", "item_type": "build", "names": same, "version_pin": os.environ["GIT_PIN"],
     "spec": {"source": {"git": f"file://{t}/gitsrc", "tag": "display-only"}, "system": "custom",
              "prefix": f"{t}/prefix", "build": "cp marker marker.built",
              "install": 'mkdir -p "$DESTDIR$PREFIX" && cp marker.built "$DESTDIR$PREFIX/git-marker"',
              "creates": f"{t}/prefix/git-marker", "check": ["cat", f"{t}/prefix/git-marker"]}},
    {"logical_name": "makething", "item_type": "build", "names": same, "version_pin": "1.0",
     "spec": {"source": {"tarball": f"file://{t}/makething-1.0.tar.gz", "sha256": os.environ["MAKE_SHA"]},
              "system": "make", "prefix": f"{t}/prefix", "creates": f"{t}/prefix/proof/make/install"}},
    *cmake,
    {"logical_name": "tool", "item_type": "artifact", "names": same, "version_pin": "3.0",
     "artifacts": {"any": {"url": f"file://{t}/tool.bin", "sha256": os.environ["ART_SHA"]}}},
]
print(json.dumps({"software": software, "stacks": [
    {"slug": "smoke", "members": [{"software": e["logical_name"]} for e in software]}]}))
PY
python3 "$ROOT/deploy/fleet/stackgen.py" --from-json "$T/defs.json" --family "$FAMILY" --arch "$ARCH" \
  --stack smoke > "$T/render.json"
python3 - "$T/render.json" <<'PY' || die "render step order"
import json, sys
r = json.load(open(sys.argv[1]))
nb = len(r["solari_stack_builds"])
assert nb in (3, 4), nb
assert [s["type"] for s in r["solari_stack_steps"]] == ["artifact"] + ["build"] * nb + ["script", "role"], r["solari_stack_steps"]
assert all("commands" not in b for b in r["solari_stack_builds"])
PY
ok "render: artifact → builds (custom tarball, custom git, make[, cmake]) → script → role"

cat > "$T/play.yml" <<EOF
---
- hosts: localhost
  connection: local
  gather_facts: true
  roles:
    - role: stack-items
EOF
ADMIN="$(id -un)"; MYUID="$(id -u)"
export PATH="$T/fakebin:$PATH" ANSIBLE_BECOME_PLUGINS="$T/become_plugins"
# ansible_become_user is deliberately NOT set: each task falls back to its own
# become_user keyword, else root. The admin user here IS the connection user,
# and Ansible elides a same-user become unless told not to.
BECOME=(-e ansible_become=true -e ansible_become_method=solari_fakebecome)
export ANSIBLE_BECOME_ALLOW_SAME_USER=1
play() {  # play <render> <log> [extra ansible-playbook args...]
  local render="$1" log="$2"; shift 2
  ANSIBLE_ROLES_PATH="$ANS/roles:$T/roles" ANSIBLE_STDOUT_CALLBACK=default ANSIBLE_NOCOLOR=1 \
    ANSIBLE_LOCALHOST_WARNING=0 ANSIBLE_INVENTORY_UNPARSED_WARNING=0 \
    ansible-playbook -i localhost, "$T/play.yml" -e "@$render" "${BECOME[@]}" \
      -e "solari_stack_admin_user=$ADMIN" -e "solari_stack_work_root=$T/work" \
      -e "solari_stack_artifact_root=$T/opt" "$@" > "$log" 2>&1
}
run_play() {
  play "$T/render.json" "$T/run$1.log" || { cat "$T/run$1.log" >&2; die "stack-items run $1"; }
}

# ---- 3. first run: effects --------------------------------------------------
run_play 1
[ "$(cat "$T/out/greeting.txt")" = "hi from env" ] || die "script env/body"
[ "$("$T/prefix/bin/hello")" = "hello" ] || die "custom build install"
cmp -s "$T/tool.bin" "$T/opt/tool/3.0/tool.bin" || die "artifact download"
[ -f "$T/out/role.txt" ] || die "role step"
[ "$(cat "$T/prefix/git-marker")" = "pinned" ] || die "git build did not use the pinned commit"
ok "first run applied artifact, builds (tarball + git at pinned commit, not branch tip), script (env), role"

# Privilege boundary, per phase: "<uid> <simulated become user>".
want() {  # want <proof file> <expected user> <label>
  local got; got="$(cat "$1" 2>/dev/null)" || die "$3: no proof file $1"
  if [ "$2" = root ]; then
    [ "$got" = "0 root" ] || die "$3 should run as root, got '$got'"
  else
    [ "$MYUID" != 0 ] && [ "$got" = "$MYUID $2" ] || die "$3 should run as uid $MYUID/$2 (non-root), got '$got'"
  fi
}
want "$T/prefix/proof/custom/compile" "$ADMIN" "custom build"
want "$T/prefix/proof/custom/install" "$ADMIN" "custom install"
want "$T/prefix/proof/make/compile" "$ADMIN" "make compile"
want "$T/prefix/proof/make/install" "$ADMIN" "make install"
ok "custom + make: compile AND install ran as $ADMIN (uid $MYUID), never as root"
[ -f "$T/work/build/makething/1.0/_stage$T/prefix/proof/make/install" ] \
  && [ -f "$T/work/build/hello/1.0/_stage$T/prefix/bin/hello" ] \
  || die "make/custom install was not staged via DESTDIR"
[ "$(stat -c %a "$T/prefix/bin/hello")" = 755 ] && [ "$(stat -c %a "$T/prefix/proof/make/compile")" = 644 ] \
  || die "installed modes not normalised to 0755/0644"
ok "make + custom installs staged via DESTDIR; root step copied them with normalised modes"
if [ "$HAVE_CMAKE" = 1 ]; then
  want "$T/prefix/proof/cmake/compile" "$ADMIN" "cmake compile"
  want "$T/prefix/proof/cmake/stage" "$ADMIN" "cmake --install (DESTDIR stage)"
  [ -f "$T/work/build/cmakething/1.0/_stage$T/prefix/proof/cmake/compile" ] \
    || die "cmake install was not staged via DESTDIR"
  ok "cmake: compile + DESTDIR stage ran as $ADMIN, root copied the stage into the prefix"
else
  echo "skip - cmake not installed: cmake staged-install leg"
fi
[ "$(stat -c %U "$T/work/build/hello/1.0")" = "$ADMIN" ] || die "build dir not owned by the admin user"
ok "build dir owned by $ADMIN"

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
if play "$T/render.json" "$T/run4.log"; then
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
if play "$T/render-escape.json" "$T/run5.log" -e "solari_stack_artifact_root=$T/opt/art"; then
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
play "$T/render-inject.json" "$T/run6.log" || true
[ ! -e "$T/out/pwned" ] || die "guard check was interpreted by a shell"
ok "guard check metacharacters are inert (argv, no shell)"

# ---- 9. privilege boundary: forced root become makes compile refuse ----------
python3 - "$T/render.json" "$T/render-root.json" "$T" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); t = sys.argv[3]
b = next(b for b in r["solari_stack_builds"] if b["name"] == "hello")
b.update(prefix=f"{t}/prefix-root", creates=f"{t}/prefix-root/bin/hello", version="1.0-root")
r["solari_stack_builds"] = [b]; r["solari_stack_steps"] = [{"type": "build", "index": 0}]
json.dump(r, open(sys.argv[2], "w"))
PY
# A connection var beats the task's become_user keyword: compile must fail closed.
if play "$T/render-root.json" "$T/run7.log" -e ansible_become_user=root; then
  die "compile ran under a forced root become"
fi
grep -q 'refusing to compile' "$T/run7.log" || { cat "$T/run7.log" >&2; die "forced-root run failed for another reason"; }
[ ! -e "$T/prefix-root" ] || die "install ran after a refused compile"
ok "compile refuses uid 0 even when inventory forces become_user=root"

# ---- 10. staged install outside the prefix is refused --------------------------
if [ "$HAVE_CMAKE" = 1 ]; then
  mkdir -p "$T/src/escape-1.0"
  cat > "$T/src/escape-1.0/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.10)
project(escape NONE)
install(FILES CMakeLists.txt DESTINATION share)
install(FILES CMakeLists.txt DESTINATION $T/outside-prefix)
EOF
  tar -C "$T/src" -czf "$T/escape-1.0.tar.gz" escape-1.0
  python3 - "$T/render.json" "$T/render-outside.json" "$T" "$(sha256sum "$T/escape-1.0.tar.gz" | cut -d' ' -f1)" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); t = sys.argv[3]
b = dict(next(b for b in r["solari_stack_builds"] if b["system"] == "cmake"))
b.update(name="escape", id="smoke--escape", prefix=f"{t}/prefix-esc", creates=f"{t}/prefix-esc/share/CMakeLists.txt",
         source={"git": None, "commit": None, "tag": None, "tarball": f"file://{t}/escape-1.0.tar.gz", "sha256": sys.argv[4]})
r["solari_stack_builds"] = [b]; r["solari_stack_steps"] = [{"type": "build", "index": 0}]
json.dump(r, open(sys.argv[2], "w"))
PY
  if play "$T/render-outside.json" "$T/run8.log"; then die "install outside the prefix was accepted"; fi
  grep -q 'staged files outside its prefix' "$T/run8.log" \
    || { cat "$T/run8.log" >&2; die "outside-prefix install failed for another reason"; }
  [ ! -e "$T/outside-prefix" ] && [ ! -e "$T/prefix-esc" ] || die "something was installed despite the refusal"
  ok "cmake install staged outside its prefix is refused, nothing installed"
else
  echo "skip - cmake not installed: outside-prefix leg"
fi

# ---- 11. repo fields cannot inject config lines (role-side check) -------------
for field in url suite; do
  if [ "$field" = suite ] && [ "$FAMILY" != debian ]; then continue; fi
  python3 - "$T/render.json" "$T/render-repo-$field.json" "$field" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); f = sys.argv[3]
repo = {"name": "evil", "id": "smoke--evil", "stack": "smoke", "version": None,
        "url": "https://repo.invalid/deb", "key_url": "https://repo.invalid/key.asc",
        "fingerprint": "A" * 40, "suite": "stable", "components": ["main"]}
repo[f] += "\ndeb [trusted=yes] http://evil.invalid/ x main"
r["solari_stack_repos"] = [repo]; r["solari_stack_steps"] = [{"type": "repo", "index": 0}]
json.dump(r, open(sys.argv[2], "w"))
PY
  if play "$T/render-repo-$field.json" "$T/run-repo-$field.log"; then die "repo $field with a newline was accepted"; fi
  grep -q 'could inject config lines' "$T/run-repo-$field.log" \
    || { cat "$T/run-repo-$field.log" >&2; die "repo $field refused for another reason"; }
  [ ! -e "$T/work/keys/smoke--evil.asc" ] || die "repo key fetched before field validation"
  ok "repo $field carrying a newline refused before anything is fetched"
done

# ---- 12. install that ignores DESTDIR stages nothing: refused ------------------
nodest_build() {  # nodest_build <out render> <name> <prefix> <install cmd>
  python3 - "$T/render.json" "$1" "$2" "$3" "$4" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); name, prefix, inst = sys.argv[3:6]
b = dict(next(b for b in r["solari_stack_builds"] if b["name"] == "hello"))
b.update(name=name, id=f"smoke--{name}", prefix=prefix, creates=f"{prefix}/bin/x", install=inst)
r["solari_stack_builds"] = [b]; r["solari_stack_steps"] = [{"type": "build", "index": 0}]
json.dump(r, open(sys.argv[2], "w"))
PY
}
# (Here the admin user can write the prefix; on a real target it cannot, and
# the root step refuses either way because nothing was staged.)
nodest_build "$T/render-nodest.json" nodest "$T/prefix-nodest" 'mkdir -p "$PREFIX/bin" && echo x > "$PREFIX/bin/x"'
if play "$T/render-nodest.json" "$T/run9.log"; then die "install ignoring DESTDIR was accepted"; fi
grep -q 'did not honour DESTDIR' "$T/run9.log" && grep -q 'modelled as a `script` item' "$T/run9.log" \
  || { cat "$T/run9.log" >&2; die "DESTDIR-ignoring install failed for another reason"; }
ok "install that ignores DESTDIR is refused (root-time install logic must be a script item)"

# ---- 13. a destination symlink is never followed by the root copy -------------
mkdir -p "$T/prefix-sym" "$T/victim"
ln -s "$T/victim" "$T/prefix-sym/bin"
nodest_build "$T/render-sym.json" symdest "$T/prefix-sym" 'mkdir -p "$DESTDIR$PREFIX/bin" && echo pwned > "$DESTDIR$PREFIX/bin/x"'
if play "$T/render-sym.json" "$T/run10.log"; then die "install through a destination symlink was accepted"; fi
grep -q 'refusing to install through destination symlink' "$T/run10.log" \
  || { cat "$T/run10.log" >&2; die "destination-symlink install failed for another reason"; }
[ -z "$(ls -A "$T/victim")" ] || die "root copy followed prefix/bin -> $T/victim"
ok "root stage copy refuses a destination symlink (prefix/bin -> elsewhere), nothing written through it"

echo "PASS: $pass checks"
