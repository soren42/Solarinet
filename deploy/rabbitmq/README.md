# Benzene RabbitMQ

The source definition for the Benzene broker is a rootful Podman Quadlet. It
pins the observed RabbitMQ 3.13.7 management image by registry digest, uses the
existing `rabbitmq-data` volume, keeps the existing host ports, and lets
systemd restart the container after a failed health check. The management and
AMQP ports retain their current all-interface IPv4 bindings.

Checked on Benzene on 2026-10-22: Podman 5.8.3 and systemd 260 support Quadlet;
`podman-restart.service` is enabled. The live broker reports RabbitMQ 3.13.7
and the same image digest used by CI, but its container references the floating
`rabbitmq:3-management` tag and has no checked-in unit. The candidate files do
not change the live broker. Independent review and the staged rehearsal below
are required before cutover.

## Credentials

The actual environment file lives at `/etc/solarinet/rabbitmq.env` on Benzene.
It contains the existing bootstrap username, password, and vhost; it is not
stored in this repository. `.gitignore` excludes the runtime file. Keep it
owned by root with mode `0600` and its parent directory root-owned with mode
`0700`.

To copy only the three bootstrap variables from the current container into the
root-only file without printing their values:

```sh
sudo install -d -o root -g root -m 0700 /etc/solarinet
sudo sh -c 'umask 077; podman inspect --format "{{range .Config.Env}}{{println .}}{{end}}" rabbitmq | grep -E "^RABBITMQ_DEFAULT_(USER|PASS|VHOST)=" > /etc/solarinet/rabbitmq.env'
sudo chmod 0600 /etc/solarinet/rabbitmq.env
sudo cut -d= -f1 /etc/solarinet/rabbitmq.env
```

The last command prints variable names only. Confirm that it prints exactly
`RABBITMQ_DEFAULT_PASS`, `RABBITMQ_DEFAULT_USER`, and
`RABBITMQ_DEFAULT_VHOST` before installing the Quadlet. For a new, empty volume,
create this file from `rabbitmq.env.example` and replace the placeholder with a
new secret. Never use the example file as the runtime environment file.

## Staged cutover

1. Save the active container image reference, container ID, volume name, ports,
   restart policy, and broker health state. Confirm the candidate image digest
   is available locally and matches the digest in `solarinet-rabbitmq.container`.
2. Create a root-only stopped-volume backup before cutover. Stop RabbitMQ,
   archive the volume reported by `podman volume inspect rabbitmq-data`, then
   start the existing container and verify queue consumers recover.
3. Install `solarinet-rabbitmq.container` as
   `/etc/containers/systemd/solarinet-rabbitmq.container`, create and validate
   the credential file above, then run `sudo systemctl daemon-reload`. Confirm
   `solarinet-rabbitmq.service` is generated before stopping the live broker.
4. Disable automatic restart of the old container, stop it, and rename it to a
   dated rollback name. For example:

   ```sh
   sudo podman update --restart-policy=no rabbitmq
   sudo podman stop rabbitmq
   sudo podman rename rabbitmq rabbitmq-rollback-20261022
   sudo systemctl enable --now solarinet-rabbitmq.service
   ```

   Start `solarinet-rabbitmq.service`; the Quadlet creates the replacement as
   `rabbitmq` on the same volume and ports. Do not start it while the old
   container still owns the `rabbitmq` name: Quadlet uses Podman's `--replace`
   option.
5. Verify the unit and container are active, the server reports 3.13.7, local
   alarms are clear, all four `solari` queues have consumers, ready and
   unacknowledged counts settle to their expected values, and the Xenon
   emit/apply/reconcile and notification consumers reconnect. Exercise one
   disposable publish/consume round trip.
6. Keep the stopped rollback container and the volume backup until the agreed
   observation window passes. Then remove only the stale container; retain the
   backup under the existing backup policy.

To roll back, stop and disable `solarinet-rabbitmq.service`, remove the
replacement container without deleting `rabbitmq-data`, rename the dated
container back to `rabbitmq`, restore its `unless-stopped` restart policy, and
start it:

```sh
sudo systemctl disable --now solarinet-rabbitmq.service
sudo podman rm -f rabbitmq
sudo podman rename rabbitmq-rollback-20261022 rabbitmq
sudo podman update --restart-policy=unless-stopped rabbitmq
sudo podman start rabbitmq
```

Confirm queues and consumers recover before resuming publishers.
Restoring the volume backup is reserved for evidence of on-disk corruption; the
candidate uses the same RabbitMQ version and volume format as the live broker.

The CI upgrade rehearsal remains separate: it tests 3.13.7 → 4.2.9 → 4.3.7 →
4.4.0 and rollback from a pre-4.4 snapshot on disposable volumes. The Quadlet
candidate pins 3.13.7 for the live service and does not enable unattended image
updates.
