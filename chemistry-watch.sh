#!/usr/bin/env bash
# chemistry-watch — capture chemistry (UDR7) logs + resource trend to xenon so the
# NEXT hard-freeze is recorded off-box. Chemistry's own journal is tiny (~80MB, a
# few hours) and a hard hang saves no panic, so we mirror it here where it's safe.
#
# Two independent streams into /var/log/chemistry-watch/:
#   journal.log  — live `journalctl -f` follow (reconnects across reboots/hangs)
#   stats.log    — every 30s: loadavg, MemAvailable, Committed_AS, temps, top CPU.
#                  A "POLL-FAILED" line timestamps the moment chemistry goes
#                  unreachable — i.e. the freeze onset, bracketing the event.
set -u

TARGET="root@10.0.0.1"
OUTDIR="/home/jason/chemistry-watch"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=2)

mkdir -p "$OUTDIR"

journal_follow() {
    while true; do
        echo "[$(date -Is)] --- (re)connecting journal follow ---" >> "$OUTDIR/journal.log"
        ssh "${SSH_OPTS[@]}" "$TARGET" 'journalctl -f -o short-iso -n 0' \
            >> "$OUTDIR/journal.log" 2>> "$OUTDIR/journal.err" || true
        sleep 5
    done
}

# Low-latency kernel ring-buffer follow. journalctl buffers; `dmesg -w` delivers
# kernel printk (oops / driver fault / lockup) with minimal delay — the best shot
# at catching the SoC's dying words in the instant before a hard freeze.
kernel_follow() {
    while true; do
        echo "[$(date -Is)] --- (re)connecting dmesg follow ---" >> "$OUTDIR/kernel.log"
        ssh "${SSH_OPTS[@]}" "$TARGET" 'dmesg -w -T' \
            >> "$OUTDIR/kernel.log" 2>> "$OUTDIR/journal.err" || true
        sleep 5
    done
}

stats_poll() {
    local snap
    while true; do
        if snap=$(ssh "${SSH_OPTS[@]}" "$TARGET" \
                'echo "LOAD=$(cut -d" " -f1-3 /proc/loadavg)" \
                      "MEM=$(awk "/MemAvailable/{a=\$2}/Committed_AS/{c=\$2}END{print a\"/\"c\"kB\"}" /proc/meminfo)" \
                      "TEMPS=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | tr "\n" "," )" \
                      "TOP=$(ps -eo pcpu,comm --sort=-pcpu | awk "NR>1&&NR<5" | tr "\n" ";")"' 2>/dev/null); then
            echo "[$(date -Is)] $snap" >> "$OUTDIR/stats.log"
        else
            echo "[$(date -Is)] POLL-FAILED (chemistry unreachable — possible freeze)" >> "$OUTDIR/stats.log"
        fi
        sleep 30
    done
}

journal_follow &
kernel_follow &
stats_poll &
wait
