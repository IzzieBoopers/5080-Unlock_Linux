# Boot persistence setup (omen_wmi_boost)

One-time install loads **max performance** at every boot. If anything fails (kernel update, Secure Boot, missing build), you get a **KDE critical notification** and journal errors.

## Install (once)

```bash
cd ~/5080_Unlock
sudo ./install.sh
sudo reboot
```

Requirements: `kernel-devel-$(uname -r)`, `gcc`, `make`.

After reboot, check:

```bash
cat /sys/kernel/omen_wmi_boost/gpu_state
# expect: ctgp=1 ppab=1 ...
systemctl status omen-wmi-boost-verify.service
```

## Uninstall

```bash
sudo ./install.sh --uninstall
```

## What gets installed

| Path | Purpose |
|------|---------|
| `/lib/modules/<kernel>/extra/omen_wmi_boost.ko` | Kernel module |
| `/usr/src/omen_wmi_boost/` | Source for rebuilds |
| `/etc/modules-load.d/omen_wmi_boost.conf` | Load at boot |
| `/etc/modprobe.d/omen_wmi_boost.conf` | `persist=1 auto_boost=1` |
| `/usr/local/sbin/omen-wmi-boost-verify` | Post-boot check + notify |
| `/etc/systemd/system/omen-wmi-boost-verify.service` | Runs verify every boot |
| `/etc/kernel/install.d/zz-omen-wmi-boost.install` | Rebuild on kernel update |

## After a kernel update

Fedora installs a new kernel; the hook tries to rebuild automatically.

If boost still fails:

```bash
cd ~/5080_Unlock
sudo ./install.sh
sudo reboot
```

Check for a stale rebuild flag:

```bash
cat /var/lib/omen_wmi_boost/rebuild-needed 2>/dev/null
```

## Troubleshooting

### KDE notification: "GPU power boost failed"

1. Journal: `journalctl -t omen-wmi-boost -b`
2. Module: `lsmod | grep omen_wmi_boost`
3. Sysfs: `cat /sys/kernel/omen_wmi_boost/last_error`
4. dmesg: `sudo dmesg | grep omen_wmi_boost`
5. Verify unit: `systemctl status omen-wmi-boost-verify.service`

### Secure Boot

If `mokutil --sb-state` shows **enabled**, unsigned modules will not load. Either:

- Disable Secure Boot in firmware, or
- Sign the module and enroll your MOK key

### Manual re-apply (without reboot)

```bash
sudo modprobe -r omen_wmi_boost
sudo modprobe omen_wmi_boost
# or
echo 1 | sudo tee /sys/kernel/omen_wmi_boost/performance
```

### Power still 80W

Confirm AC power, check `journalctl -u nvidia-powerd -b`, and review the main `README.md` troubleshooting section.

## Files on failure

- `/run/omen_wmi_boost.failed` — timestamp and reason from last verify run
