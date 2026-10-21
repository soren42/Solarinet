# Opie service deployment

Opie runs under the `solari-opie` system account. Claude receives one structured
`mcp__opie__probe` tool. Bash and generic file tools are explicitly denied. The
MCP server invokes the separate probe executable through argv, and the probe
remains an exact host allowlist of read-only checks.

## Runtime requirements

- Install the service code and virtual environment under `/opt/solarinet/opie`.
- Build the venv from `deploy/opie/requirements.txt`; it includes the MCP
  server SDK alongside Opie's database and notification clients.
- Install `opied.service` as `/etc/systemd/system/opied.service` and the
  sysusers file as `/usr/lib/sysusers.d/solari-opie.conf`.
- Install `/etc/solarinet/opie.conf` readable by `solari-opie` (mode `0640`,
  root:solari-opie). Create the dedicated database identity through the
  protected credential workflow, then apply `grants.sql`. It grants column-level
  reads for alert, node, rule, and maintenance fields plus only the required
  `opieReport` select/insert/update columns. A dedicated RabbitMQ identity
  limited to the notification exchange is also required. The grant script
  first revokes existing direct grants; confirm `SHOW GRANTS` afterward and
  ensure the account has no inherited roles. Do not use the broad `solari`
  database account.
- Create `/etc/solarinet/opie.env` root-owned and readable only by root (mode
  `0600`). It is loaded by systemd and must contain only `SOLARI_DB_PASS` for
  that restricted database identity. Do not copy `run/db.env`.
- Opie strips `SOLARI_DB_PASS` and Anthropic API/token variables from the Claude
  child environment. Configure Claude authentication in the private service
  account instead of putting credentials in environment variables.
- Install `opie-mcp.example.json` as root-owned `/etc/solarinet/opie-mcp.json`
  (mode `0644`). It configures exactly one stdio MCP server named `opie`; the
  CLI invocation allows only `mcp__opie__probe` and explicitly denies Bash,
  generic file tools, and web/agent tools. `--tools ""` disables all built-in
  Claude tools; `--strict-mcp-config` limits MCP to the single configured Opie
  server. Deploy only a Claude CLI version that supports these options.
- Keep the `[state] file` in `opie.conf` under `/var/lib/solari-opie`, the only
  writable service directory.
- Install the Claude CLI and `opie-probe` at system paths visible to the service.
  Provision Claude authentication separately into `/var/lib/solari-opie` while
  acting as that account. Never copy Jason's Claude credentials. Keep its
  credentials private to that account.
- Create root-owned `/etc/solarinet/opie-hosts.allow` (mode `0644`) from the example with one exact,
  confirmed SSH target per line. The probe rejects unlisted hosts and network
  ranges. Keep the list limited to managed Linux systems with the required
  read-only checks.
- Provision `/var/lib/solari-opie/.ssh/id_ed25519` owned by `solari-opie` with
  private key mode `0600`. Keep the `.ssh` directory and pinned
  `known_hosts` root-owned and readable by `solari-opie` (directory `0750`, file
  `0640`) so the service cannot expand its own trust set. The probe uses strict
  host key checking and remote account `solari-probe`; it does not use
  `jason@host` or accept new host keys automatically.
- On each target, force the key through a root-owned `authorized_keys` entry
  such as `restrict,command="/usr/local/libexec/solari-opie-remote" <public-key>`.
  Install `opie-probe-remote.sh` at that path. The forced-command wrapper parses
  a fixed operation and one validated argument; it never evaluates the
  requested command as shell code. Disable password and keyboard-interactive
  authentication for that account. Configure noninteractive sudo only for
  `smartctl` if that check is required.
  The `service` check returns selected unit state fields rather than full unit
  contents, and `proc` omits process arguments to avoid disclosing embedded
  credentials.

Run `bash deploy/opie/test_opie_probe.sh` and
`bash deploy/opie/test_opie_probe_remote.sh` before packaging. They verify the
exact host allowlist, pinned SSH options, forced-command protocol, dedicated
remote user, and rejection of unlisted hosts/commands without a network
connection.

The service intentionally has no fallback to Jason's home directory or
credentials. It will fail to start if its configuration, credentials, runtime,
or executable paths have not been provisioned. The live Xenon unit has not been
migrated by this candidate change.
