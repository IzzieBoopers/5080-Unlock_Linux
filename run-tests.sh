#!/usr/bin/env bash
# Quick validation for omen_wmi_boost (requires root).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
KO="$ROOT/omen_wmi_boost/omen_wmi_boost.ko"
SYSFS="/sys/kernel/omen_wmi_boost"

if [[ $EUID -ne 0 ]]; then
	exec sudo -k "$0" "$@"
fi

log() { printf '[%s] %s\n' "$(date -Iseconds)" "$*"; }

[[ -f "$KO" ]] || make -C "$ROOT/omen_wmi_boost"

modprobe wmi 2>/dev/null || true
rmmod omen_wmi_boost 2>/dev/null || true

log "Phase 1: read (one-shot)"
insmod "$KO" persist=0 boot_mode=read
rmmod omen_wmi_boost

log "Phase 2: performance (persistent + sysfs)"
insmod "$KO" persist=1 auto_boost=1
cat "$SYSFS/gpu_state"
nvidia-smi -q -d POWER 2>/dev/null | grep -E "Current Power|Max Power" || true

log "Phase 3: sysfs boost off/on"
echo 0 > "$SYSFS/boost"
cat "$SYSFS/gpu_state"
echo 1 > "$SYSFS/performance"
cat "$SYSFS/gpu_state"
nvidia-smi -q -d POWER 2>/dev/null | grep -E "Current Power|Max Power" || true

log "Done — leave loaded: sudo make -C omen_wmi_boost load"
log "Unload: sudo rmmod omen_wmi_boost"
