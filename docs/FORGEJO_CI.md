# Forgejo CI

**Updated:** 2026-10-22  
**Primary repository:** Forgejo (`jason/Solarinet`)  
**Candidate branch:** `integrate/stability-20261020`

## Decision

Forgejo is the authoritative remote for this project. GitHub is a mirror and can lag or
be unavailable during service incidents. The Forgejo workflow is maintained as a generated
adaptation of `.github/workflows/ci.yml`; the renderer and parity check are
`tools/ci/render_forgejo_workflow.py`. The GitHub workflow runs that check so changes to the
source CI cannot silently leave Forgejo CI stale.

## Execution contract

The workflow is in `.forgejo/workflows/ci.yml`. It uses a dedicated runner label,
`solarinet-ci-host`, and runs disposable MariaDB/RabbitMQ fixtures as Docker containers
published only on `127.0.0.1`. This preserves the existing integration tests' loopback
requirements and the RabbitMQ upgrade test's Docker access and dynamically published ports.
Tests and their live-SoR guards remain unchanged.

This contract requires a **dedicated, disposable Linux VM** with Docker, `sudo`, and the
Forgejo runner installed in host mode using label `solarinet-ci-host:host`. Configure runner
capacity to `1`; jobs share fixed loopback ports. Host mode does not isolate job code. Never
register a shared application, database, DNS, workstation, or production host. The VM should
hold no production credentials or writable production mounts and should be reset/recreated
when its trust state is uncertain. The workflow cleans up its named service containers with
`if: always()` steps.

Native macOS coverage remains GitHub-only. The Linux Forgejo workflow does not claim that
gate has run.

## Verification and rollout state

The workflow renderer's `--check` verifies generated-file parity. Local validation also
parses the YAML and runs `git diff --check`. Those checks validate source shape only; they do
not prove that a Forgejo runner is registered, that host mode executes successfully, or that
the full CI matrix passes there.

No dedicated disposable runner VM has been identified or provisioned. Therefore Forgejo CI
execution is **UNVERIFIED**. Do not describe this workflow as a green hosted gate until a
dedicated runner has run the complete matrix. The existing GitHub workflow remains available
as a mirror validation path when GitHub Actions is operating normally.

## Forgejo references

- [Actions administrator guide](https://forgejo.org/docs/v15.0/admin/actions/) — runner
  installation, registration, and security model.
- [Runner configuration and labels](https://forgejo.org/docs/v15.0/admin/actions/configuration/)
  — host execution labels and runner capacity.
- [Docker access from Actions](https://forgejo.org/docs/v15.0/admin/actions/docker-access/)
  — Docker socket access grants substantial authority over the runner host.
- [Actions security guidance](https://forgejo.org/docs/v15.0/admin/actions/security/)
  — host execution is not job isolation.
- [Actions service reference](https://forgejo.org/docs/v15.0/user/actions/reference/)
  — service-container networking behavior.
- [Artifact action guidance](https://forgejo.org/docs/v15.0/user/actions/advanced-features/)
  — compatible artifact action versions.
