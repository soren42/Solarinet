# DNS release tooling live cutover

This runbook is a change plan only. **Do not execute it without Jason's separate
DNS change approval.** It applies to Xenon first; each secondary is an
independent, explicitly reviewed follow-up. Steel remains out of scope until its
container and runtime adapter are verified.

## Preconditions

- Review the exact release helper, root-owned config, `xenon.json.example`, and
  generated candidate contents. Record hashes in the change record.
- Confirm a restorable backup of `/etc/bind/named.conf`, every managed include,
  zone files, current serials, ownership, modes, AppArmor policy, and service
  state. Confirm out-of-band access and a maintenance window.
- Inventory every writer and prove the legacy copy-based SoR writer is stopped.
  Keep it stopped through the cutover; the release lock cannot serialize it.
- Confirm the configured `legacyIncludes`, runtime/container path mapping,
  service account, listener, reload command, health checks, and transfer ACL
  against the host. Reject unknown or unmanaged zone ownership.
- Verify the installed helper is root-owned and immutable to the applier;
  config, release root, and current-link parent are root-owned and not
  group/world writable. Install private incoming and state directories for the
  applier account.
- Test candidate and rollback using the same BIND version and topology in an
  isolated environment, including each online secondary. Confirm secondary
  AXFR updates and served SOA serials. Have the previous include and writer
  restoration procedure ready.

## Bootstrap cutover

1. Announce the freeze and stop the old writer. Verify no writer process or
   timer remains; capture service state and current authoritative SOAs.
2. Install the reviewed helper at `/usr/local/libexec/solarinet-dns-release`
   and protected config at `/etc/solarinet/dns-release.json`. Validate hashes,
   ownership, modes, paths, and command argv arrays.
3. Run `sudo /usr/local/libexec/solarinet-dns-release --bootstrap --config
   /etc/solarinet/dns-release.json` once. The helper seeds a complete baseline,
   compares it with currently served data, journals the original parent include,
   then switches the include and verifies serving.
4. Independently query each zone's authoritative SOA and expected record on the
   primary and every configured secondary. Compare these with the recorded
   applied generation. Check BIND logs and `state.json` (`phase=applied`).
5. If bootstrap fails, do not start the new applier or old writer until the
   journal is understood. The helper restores the exact parent include on
   recoverable failure; verify the legacy serving state and serials manually.

## Enable and observe

1. Start only the DNS applier. Confirm its full startup reconcile succeeds for
   `primary` and every configured `secondary:<name>` in target status.
2. Confirm served SOAs and records independently with `dig`; an emitter
   checkpoint is not evidence of DNS freshness. Review BIND and applier logs,
   release journal, target state, and RabbitMQ queue depth.
3. Keep the old writer stopped until multiple successful reconciliations and
   event application are observed. Then close the change only after each
   secondary's generation and SOA are verified.

## Rollback

Stop the new applier, preserve logs/state, and use the helper's journaled
rollback/recovery path. Verify the restored generation is authoritative on the
primary and each secondary before reopening writes. If helper recovery fails,
keep both writers stopped and restore the backed-up parent include and files
using the approved host procedure; reload BIND and verify every zone by SOA and
expected records. Never point the legacy writer at the release tree.
