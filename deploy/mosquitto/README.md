# Mosquitto on Benzene

This documents the rootful Podman deployment used for SolariNet MQTT clients.
It provides MQTT on port `1883` and WebSockets on port `9001`, requires password
authentication, and persists broker state outside the container. Both listeners
are plaintext; keep them inside the trusted intranet and restrict access with the
host firewall.

The example configuration is secret-free. Keep the generated `passwd` file and
any client credentials outside Git. The broker container runs as the image's
`mosquitto` user (UID/GID `1883:1883`); the password file must be owned by that
identity and have mode `0700`.

The pinned image digest below is the x86_64 Mosquitto `2.1.2` image verified on
Benzene. The moving major tag `:2` is retained on the existing container; use
the digest for repeatable new installs.

## Provision

Run the commands below from this repository directory. Create the host paths
and install the sample config:

```sh
sudo install -d -o root -g root -m 0755 /opt/mosquitto/config
sudo install -d -o 1883 -g 1883 -m 0750 /opt/mosquitto/data
sudo install -o root -g root -m 0644 mosquitto.conf.example \
  /opt/mosquitto/config/mosquitto.conf
```

Create the password file interactively with the utility in the pinned image.
The password is entered at the prompt, not passed as a command argument:

```sh
sudo podman run --rm --interactive --tty \
  --volume /opt/mosquitto/config:/mosquitto/config:rw \
  --entrypoint /usr/bin/mosquitto_passwd \
  docker.io/library/eclipse-mosquitto@sha256:6dba0f1b2795ddcbe0d41bdfb8b8d56a423acca23ccde4342a4652be54639b11 \
  -c /mosquitto/config/passwd YOUR_SERVICE_USERNAME
sudo chown 1883:1883 /opt/mosquitto/config/passwd
sudo chmod 0700 /opt/mosquitto/config/passwd
```

`-c` creates or truncates the file. Use it only for first-time provisioning;
for an existing broker, use `mosquitto_passwd` without `-c` so other users are
preserved. Do not print or commit the password file.

Validate the config before starting the broker:

```sh
sudo podman run --rm --network none \
  --volume /opt/mosquitto/config:/mosquitto/config:ro \
  --entrypoint /usr/sbin/mosquitto \
  docker.io/library/eclipse-mosquitto@sha256:6dba0f1b2795ddcbe0d41bdfb8b8d56a423acca23ccde4342a4652be54639b11 \
  --test-config -c /mosquitto/config/mosquitto.conf
```

The sample sends logs to standard output for `podman logs`; persistent broker
state is stored in `/opt/mosquitto/data`. Start the broker:

```sh
sudo podman run --detach --name mosquitto --restart=unless-stopped \
  --publish 1883:1883 --publish 9001:9001 \
  --volume /opt/mosquitto/config:/mosquitto/config:ro \
  --volume /opt/mosquitto/data:/mosquitto/data \
  docker.io/library/eclipse-mosquitto@sha256:6dba0f1b2795ddcbe0d41bdfb8b8d56a423acca23ccde4342a4652be54639b11
```

The rootful `podman-restart.service` must be enabled so containers with a
restart policy start after host boot. The published ports bind all host
interfaces by default; firewall policy must limit them to approved clients.

## Verify

```sh
sudo systemctl is-active podman-restart.service
sudo podman inspect mosquitto \
  --format 'status={{.State.Status}} restarts={{.RestartCount}}'
sudo podman exec mosquitto /usr/sbin/mosquitto --test-config \
  -c /mosquitto/config/mosquitto.conf
sudo podman logs --tail 40 mosquitto
sudo ss -ltn '( sport = :1883 or sport = :9001 )'
```

Expected state: container is running, restart count is zero, the config test
passes, logs end with `mosquitto version ... running`, and both listener ports
are present. A broker startup check does not verify application credentials;
confirm that separately with a configured MQTT client. The Authbroker service
is not currently deployed on Xenon.

## Diagnose a config-open restart loop

Inspect the broker log and exact bind mounts without printing the password file:

```sh
sudo podman logs --tail 80 mosquitto
sudo podman inspect mosquitto --format '{{json .Mounts}}'
sudo stat -c '%n mode=%a owner=%u:%g' \
  /opt/mosquitto/config /opt/mosquitto/config/mosquitto.conf \
  /opt/mosquitto/config/passwd
```

Check that the host config and password file exist, the config mount is
read-only, and the password file is mode `0700` and owned by `1883:1883`. Test
the config with a fresh isolated container before recreating the service.

When recreating a failed container, preserve its data directory and log volume.
Keep the failed container stopped under a diagnostic name until the replacement
passes verification. A failed container is not a rollback target unless it was
known-good before the incident. Never delete `/opt/mosquitto/data` as a routine
repair step.
