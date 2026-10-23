#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

assert_blocked() {
	local output
	if output="$(env -u SOLARI_ALLOW_UNVERIFIED_PROVISIONING "$@" 2>&1)"; then
		printf 'expected F-07 gate to block: %s\n' "$*" >&2
		exit 1
	fi
	case "$output" in
	*"F-07: unsigned provisioning is disabled."*) ;;
	*)
		printf 'missing F-07 denial for %s:\n%s\n' "$*" "$output" >&2
		exit 1
		;;
	esac
}

assert_blocked bash "$ROOT/deploy/fleet/fleet-provision.sh" \
	--target aa-bb-cc-dd-ee-ff --distro debian --arch amd64 --hostname gate-test
assert_blocked bash "$ROOT/deploy/fleet/fleet-image.sh" \
	--hostname gate-test --arch arm64
assert_blocked bash "$ROOT/deploy/fleet/netboot/build-ipxe-loaders.sh" \
	--http-base http://127.0.0.1:8080
assert_blocked bash "$ROOT/deploy/fleet/pi/build-pi-image.sh" \
	--hostname gate-test --arch arm64 --ssh-pubkey 'ssh-ed25519 test' \
	--out /dev/null
assert_blocked bash "$ROOT/deploy/fleet/pi/pi5-netboot/setup-pi5-netboot.sh" \
	--serial 12345678 --hostname gate-test --img /missing/test.img

override_output="$(SOLARI_ALLOW_UNVERIFIED_PROVISIONING=1 bash -c \
	'. "$1"; fleet_require_unverified_override' _ \
	"$ROOT/deploy/fleet/provisioning-guard.sh" 2>&1)"
case "$override_output" in
*"F-07 override active"*) ;;
*)
	printf 'unsafe lab override did not emit warning:\n%s\n' "$override_output" >&2
	exit 1
	;;
esac

printf '%s\n' 'F-07 unsigned provisioning entry points fail closed by default.'
