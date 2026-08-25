#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Static/unit checks by default; privileged hardware checks only on request.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
KO="$ROOT/omen_wmi_boost/omen_wmi_boost.ko"
SYSFS="/sys/kernel/omen_wmi_boost"

log() { printf '[%s] %s\n' "$(date -Iseconds)" "$*"; }

run_static() {
	log "Checking shell syntax"
	bash -n \
		"$ROOT/install.sh" \
		"$ROOT/kernel/install.d/zz-omen-wmi-boost.install" \
		"$ROOT/scripts/omen-wmi-boost-rebuild" \
		"$ROOT/scripts/omen-wmi-boost-verify" \
		"$ROOT/scripts/omen-wmi-notify"

	python3 -m py_compile \
		"$ROOT/scripts/omen_wmi_usb_s5.py" \
		"$ROOT/scripts/omen-wmi-usb-s5-guard" \
		"$ROOT/scripts/omen-wmi-boost-disarm"

	if command -v shellcheck >/dev/null; then
		log "Running ShellCheck"
		shellcheck \
			"$ROOT/install.sh" \
			"$ROOT/run-tests.sh" \
			"$ROOT/kernel/install.d/zz-omen-wmi-boost.install" \
			"$ROOT/scripts/omen-wmi-boost-rebuild" \
			"$ROOT/scripts/omen-wmi-boost-verify" \
			"$ROOT/scripts/omen-wmi-notify"
	fi

	log "Running fan policy unit tests"
	python3 -m unittest discover -s "$ROOT/tests" -v

	log "Validating systemd units"
	unit_dir=$(mktemp -d)
	sed "s|/usr/local/sbin|${ROOT}/scripts|g" \
		"$ROOT/systemd/omen-wmi-boost-verify.service" \
		> "${unit_dir}/omen-wmi-boost-verify.service"
	sed "s|/usr/local/sbin|${ROOT}/scripts|g" \
		"$ROOT/systemd/omen-wmi-fan-control.service" \
		> "${unit_dir}/omen-wmi-fan-control.service"
	sed "s|/usr/local/sbin|${ROOT}/scripts|g" \
		"$ROOT/systemd/omen-wmi-usb-s5-guard.service" \
		> "${unit_dir}/omen-wmi-usb-s5-guard.service"
	cp "$ROOT/systemd/omen-wmi-usb-s5-inhibit.service" \
		"${unit_dir}/omen-wmi-usb-s5-inhibit.service"
	systemd-analyze verify "${unit_dir}"/*.service
	rm -rf "$unit_dir"

	log "Building kernel module for $(uname -r)"
	make -C "$ROOT/omen_wmi_boost" clean
	make -C "$ROOT/omen_wmi_boost"

	log "Static checks passed"
}

restore_hardware_state() {
	rm -f /run/omen-wmi-thermal-test.conf /run/omen-wmi-thermal-test.state
	if [[ -w "$SYSFS/fan_mode" ]]; then
		echo auto >"$SYSFS/fan_mode" || true
	fi
	modprobe -r omen_wmi_boost 2>/dev/null || true
	modprobe omen_wmi_boost 2>/dev/null || true
	systemctl start omen-wmi-fan-control.service 2>/dev/null || true
}

run_hardware() {
	if [[ $EUID -ne 0 ]]; then
		exec sudo -k "$0" --hardware
	fi

	run_static
	trap restore_hardware_state EXIT
	systemctl stop omen-wmi-fan-control.service 2>/dev/null || true
	modprobe wmi 2>/dev/null || true
	modprobe -r omen_wmi_boost 2>/dev/null || true

	log "Reading firmware state with a one-shot module load"
	insmod "$KO" persist=0 boot_mode=read
	rmmod omen_wmi_boost

	log "Loading performance path and checking sysfs"
	insmod "$KO" persist=1 auto_boost=1
	awk '{ print }' "$SYSFS/gpu_state"
	awk '{ print }' "$SYSFS/fan_state"

	log "Checking safe manual-to-auto fan handoff"
	echo manual >"$SYSFS/fan_mode"
	echo 30 >"$SYSFS/fan_speed"
	echo auto >"$SYSFS/fan_mode"
	[[ "$(<"$SYSFS/fan_mode")" == "auto" ]]

	log "Checking boost disable and re-apply"
	echo 0 >"$SYSFS/boost"
	echo 1 >"$SYSFS/performance"
	awk '{ print }' "$SYSFS/gpu_state"

	log "Simulating warning policy below the current temperature"
	cat >/run/omen-wmi-thermal-test.conf <<'EOF'
fan_curve=20:50,82:100
target_temp_c=70
warning_temp_c=30
unlock_disable_temp_c=100
critical_samples=1
EOF
	warning_output=$(
		"$ROOT/scripts/omen-wmi-fan-control" \
			--config /run/omen-wmi-thermal-test.conf \
			--notifier /bin/true --dry-run --once 2>&1
	)
	[[ "$warning_output" == *"notification: GPU temperature warning"* ]]

	log "Simulating critical protection below the current temperature"
	cat >/run/omen-wmi-thermal-test.conf <<'EOF'
fan_curve=20:50,35:100
target_temp_c=30
warning_temp_c=35
unlock_disable_temp_c=40
critical_samples=1
EOF
	"$ROOT/scripts/omen-wmi-fan-control" \
		--config /run/omen-wmi-thermal-test.conf \
		--notifier /bin/true \
		--thermal-state /run/omen-wmi-thermal-test.state --once
	gpu_state=$(<"$SYSFS/gpu_state")
	[[ "$gpu_state" == *"ctgp=0"* ]]
	[[ "$(<"$SYSFS/fan_mode")" == "max" ]]
	[[ "$(<"/run/omen-wmi-thermal-test.state")" == *"boost_disabled=1"* ]]
	echo auto >"$SYSFS/fan_mode"
	echo 1 >"$SYSFS/performance"

	log "Hardware checks passed; restoring installed service state"
}

case "${1:-}" in
	"") run_static ;;
	--hardware) run_hardware ;;
	*) echo "Usage: $0 [--hardware]" >&2; exit 2 ;;
esac
