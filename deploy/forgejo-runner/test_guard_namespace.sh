#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
  echo 'run as root; this test creates temporary network namespaces' >&2
  exit 2
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
suffix="$$"
router="ci-fw-router-$suffix"
guest="ci-fw-guest-$suffix"
remote="ci-fw-remote-$suffix"
test_tap="ct${suffix: -10}"
guest_tap="cg${suffix: -10}"
router_uplink="ru${suffix: -10}"
remote_if="rm${suffix: -10}"
temporary="$(mktemp -d /run/solarinet-ci-fw-test.XXXXXX)"
server_pid=''
ssh_server_pid=''
listener_pid=''

cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" >/dev/null 2>&1 || true
    wait "$server_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$ssh_server_pid" ]]; then
    kill "$ssh_server_pid" >/dev/null 2>&1 || true
    wait "$ssh_server_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$listener_pid" ]]; then
    kill "$listener_pid" >/dev/null 2>&1 || true
    wait "$listener_pid" >/dev/null 2>&1 || true
  fi
  ip netns del "$guest" >/dev/null 2>&1 || true
  ip netns del "$remote" >/dev/null 2>&1 || true
  ip netns del "$router" >/dev/null 2>&1 || true
  rm -rf "$temporary"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_for_listener() {
  local namespace="$1" port="$2" attempt
  for attempt in {1..100}; do
    if ip netns exec "$namespace" ss -H -lnt "sport = :$port" | grep -q .; then
      return 0
    fi
    sleep 0.1
  done
  echo "listener $namespace:$port did not become ready" >&2
  return 1
}

ip netns add "$router"
ip netns add "$guest"
ip netns add "$remote"
ip link add "$test_tap" type veth peer name "$guest_tap"
ip link set "$test_tap" netns "$router"
ip link set "$guest_tap" netns "$guest"
ip netns exec "$router" ip link set "$test_tap" name ci-tap0
ip netns exec "$guest" ip link set "$guest_tap" name guest0
ip link add "$router_uplink" type veth peer name "$remote_if"
ip link set "$router_uplink" netns "$router"
ip link set "$remote_if" netns "$remote"

ip netns exec "$router" ip link add virbr-ci0 type bridge
ip netns exec "$router" ip link set ci-tap0 master virbr-ci0
ip netns exec "$router" ip addr add 192.168.123.1/24 dev virbr-ci0
ip netns exec "$router" ip link set lo up
ip netns exec "$router" ip link set ci-tap0 up
ip netns exec "$router" ip link set virbr-ci0 up
ip netns exec "$router" ip addr add 8.8.8.1/24 dev "$router_uplink"
ip netns exec "$router" ip link set "$router_uplink" up
ip netns exec "$router" sysctl -q -w net.ipv4.ip_forward=1

ip netns exec "$guest" ip link set lo up
ip netns exec "$guest" ip addr add 192.168.123.2/24 dev guest0
ip netns exec "$guest" ip link set guest0 up
ip netns exec "$guest" ip route add default via 192.168.123.1
ip netns exec "$remote" ip link set lo up
ip netns exec "$remote" ip addr add 10.1.0.200/32 dev lo
ip netns exec "$remote" ip addr add 8.8.8.2/24 dev "$remote_if"
ip netns exec "$remote" ip link set "$remote_if" up
ip netns exec "$remote" ip route add 192.168.123.0/24 via 8.8.8.1
ip netns exec "$router" ip route add 10.1.0.200/32 via 8.8.8.2 dev "$router_uplink"

ip netns exec "$router" nft -f "$script_dir/solarinet-ci-guard.nft"

drop_rule_packets() {
  ip netns exec "$router" nft -a list chain inet solarinet_ci_guard forward |
    awk '/^[[:space:]]*oifname \{.*virbr-ci0.*\} counter.*drop/ { for (i = 1; i <= NF; i++) if ($i == "packets") { packets = $(i + 1); break } } END { if (packets != "") print packets }'
}

egress_drop_rule_packets() {
  ip netns exec "$router" nft -a list chain inet solarinet_ci_guard forward |
    awk '/^[[:space:]]*iifname \{.*virbr-ci0.*\} counter.*drop/ { for (i = 1; i <= NF; i++) if ($i == "packets") { packets = $(i + 1); break } } END { if (packets != "") print packets }'
}

private_https_drop_rule_packets() {
  ip netns exec "$router" nft -a list chain inet solarinet_ci_guard forward |
    awk '/^[[:space:]]*iifname \{.*virbr-ci0.*\} ip daddr 10\.0\.0\.0\/8 counter.*drop/ { for (i = 1; i <= NF; i++) if ($i == "packets") { packets = $(i + 1); break } } END { if (packets != "") print packets }'
}

ip netns exec "$remote" python3 -c '
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.settimeout(10)
s.bind(("0.0.0.0", 443))
s.listen(1)
for _ in range(2):
    c, _ = s.accept()
    data = c.recv(8)
    c.sendall(b"reply:" + data)
    c.close()
' >"$temporary/remote-server.log" 2>&1 &
server_pid=$!
wait_for_listener "$remote" 443
ip netns exec "$guest" python3 -c '
import socket
s = socket.socket()
s.settimeout(2)
s.connect(("8.8.8.2", 443))
s.sendall(b"hello")
assert s.recv(16) == b"reply:hello"
'

before_private_https_packets="$(private_https_drop_rule_packets)"
if ip netns exec "$guest" python3 -c '
import socket
s = socket.socket()
s.settimeout(2)
s.connect(("10.1.0.200", 443))
' >/dev/null 2>&1; then
  echo 'HTTPS to a private-network address reached the remote listener' >&2
  exit 1
fi
after_private_https_packets="$(private_https_drop_rule_packets)"
if [[ ! "$before_private_https_packets" =~ ^[0-9]+$ || ! "$after_private_https_packets" =~ ^[0-9]+$ ]] ||
  (( after_private_https_packets <= before_private_https_packets )); then
  echo "private HTTPS did not increment the 10/8 destination drop counter (before=$before_private_https_packets after=$after_private_https_packets)" >&2
  ip netns exec "$router" nft -a list chain inet solarinet_ci_guard forward >&2
  exit 1
fi

ip netns exec "$guest" python3 -c '
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("192.168.123.2", 2222))
s.listen(1)
s.settimeout(10)
try:
    c, _ = s.accept()
    c.sendall(b"unexpected")
    c.close()
except TimeoutError:
    pass
' >"$temporary/guest-listener.log" 2>&1 &
listener_pid=$!
wait_for_listener "$guest" 2222

before_drop_packets="$(drop_rule_packets)"
if ip netns exec "$remote" python3 -c '
import socket
s = socket.socket()
s.settimeout(2)
s.connect(("192.168.123.2", 2222))
' >/dev/null 2>&1; then
  echo 'unexpected new forwarded connection reached the runner namespace' >&2
  exit 1
fi
after_drop_packets="$(drop_rule_packets)"
if [[ ! "$before_drop_packets" =~ ^[0-9]+$ || ! "$after_drop_packets" =~ ^[0-9]+$ ]] ||
  (( after_drop_packets <= before_drop_packets )); then
  echo "unsolicited flow did not increment the guest-forward drop rule counter (before=$before_drop_packets after=$after_drop_packets)" >&2
  ip netns exec "$router" nft -a list chain inet solarinet_ci_guard forward >&2
  exit 1
fi

ip netns exec "$remote" python3 -c '
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.settimeout(8)
s.bind(("8.8.8.2", 22))
s.listen(1)
try:
    c, _ = s.accept()
    c.close()
    raise SystemExit("forbidden SSH egress reached the remote listener")
except TimeoutError:
    pass
' >"$temporary/ssh-server.log" 2>&1 &
ssh_server_pid=$!
wait_for_listener "$remote" 22
before_egress_drop_packets="$(egress_drop_rule_packets)"
if ip netns exec "$guest" python3 -c '
import socket
s = socket.socket()
s.settimeout(2)
s.connect(("8.8.8.2", 22))
' >/dev/null 2>&1; then
  echo 'forbidden SSH egress reached the remote network' >&2
  exit 1
fi
after_egress_drop_packets="$(egress_drop_rule_packets)"
if [[ ! "$before_egress_drop_packets" =~ ^[0-9]+$ || ! "$after_egress_drop_packets" =~ ^[0-9]+$ ]] ||
  (( after_egress_drop_packets <= before_egress_drop_packets )); then
  echo "forbidden SSH egress did not increment the guest egress drop rule counter (before=$before_egress_drop_packets after=$after_egress_drop_packets)" >&2
  ip netns exec "$router" nft -a list chain inet solarinet_ci_guard forward >&2
  exit 1
fi

echo 'isolated firewall namespace test passed: public HTTPS egress/reply allowed, private HTTPS and SSH egress blocked, unsolicited forwarding dropped'
