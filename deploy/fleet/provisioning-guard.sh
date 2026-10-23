#!/bin/sh
# Shared fail-closed gate until F-07's signed provisioning chain is implemented.
fleet_require_unverified_override() {
	if [ "${SOLARI_ALLOW_UNVERIFIED_PROVISIONING:-0}" != "1" ]; then
		printf '%s\n' '[fleet][error] F-07: unsigned provisioning is disabled.' \
			'Use SOLARI_ALLOW_UNVERIFIED_PROVISIONING=1 only on an isolated lab network.' >&2
		return 1
	fi
	printf '%s\n' '[fleet][warn] F-07 override active: unsigned provisioning is unsafe; use only on an isolated lab network.' >&2
}
