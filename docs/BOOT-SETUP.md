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
```

`gpu_state` should include `ctgp=1 ppab=1`.

## Installed components

- `/lib/modules/<kernel>/extra/omen_wmi_boost.ko`
- `/usr/src/omen_wmi_boost/`
- `/etc/modprobe.d/omen_wmi_boost.conf`
- `/etc/modules-load.d/omen_wmi_boost.conf`
- `/etc/omen-wmi-fan-control.conf`
- `/etc/omen-wmi-fan-control.conf.example`
- `/usr/local/sbin/omen-wmi-{boost-rebuild,boost-verify,fan-control,notify}`
- `/etc/systemd/system/omen-wmi-{boost-verify,fan-control}.service`
- `/etc/kernel/install.d/zz-omen-wmi-boost.install`
- `/usr/share/doc/omen-wmi-boost/`

Use `modinfo -n omen_wmi_boost` to see the actual module path.

## Boot ordering and kernel updates

`omen-wmi-boost-verify.service` starts after module loading and NVIDIA power
setup. It waits for the firmware flags to settle and re-applies performance if
needed. The fan controller starts only after verification succeeds.

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
configured manual curve; workload duration can add heat-soak bias. Edit:

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
cat /sys/kernel/omen_wmi_boost/last_error
cat /sys/kernel/omen_wmi_boost/fan_state
nvidia-smi
```

## Uninstall

```bash
sudo ./install.sh --uninstall
```

Installed binaries, source, services, documentation, and modules are removed.
The repository checkout and user-edited `/etc/omen-wmi-fan-control.conf` remain.
