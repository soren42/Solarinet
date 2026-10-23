#!/bin/bash
set -euo pipefail

readonly image_dir=/var/lib/libvirt/images/solarinet-ci
readonly image_base="$image_dir/ubuntu-24.04-server-cloudimg-amd64.img"
readonly seed_iso="$image_dir/seed.iso"
readonly work_dir=/run/solarinet-ci
readonly domain=solarinet-ci-runner
readonly ubuntu_sha256=04e8f2461a16a44e78a370284471f29616cdd9baf1893df25807b71f985b6c94
readonly ubuntu_image_url=https://cloud-images.ubuntu.com/releases/noble/release-20261017/ubuntu-24.04-server-cloudimg-amd64.img
readonly register_host=cesium.akoria.net
readonly register_user=jason
readonly bootstrap_timeout_seconds=2700
readonly timeout_seconds=86400
serial_capture_pid=''
active_pid=''

umask 077
install -d -o root -g root -m 0700 "$work_dir"
install -d -o root -g root -m 0755 "$image_dir"

stop_serial_capture() {
  local state attempt
  [[ -n "$serial_capture_pid" ]] || return 0
  for ((attempt = 0; attempt < 20; attempt++)); do
    state="$(ps -o stat= -p "$serial_capture_pid" 2>/dev/null || true)"
    [[ -z "$state" || "$state" == Z* ]] && break
    sleep 0.1
  done
  state="$(ps -o stat= -p "$serial_capture_pid" 2>/dev/null || true)"
  if [[ -n "$state" && "$state" != Z* ]]; then
    kill "$serial_capture_pid" >/dev/null 2>&1 || true
  fi
  wait "$serial_capture_pid" >/dev/null 2>&1 || true
  serial_capture_pid=''
}

cleanup() {
  local domains xml state
  stop_serial_capture
  domains="$(/usr/bin/virsh list --all --name)" || return 1
  if grep -Fxq "$domain" <<<"$domains"; then
    xml="$(/usr/bin/virsh dumpxml "$domain")" || return 1
    if [[ "$xml" != *'<description>SolariNet disposable CI runner</description>'* || \
          "$xml" != *"$image_dir/overlay.qcow2"* ]]; then
      echo "refusing to remove unexpected libvirt domain named $domain" >&2
      return 1
    fi
    state="$(/usr/bin/virsh domstate "$domain")" || return 1
    case "$state" in
      'shut off'|'crashed') ;;
      *) /usr/bin/virsh destroy "$domain" >/dev/null || return 1 ;;
    esac
    /usr/bin/virsh undefine "$domain" >/dev/null || return 1
    if /usr/bin/virsh dominfo "$domain" >/dev/null 2>&1; then
      echo "libvirt domain $domain remains defined after cleanup" >&2
      return 1
    fi
  fi
  rm -f "$work_dir"/user-data "$work_dir"/meta-data "$work_dir"/guest-bootstrap \
    "$work_dir"/runner-token "$work_dir"/runner-uuid "$work_dir"/serial.log \
    "$work_dir"/job-started "$work_dir"/job-exit "$work_dir"/job-started.*.tmp \
    "$work_dir"/job-exit.*.tmp "$seed_iso" \
    "$image_dir"/overlay.qcow2 "$image_base.part"
}

ensure_base() {
  if [[ ! -s "$image_base" ]]; then
    local tmp="$image_base.part"
    run_cancellable curl --fail --location --retry 4 --retry-delay 2 \
      --connect-timeout 15 --max-time 1800 "$ubuntu_image_url" -o "$tmp" || return 1
    printf '%s  %s\n' "$ubuntu_sha256" "$tmp" | sha256sum --check --status || return 1
    mv "$tmp" "$image_base" || return 1
    chown qemu:qemu "$image_base" || return 1
    chmod 0440 "$image_base" || return 1
  fi
  printf '%s  %s\n' "$ubuntu_sha256" "$image_base" | sha256sum --check --status || {
    echo 'Ubuntu runner base image failed its pinned SHA-256 check' >&2
    return 1
  }
  chown qemu:qemu "$image_base" || return 1
  chmod 0440 "$image_base" || return 1
}

run_cancellable() {
  "$@" &
  active_pid=$!
  local status=0
  wait "$active_pid" || status=$?
  active_pid=''
  return "$status"
}

new_registration() {
  local suffix secret uuid
  suffix="$(openssl rand -hex 12)" || return 1
  secret="534f4c4152494349${suffix}"
  local registration_output
  registration_output="$(printf '%s' "$secret" | timeout 60 ssh -o BatchMode=yes -o IdentitiesOnly=yes \
    -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
    -o StrictHostKeyChecking=yes \
    -o UserKnownHostsFile=/etc/solarinet-ci/known_hosts \
    -i /etc/solarinet-ci/registration_key \
    "$register_user@$register_host")" || return 1
  uuid="$(printf '%s\n' "$registration_output" | grep -Eo '[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}' | tail -n 1)"
  [[ "$uuid" =~ ^[0-9a-fA-F-]{36}$ ]] || {
    echo 'Forgejo registration did not return a UUID' >&2
    return 1
  }
  printf '%s' "$secret" >"$work_dir/runner-token" || return 1
  printf '%s' "$uuid" >"$work_dir/runner-uuid" || return 1
}

start_job_vm() {
  python3 "$(dirname "$0")/render-user-data.py" \
    "$work_dir/runner-token" "$work_dir/runner-uuid" \
    "$(dirname "$0")/guest-runner.sh" >"$work_dir/user-data" || return 1
  cat >"$work_dir/meta-data" <<EOF
instance-id: solarinet-ci-$(date +%s)
local-hostname: solarinet-ci
EOF
  xorriso -as mkisofs -quiet -output "$seed_iso" -volid cidata \
    -joliet -rock "$work_dir/user-data" "$work_dir/meta-data" || return 1
  chown qemu:qemu "$seed_iso" || return 1
  chmod 0400 "$seed_iso" || return 1
  /usr/bin/qemu-img create -q -f qcow2 -F qcow2 -b "$image_base" \
    "$image_dir/overlay.qcow2" 32G || return 1
  chown qemu:qemu "$image_dir/overlay.qcow2" || return 1
  chmod 0600 "$image_dir/overlay.qcow2" || return 1
  run_cancellable /usr/bin/virt-install --name "$domain" \
    --description 'SolariNet disposable CI runner' --memory 10240 --vcpus 6 \
    --os-variant ubuntu24.04 --import --noautoconsole \
    --disk path="$image_dir/overlay.qcow2",format=qcow2,bus=virtio \
    --disk path="$seed_iso",device=cdrom \
    --network network=solarinet-ci,model=virtio,mac=52:54:00:12:34:56,target=ci-tap0 \
    --graphics none --serial pty --console pty,target.type=serial \
    --boot hd,firmware=bios || return 1
  if ! /usr/bin/virsh dumpxml "$domain" | grep -q "<target dev='ci-tap0'"; then
    echo 'runner interface is not attached to the firewall-pinned ci-tap0 device' >&2
    return 1
  fi
  local serial_device
  serial_device="$(/usr/bin/virsh ttyconsole "$domain")" || return 1
  [[ -c "$serial_device" ]] || {
    echo "runner serial console is not a character device: $serial_device" >&2
    return 1
  }
  stty -F "$serial_device" raw -echo || return 1
  python3 "$(dirname "$0")/serial-capture.py" "$serial_device" \
    "$work_dir/serial.log" "$work_dir/job-started" "$work_dir/job-exit" &
  serial_capture_pid=$!
}

run_one_cycle() {
  local deadline state state_errors=0 job_started=0 timeout_kind=bootstrap
  cleanup || return 1
  ensure_base || return 1
  new_registration || return 1
  start_job_vm || return 1
  deadline=$(( $(date +%s) + bootstrap_timeout_seconds ))
  while (( $(date +%s) < deadline )); do
    if ! state="$(/usr/bin/virsh domstate "$domain" 2>/dev/null)"; then
      state_errors=$((state_errors + 1))
      if (( state_errors == 1 || state_errors % 12 == 0 )); then
        echo "libvirt has not reported runner VM state (consecutive failures: $state_errors)" >&2
      fi
      run_cancellable sleep 5 || return 1
      continue
    fi
    if (( job_started == 0 )) && [[ -f "$work_dir/job-started" ]]; then
      job_started=1
      timeout_kind=job
      deadline=$(( $(date +%s) + timeout_seconds ))
      echo "Forgejo job started; granting it a ${timeout_seconds}s execution window"
    fi
    state_errors=0
    case "$state" in
      'shut off')
        if [[ -n "$serial_capture_pid" ]]; then
          stop_serial_capture
        fi
        local guest_rc
        guest_rc=''
        if [[ -f "$work_dir/job-exit" ]]; then
          guest_rc="$(cat "$work_dir/job-exit")"
        fi
        if [[ ! "$guest_rc" =~ ^[0-9]+$ ]]; then
          echo 'runner VM powered off without reporting a completed Forgejo job' >&2
          return 1
        fi
        if [[ "$guest_rc" != 0 ]]; then
          echo "Forgejo one-job runner exited with status $guest_rc" >&2
          return 1
        fi
        return 0
        ;;
      'crashed') echo 'runner VM crashed' >&2; return 1 ;;
    esac
    run_cancellable sleep 5 || return 1
  done
  echo "runner VM exceeded ${timeout_kind} timeout; destroying it" >&2
  return 1
}

failure_streak=0
trap cleanup EXIT
stop_supervisor() {
  if [[ -n "$active_pid" ]]; then
    kill -TERM "$active_pid" >/dev/null 2>&1 || true
    wait "$active_pid" >/dev/null 2>&1 || true
    active_pid=''
  fi
  exit 0
}
trap stop_supervisor INT TERM
while true; do
  if run_one_cycle; then
    cleanup || exit 1
    failure_streak=0
  else
    cleanup || exit 1
    failure_streak=$((failure_streak + 1))
  fi
  # Back off after host-side failures. Every completed guest cycle still gets a
  # short teardown pause so the next ephemeral registration does not race libvirt.
  if (( failure_streak > 0 )); then
    delay_exponent=$((failure_streak > 6 ? 6 : failure_streak - 1))
    retry_delay=$((30 * (2 ** delay_exponent)))
  else
    retry_delay=30
  fi
  run_cancellable sleep "$retry_delay" || exit 1
done
