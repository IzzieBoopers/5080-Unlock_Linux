#!/usr/bin/bash
# One-time install: boot persistence for omen_wmi_boost (max performance).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="${REPO_ROOT}/omen_wmi_boost"
SRC_INSTALL=/usr/src/omen_wmi_boost
KVER="$(uname -r)"

log() { printf '==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

require_root() {
	[[ $EUID -eq 0 ]] || die "Run as root: sudo $0"
}

check_build_deps() {
	[[ -d "/lib/modules/${KVER}/build" ]] || die \
		"kernel headers missing for ${KVER}. Install: dnf install kernel-devel-${KVER}"
	command -v make >/dev/null || die "make not found"
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
	log "Installing module to /lib/modules/${KVER}"
	make -C "$MODULE_DIR" modules_install
	depmod -a "$KVER"
}

install_source_tree() {
	log "Installing source to ${SRC_INSTALL}"
	install -d "$SRC_INSTALL"
	install -m 644 "${MODULE_DIR}/omen_wmi_boost.c" "${MODULE_DIR}/Makefile" "$SRC_INSTALL/"
}

install_configs() {
	log "Installing modprobe and modules-load configs"
	install -d /etc/modprobe.d /etc/modules-load.d
	install -m 644 "${REPO_ROOT}/modprobe.d/omen_wmi_boost.conf" /etc/modprobe.d/
	install -m 644 "${REPO_ROOT}/modules-load.d/omen_wmi_boost.conf" /etc/modules-load.d/
}

install_scripts() {
	log "Installing helper scripts"
	install -d /usr/local/sbin
	install -m 755 "${REPO_ROOT}/scripts/omen-wmi-boost-verify" /usr/local/sbin/
	install -m 755 "${REPO_ROOT}/scripts/omen-wmi-boost-rebuild" /usr/local/sbin/
}

install_systemd() {
	log "Installing and enabling systemd verify unit"
	install -d /etc/systemd/system
	install -m 644 "${REPO_ROOT}/systemd/omen-wmi-boost-verify.service" /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable omen-wmi-boost-verify.service
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
	install -m 644 "${REPO_ROOT}/docs/BOOT-SETUP.md" /usr/share/doc/omen-wmi-boost/
}

smoke_test() {
	log "Smoke test: load module and verify"
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

	if [[ -n "$(find "/lib/modules/${KVER}" -name 'omen_wmi_boost.ko*' -print -quit 2>/dev/null)" ]]; then
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
	show_path_status "/lib/modules/${KVER}/updates/omen_wmi_boost.ko"
	show_path_status /etc/modules-load.d/omen_wmi_boost.conf
	show_path_status /etc/modprobe.d/omen_wmi_boost.conf
	show_path_status /etc/systemd/system/omen-wmi-boost-verify.service
	show_path_status /etc/kernel/install.d/zz-omen-wmi-boost.install
	show_path_status /usr/local/sbin/omen-wmi-boost-verify
	show_path_status /usr/local/sbin/omen-wmi-boost-rebuild

	if [[ -d /sys/module/omen_wmi_boost ]]; then
		log "Module is currently loaded"
	else
		log "Module is not currently loaded"
	fi

	if [[ -r /sys/kernel/omen_wmi_boost/gpu_state ]]; then
		log "Current GPU state: $(< /sys/kernel/omen_wmi_boost/gpu_state)"
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
	rm -f /var/lib/omen_wmi_boost/rebuild-needed 2>/dev/null || true
	mkdir -p /var/lib/omen_wmi_boost

	printf '\n'
	log "Install complete. No automatic reboot was performed."
	log "Reboot is required for boot persistence to take effect."
	log "When ready, run: sudo reboot"
	log "Docs: /usr/share/doc/omen-wmi-boost/BOOT-SETUP.md"
}

do_uninstall() {
	require_root
	log "Removing omen_wmi_boost boot persistence"

	systemctl disable --now omen-wmi-boost-verify.service 2>/dev/null || true
	rm -f /etc/modprobe.d/omen_wmi_boost.conf

	disable_boost
	modprobe -r omen_wmi_boost 2>/dev/null || true

	rm -f /etc/modules-load.d/omen_wmi_boost.conf
	rm -f /etc/systemd/system/omen-wmi-boost-verify.service
	rm -f /etc/kernel/install.d/zz-omen-wmi-boost.install
	rm -f /usr/local/sbin/omen-wmi-boost-verify
	rm -f /usr/local/sbin/omen-wmi-boost-rebuild
	rm -f /run/omen_wmi_boost.failed

	find "/lib/modules/${KVER}" -name 'omen_wmi_boost.ko*' -delete 2>/dev/null || true
	depmod -a "$KVER" 2>/dev/null || true

	systemctl daemon-reload
	log "Uninstall complete. GPU boost flags were disabled before removal."
	log "Source in ${SRC_INSTALL} and repo left in place."
}

case "${1:-}" in
	--uninstall|-u) do_uninstall ;;
	--dry-run|-n) do_dry_run ;;
	"") do_install ;;
	*) die "Usage: $0 [--uninstall|-u|--dry-run|-n]" ;;
esac
