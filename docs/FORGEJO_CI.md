# Forgejo CI readiness

**Updated:** 2026-10-22  
**Primary repository:** Forgejo (`jason/Solarinet`)  
**Current integration ref:** `integrate/stability-20261020` at `c867bcb`

## Decision

Forgejo remains the primary remote. Do not enable a Forgejo workflow until a runner design can execute the existing integration checks without weakening their scratch-database protections.

A first workflow prototype was reviewed locally and removed before publication. It used Forgejo service DNS names, but existing tests intentionally reject non-loopback DB and broker endpoints. It also accidentally changed the local Mosquitto test's host and did not provide job-loopback access to dynamically published ports used by the RabbitMQ upgrade test. A local workflow parity check passed syntactically but did not detect these runtime-contract failures; that check was removed with the prototype.

## Verified repository constraints

The current GitHub workflow is in `.github/workflows/ci.yml`. Its jobs use service containers for MariaDB and RabbitMQ, expect those services at `127.0.0.1`, and use Docker directly in the RabbitMQ upgrade path. The test helpers enforce loopback and scratch-schema constraints, including:

- `deploy/sorsync/test_broker_recovery.py`
- `deploy/sorsync/test_dns_chain_integration.py`
- `tests/integration/test_decommission_queue_http.py`
- `tests/browser/test_scan_worker_browser_e2e.cjs`
- `tests/browser/test_scanner_import_e2e.cjs`
- `tests/dashboard/test_scanner_import.php`

These guards are intentional protections against writing to the live SoR. Keep them in place unless a replacement isolation contract is independently designed and tested.

Forgejo Actions requires a separate runner. Its Docker backend supports service containers addressed by service hostname; that topology does not meet this repository's existing loopback-only test contract without a verified loopback forwarding design. A Forgejo `host` runner executes job steps on the runner host without container isolation, and project tests invoke Docker builds and dynamically published ports. A shared production or workstation host is not a safe runner target.

## Recommended next implementation stage

Provision a dedicated disposable Linux VM for Forgejo CI, with no credentials or writable mounts for Forgejo, SoR, DNS, or production services. Validate whether a single-capacity host runner can provide the existing jobs' loopback service ports while starting and cleaning up disposable Docker service containers. If that execution model is verified, add a Forgejo-specific workflow that:

1. Uses only that runner's dedicated label and executes one job at a time to avoid fixed-port collisions.
2. Starts pinned disposable RabbitMQ/MariaDB containers bound to loopback, waits for health, and removes them even on failure.
3. Preserves the existing loopback DB/broker environment and safety checks unchanged.
4. Preserves the Docker upgrade test's dynamic-port behavior.
5. Runs the full Linux job matrix and separately records native macOS as a GitHub-only gate unless a Forgejo macOS runner is added.

If the host-runner model cannot be made isolated and repeatable, implement a loopback proxy/forwarding contract and test it end to end before changing any test guard. Do not publish a workflow based only on YAML parsing or source parity.

## Current evidence

- Forgejo is the primary Git remote; Forgejo and GitHub integration refs were synchronized at docs tip `05aa2be` when checked. This readiness documentation was published Forgejo-first.
- Forgejo instance version 15.0.3 was observed earlier; Actions are enabled by default unless disabled in configuration.
- No runner process or unit was found on Cesium, Benzene, Xenon, Chlorine, or the current workspace. Steel was unreachable; other possible hosts were not checked.
- The current GitHub workflow remains the only hosted validation path. Run `39798443486` on code head `c867bcb` completed with all 15 jobs successful. Docs-tip run `39800482855` on `05aa2be` is pending. GitHub's status API still lists an active Actions result-delay incident.

## UNVERIFIED

No Forgejo workflow has been executed. Runner inventory outside the checked hosts, service-container behavior under the available Forgejo runner, Docker isolation, dynamic port access, and actual Forgejo validation results remain unverified.

## Forgejo references

- [Actions administrator guide](https://forgejo.org/docs/v15.0/admin/actions/) — runner installation, registration, and security model.
- [Runner configuration and labels](https://forgejo.org/docs/v15.0/admin/actions/configuration/) — Docker, LXC, and host execution labels.
- [Actions service reference](https://forgejo.org/docs/v15.0/user/actions/reference/) — service containers are addressed by their service hostname.
- [Docker access from Actions](https://forgejo.org/docs/v15.0/admin/actions/docker-access/) — Docker socket access and the resulting host authority.
- [Actions security guidance](https://forgejo.org/docs/v15.0/admin/actions/security/) — risks of host execution and untrusted workflow code.
