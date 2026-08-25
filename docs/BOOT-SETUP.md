# Boot Setup

The installer builds `omen_wmi_boost` for the running Fedora kernel, enables
the tested HP GPU performance state, and starts the AI-aware fan controller.

## Install

```bash
sudo dnf install gcc make python3 kernel-devel-$(uname -r)
./install.sh --dry-run
sudo ./install.sh
sudo reboot
```

After reboot:

```bash
cat /sys/kernel/omen_wmi_boost/gpu_state
cat /sys/kernel/omen_wmi_boost/fan_state
systemctl status omen-wmi-boost-verify.service
systemctl status omen-wmi-fan-control.service
systemctl status omen-wmi-usb-s5-inhibit.service
```

`gpu_state` should include `ctgp=1 dtgp=1`.

## Installed components

- `/lib/modules/<kernel>/extra/omen_wmi_boost.ko`
- `/usr/src/omen_wmi_boost/`
- `/etc/modprobe.d/omen_wmi_boost.conf`
- `/etc/modules-load.d/omen_wmi_boost.conf`
- `/etc/omen-wmi-fan-control.conf`
- `/etc/omen-wmi-fan-control.conf.example`
- `/etc/omen-wmi-boost.conf.example`
- `/usr/local/sbin/omen-wmi-{boost-rebuild,boost-verify,boost-disarm,usb-s5-guard,fan-control,notify}`
- `/usr/local/lib/omen-wmi-boost/omen_wmi_usb_s5.py`
- `/etc/systemd/system/omen-wmi-{boost-verify,fan-control,usb-s5-guard,usb-s5-inhibit}.service`
- `/etc/udev/rules.d/99-omen-wmi-usb-s5.rules`
- `/etc/kernel/install.d/zz-omen-wmi-boost.install`
- `/usr/share/doc/omen-wmi-boost/` (includes `S5-SAFETY.md`)

Use `modinfo -n omen_wmi_boost` to see the actual module path.

## Boot ordering and kernel updates

`omen-wmi-boost-verify.service` starts after module loading and NVIDIA power
setup. It waits for the firmware flags to settle and re-applies performance if
needed. The fan controller starts only after verification succeeds.

## Shutdown and USB-S5

**Hazard.** Incomplete S5 plus an armed TGP unlock can leave a ~150 W GPU
envelope on a live rail after Linux says the machine is off. Fans follow the
off policy. Bagging that, then pulling AC, is how this becomes a chassis
furnace. See `docs/S5-SAFETY.md`.

A normal `systemctl poweroff` does not unload `omen_wmi_boost`. Verify
`ExecStop` writes `boost=0` (`CTGP=0 DTGP=0`) before `nvidia-powerd` stops. A
kernel reboot notifier repeats that write if userspace teardown is skipped.

On validated AMD `8D87`, ACPI `XHC4` is the S5-powered xHCI. Any non-hub USB
device there can keep the discrete GPU rail alive after Linux claims off.
While that controller is occupied, `omen-wmi-usb-s5-inhibit.service` blocks
poweroff, reboot, and halt. Unplug the port, or override with:

```bash
systemctl poweroff -i
```

`-i` still runs TGP disarm and logs an emergency warning if `XHC4` is occupied.
Clearing the unlock flags does not mean the dGPU rail is off.

After every poweroff:

1. Confirm the power LED actually goes dark. If it does not, hold power ~10 s.
2. Leave the laptop on a hard surface until it is table-cold.
3. Do not bag it or unplug AC until then.

`usb_s5_inhibit=0` in `/etc/omen-wmi-boost.conf` disables the shutdown block
but keeps detection and the last-chance warning. If ACPI `XHC4` cannot be
identified, shutdown is not blocked.

The Fedora kernel-install hook rebuilds the module for new kernels. If
`kernel-devel` was not available during the RPM transaction, verification
retries the deferred build on the next boot.

Recovery:

```bash
sudo dnf install kernel-devel-$(uname -r)
sudo systemctl restart omen-wmi-boost-verify.service
sudo systemctl restart omen-wmi-fan-control.service
```

## Fan controller

Firmware remains in `auto` while the GPU is idle. Activity switches to the
configured manual curve; workload duration can add heat-soak bias after the
GPU has reached `target_temp_c`. That extra cooling is kept through
temperature dips until the soak score decays. Edit:

```text
/etc/omen-wmi-fan-control.conf
```

The default curve is:

```ini
fan_curve=32:5,45:50,70:50,76:70,82:100
target_temp_c=70
```

Curve points are `temperature-C:fan-percent` and are linearly interpolated.
The complete current defaults are always installed as
`/etc/omen-wmi-fan-control.conf.example`; upgrades preserve the active config.

Validate a configuration without hardware writes:

```bash
/usr/local/sbin/omen-wmi-fan-control --dry-run --once
/usr/local/sbin/omen-wmi-fan-control \
  --config ./config/omen-wmi-fan-control.conf --dry-run --once
```

## Thermal shutdown

At `warning_temp_c`, the service logs and sends a rate-limited desktop warning.
After `critical_samples` consecutive readings at or above
`unlock_disable_temp_c`, it requests maximum fans, disables boost, and writes:

```text
/run/omen_wmi_boost.thermal
```

Stop the workload and inspect airflow, vents, and fans. Once the machine is
cool and the cooling problem has been addressed:

```bash
sudo systemctl restart omen-wmi-fan-control.service
echo 1 | sudo tee /sys/kernel/omen_wmi_boost/performance
```

## Troubleshooting

```bash
journalctl -t omen-wmi-boost -b
journalctl -u omen-wmi-boost-verify.service -b
journalctl -u omen-wmi-fan-control.service -b
journalctl -u omen-wmi-usb-s5-guard.service -b
cat /sys/kernel/omen_wmi_boost/last_error
cat /sys/kernel/omen_wmi_boost/fan_state
cat /run/omen_wmi_boost.usb-s5
nvidia-smi
```

## Uninstall

```bash
sudo ./install.sh --uninstall
```

Installed binaries, source, services, documentation, and modules are removed.
The repository checkout and user-edited `/etc/omen-wmi-fan-control.conf` and
`/etc/omen-wmi-boost.conf` remain.
