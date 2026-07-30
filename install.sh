#!/usr/bin/bash
# SPDX-License-Identifier: MIT
# One-time install: boot persistence for omen_wmi_boost (max performance).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="${REPO_ROOT}/omen_wmi_boost"
SRC_INSTALL=/usr/src/omen_wmi_boost
KVER="$(uname -r)"
FORCE_UNSUPPORTED=0

log() { printf '==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

require_root() {
	[[ $EUID -eq 0 ]] || die "Run as root: sudo $0"
}

check_build_deps() {
	[[ -d "/lib/modules/${KVER}/build" ]] || die \
		"kernel headers missing for ${KVER}. Install: dnf install kernel-devel-${KVER}"
	command -v make >/dev/null || die "make not found"
	command -v python3 >/dev/null || die "python3 not found"
}

warn_secure_boot() {
	if command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -qi enabled; then
		printf '\nWARNING: Secure Boot is enabled. Unsigned modules will NOT load at boot.\n'
		printf '  Enroll the module with MOK or disable Secure Boot. See docs/BOOT-SETUP.md\n\n'
	fi
}

show_path_status() {
	local path="$1"

	if [[ -e "$path" ]]; then
		printf '  present: %s\n' "$path"
	else
		printf '  absent:  %s\n' "$path"
	fi
}

build_module() {
	log "Building omen_wmi_boost for kernel ${KVER}"
	make -C "$MODULE_DIR" clean 2>/dev/null || true
	make -C "$MODULE_DIR"
}

install_module() {
	local installed

	log "Installing module to /lib/modules/${KVER}"
	make -C "$MODULE_DIR" modules_install
	depmod -a "$KVER"
	installed=$(modinfo -k "$KVER" -n omen_wmi_boost 2>/dev/null || true)
	[[ -n "$installed" ]] || die "module installed but modinfo cannot locate it"
	log "Installed module: ${installed}"
}

install_source_tree() {
	log "Installing source to ${SRC_INSTALL}"
	install -d "$SRC_INSTALL"
	install -m 644 "${MODULE_DIR}/omen_wmi_boost.c" "${MODULE_DIR}/Makefile" "$SRC_INSTALL/"
}

install_configs() {
	log "Installing modprobe and modules-load configs"
	install -d /etc/modprobe.d /etc/modules-load.d
	if [[ "$FORCE_UNSUPPORTED" -eq 1 ]]; then
		log "WARNING: enabling unsupported-board override"
		sed 's/force_unsupported=0/force_unsupported=1/' \
			"${REPO_ROOT}/modprobe.d/omen_wmi_boost.conf" \
			>/etc/modprobe.d/omen_wmi_boost.conf
		chmod 644 /etc/modprobe.d/omen_wmi_boost.conf
	else
		install -m 644 \
			"${REPO_ROOT}/modprobe.d/omen_wmi_boost.conf" /etc/modprobe.d/
	fi
	install -m 644 "${REPO_ROOT}/modules-load.d/omen_wmi_boost.conf" /etc/modules-load.d/
	if [[ -e /etc/omen-wmi-fan-control.conf ]]; then
		log "Preserving existing /etc/omen-wmi-fan-control.conf"
	else
		install -m 644 "${REPO_ROOT}/config/omen-wmi-fan-control.conf" /etc/
	fi
	install -m 644 "${REPO_ROOT}/config/omen-wmi-fan-control.conf.example" \
		/etc/omen-wmi-fan-control.conf.example
}

install_scripts() {
	log "Installing helper scripts"
	install -d /usr/local/sbin
	install -m 755 "${REPO_ROOT}/scripts/omen-wmi-boost-verify" /usr/local/sbin/
	install -m 755 "${REPO_ROOT}/scripts/omen-wmi-boost-rebuild" /usr/local/sbin/
	install -m 755 "${REPO_ROOT}/scripts/omen-wmi-fan-control" /usr/local/sbin/
	install -m 755 "${REPO_ROOT}/scripts/omen-wmi-notify" /usr/local/sbin/
}

install_systemd() {
	log "Installing systemd units"
	install -d /etc/systemd/system
	install -m 644 "${REPO_ROOT}/systemd/omen-wmi-boost-verify.service" /etc/systemd/system/
	install -m 644 "${REPO_ROOT}/systemd/omen-wmi-fan-control.service" /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable omen-wmi-boost-verify.service
}

enable_controllers() {
	log "Enabling fan controller service"
	systemctl enable omen-wmi-fan-control.service
	systemctl restart omen-wmi-fan-control.service
}

install_kernel_hook() {
	log "Installing kernel-install.d hook"
	install -d /etc/kernel/install.d
	install -m 755 "${REPO_ROOT}/kernel/install.d/zz-omen-wmi-boost.install" \
		/etc/kernel/install.d/zz-omen-wmi-boost.install
}

install_docs() {
	log "Installing documentation"
	install -d /usr/share/doc/omen-wmi-boost
	install -m 644 \
		"${REPO_ROOT}/README.md" \
		"${REPO_ROOT}/CHANGELOG.md" \
		"${REPO_ROOT}/LICENSE" \
		"${REPO_ROOT}/docs/BOOT-SETUP.md" \
		"${REPO_ROOT}/docs/FAN-CONTROL.md" \
		"${REPO_ROOT}/docs/FIRMWARE-CONTROLS.md" \
		"${REPO_ROOT}/docs/REVERSE-ENGINEERING.md" \
		/usr/share/doc/omen-wmi-boost/
	install -d /usr/share/doc/omen-wmi-boost/LICENSES
	install -m 644 "${REPO_ROOT}/LICENSES/"* \
		/usr/share/doc/omen-wmi-boost/LICENSES/
}

smoke_test() {
	log "Smoke test: load module and verify"
	systemctl stop omen-wmi-fan-control.service 2>/dev/null || true
	modprobe wmi 2>/dev/null || true
	modprobe -r omen_wmi_boost 2>/dev/null || true
	modprobe omen_wmi_boost || die "modprobe omen_wmi_boost failed — check dmesg"

	if ! /usr/local/sbin/omen-wmi-boost-verify; then
		die "Verification failed after install — check journalctl -t omen-wmi-boost -b"
	fi
	log "Smoke test passed"
}

disable_boost() {
	local state

	log "Reverting GPU boost state"

	if [[ -w /sys/kernel/omen_wmi_boost/boost ]]; then
		echo 0 >/sys/kernel/omen_wmi_boost/boost || die \
			"Failed to disable boost through sysfs — leaving install in place"

		if [[ -r /sys/kernel/omen_wmi_boost/gpu_state ]]; then
			read -r state </sys/kernel/omen_wmi_boost/gpu_state || state="unreadable"
			log "GPU state after disable: ${state}"
		fi
		return 0
	fi

	if [[ -n "$(
		find "/lib/modules/${KVER}" \
			-name 'omen_wmi_boost.ko*' -print -quit 2>/dev/null
	)" ]]; then
		log "Module is not loaded; sending one-shot disable command"
		modprobe wmi 2>/dev/null || true
		modprobe omen_wmi_boost persist=0 auto_boost=0 boot_mode=disable || die \
			"Failed to run one-shot disable — leaving install in place"
		modprobe -r omen_wmi_boost 2>/dev/null || true
		return 0
	fi

	log "No loaded or installed module found; skipping active boost disable"
}

do_dry_run() {
	log "Dry run only; no system changes will be made"
	log "Running kernel: ${KVER}"

	if [[ -d "/lib/modules/${KVER}/build" ]]; then
		log "Kernel headers found: /lib/modules/${KVER}/build"
	else
		log "Kernel headers missing: /lib/modules/${KVER}/build"
	fi

	if command -v make >/dev/null; then
		log "make found: $(command -v make)"
	else
		log "make not found"
	fi

	warn_secure_boot

	log "Install targets"
	if module_path=$(modinfo -k "$KVER" -n omen_wmi_boost 2>/dev/null); then
		log "Installed module: ${module_path}"
	else
		log "No module currently installed for ${KVER}"
	fi
	show_path_status /etc/modules-load.d/omen_wmi_boost.conf
	show_path_status /etc/modprobe.d/omen_wmi_boost.conf
	show_path_status /etc/omen-wmi-fan-control.conf
	show_path_status /etc/omen-wmi-fan-control.conf.example
	show_path_status /etc/systemd/system/omen-wmi-boost-verify.service
	show_path_status /etc/systemd/system/omen-wmi-fan-control.service
	show_path_status /etc/kernel/install.d/zz-omen-wmi-boost.install
	show_path_status /usr/local/sbin/omen-wmi-boost-verify
	show_path_status /usr/local/sbin/omen-wmi-boost-rebuild
	show_path_status /usr/local/sbin/omen-wmi-fan-control
	show_path_status /usr/local/sbin/omen-wmi-notify

	if [[ -d /sys/module/omen_wmi_boost ]]; then
		log "Module is currently loaded"
	else
		log "Module is not currently loaded"
	fi

	if [[ -r /sys/kernel/omen_wmi_boost/gpu_state ]]; then
		log "Current GPU state: $(< /sys/kernel/omen_wmi_boost/gpu_state)"
	fi
	if [[ -r /sys/kernel/omen_wmi_boost/fan_state ]]; then
		log "Current fan state:"
		sed 's/^/  /' /sys/kernel/omen_wmi_boost/fan_state
	fi
}

do_install() {
	require_root
	check_build_deps
	warn_secure_boot
	build_module
	install_module
	install_source_tree
	install_configs
	install_scripts
	install_systemd
	install_kernel_hook
	install_docs
	smoke_test
	enable_controllers
	mkdir -p /var/lib/omen_wmi_boost
	rm -f /var/lib/omen_wmi_boost/rebuild-needed

	printf '\n'
	log "Install complete. No automatic reboot was performed."
	log "A reboot is recommended to validate boot persistence."
	log "Docs: /usr/share/doc/omen-wmi-boost/BOOT-SETUP.md"
}

do_uninstall() {
	require_root
	log "Removing omen_wmi_boost boot persistence"

	systemctl disable --now omen-wmi-fan-control.service 2>/dev/null || true
	systemctl disable --now omen-wmi-boost-verify.service 2>/dev/null || true
	rm -f /etc/modprobe.d/omen_wmi_boost.conf

	disable_boost
	modprobe -r omen_wmi_boost 2>/dev/null || true

	rm -f /etc/modules-load.d/omen_wmi_boost.conf
	rm -f /etc/omen-wmi-fan-control.conf.example
	rm -f /etc/systemd/system/omen-wmi-boost-verify.service
	rm -f /etc/systemd/system/omen-wmi-fan-control.service
	rm -f /etc/kernel/install.d/zz-omen-wmi-boost.install
	rm -f /usr/local/sbin/omen-wmi-boost-verify
	rm -f /usr/local/sbin/omen-wmi-boost-rebuild
	rm -f /usr/local/sbin/omen-wmi-fan-control
	rm -f /usr/local/sbin/omen-wmi-notify
	rm -f /run/omen_wmi_boost.failed
	rm -f /run/omen_wmi_boost.thermal
	rm -rf "$SRC_INSTALL"
	rm -rf /usr/share/doc/omen-wmi-boost
	rm -rf /var/lib/omen_wmi_boost

	while IFS= read -r module; do
		rm -f "$module"
	done < <(find /lib/modules -type f -name 'omen_wmi_boost.ko*' 2>/dev/null)
	for modules_dir in /lib/modules/*; do
		[[ -d "$modules_dir" ]] || continue
		depmod -a "${modules_dir##*/}" 2>/dev/null || true
	done

	systemctl daemon-reload
	log "Uninstall complete. GPU boost flags were disabled before removal."
	log "The repository checkout and user config /etc/omen-wmi-fan-control.conf were preserved."
}

case "${1:-}" in
	--uninstall|-u) do_uninstall ;;
	--dry-run|-n) do_dry_run ;;
	--force-unsupported) FORCE_UNSUPPORTED=1; do_install ;;
	"") do_install ;;
	*) die "Usage: $0 [--uninstall|-u|--dry-run|-n|--force-unsupported]" ;;
esac
