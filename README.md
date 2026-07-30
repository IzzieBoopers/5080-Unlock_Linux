# 5080 Unlock — AI Workload Edition

Linux support for the HP OMEN MAX RTX 5080 Laptop GPU performance state, with a
GPU-aware fan controller designed for sustained AI workloads.

The project enables the laptop's OEM firmware performance path; it does not
overclock the GPU. The default AI cooling profile has been exercised under
long-running inference workloads and remains fully configurable.

## Safety and supported hardware

This software changes firmware-controlled GPU power and cooling behavior.
Higher performance means substantially more heat, power draw, fan noise, and
hardware stress. Monitor temperatures and keep intake and exhaust paths clear.

Validated platform:

- HP OMEN MAX Gaming Laptop 16-ak0xxx
- System board `8D87`
- NVIDIA GeForce RTX 5080 Laptop GPU
- BIOS `F.07`
- Fedora Linux 44
- Linux `7.1.5-201.fc44.x86_64`

The kernel module refuses to load on other boards by default. Advanced users
can set `force_unsupported=1`, but the fan commands and 175 W power request may
be unsafe on untested hardware.

The installer exposes that override explicitly:

```bash
sudo ./install.sh --force-unsupported
```

Do not use it merely to bypass an installation error.

## Features

- Enables HP WMI `CTGP` and `DTGP`/`ppab` performance flags.
- Queues the tested firmware GPU power request for the 175 W limit.
- Exposes automatic, manual, and maximum fan control through sysfs.
- Uses a configurable piecewise-linear temperature/fan curve.
- Adds workload-duration heat-soak bias for sustained AI workloads.
- Returns fan control to firmware when the GPU becomes idle.
- Warns at high temperature and disables the unlock at a configurable critical
  threshold.
- Rebuilds the out-of-tree module after Fedora kernel updates and repairs a
  missing module at the next verification boot.

## Requirements

Install Fedora build dependencies for the running kernel:

```bash
sudo dnf install gcc make python3 kernel-devel-$(uname -r)
```

The NVIDIA driver and `nvidia-smi` must work. Secure Boot must be disabled or
configured to trust a signed copy of this module.

## Install

Inspect the intended changes:

```bash
./install.sh --dry-run
```

Build, install, and start the services:

```bash
sudo ./install.sh
```

A reboot is recommended to validate boot persistence:

```bash
sudo reboot
```

Verify:

```bash
cat /sys/kernel/omen_wmi_boost/gpu_state
cat /sys/kernel/omen_wmi_boost/fan_state
systemctl status omen-wmi-boost-verify.service
systemctl status omen-wmi-fan-control.service
```

Expected GPU state includes `ctgp=1 ppab=1`.

## Default AI cooling policy

The controller polls every four seconds and enters manual cooling when GPU
utilization exceeds 5%, graphics clock reaches 500 MHz, or temperature reaches
45 C. The shipped curve is:

```ini
fan_curve=32:5,45:50,70:50,76:70,82:100
target_temp_c=70
```

Each pair is `temperature-C:fan-percent`. Values between points are linearly
interpolated. The active fan floor is 50%. Long AI workloads accumulate a
gradual 5% heat-soak bias every two minutes while at or above the 70 C target,
up to 25%.

When the workload ends, short jobs receive a proportional cooldown hold. Once
the GPU is cool and idle, control returns to firmware `auto`.

## Configure fan and thermal policy

Edit:

```text
/etc/omen-wmi-fan-control.conf
```

Then validate without changing hardware and restart:

```bash
sudo /usr/local/sbin/omen-wmi-fan-control --dry-run --once
sudo systemctl restart omen-wmi-fan-control.service
```

Common curve shapes:

```ini
# Linear
fan_curve=35:20,50:40,65:60,75:80,82:100

# Balanced
fan_curve=32:10,45:40,68:55,76:75,82:100

# Sharp upper-end climb
fan_curve=32:10,65:45,74:55,78:80,82:100
```

The AI profile remains the default. `active_fan_floor` can override low curve
points while a workload is active.

Upgrade installs preserve the active config and place current defaults at:

```text
/etc/omen-wmi-fan-control.conf.example
```

## Thermal protection

Default safety settings:

```ini
warning_temp_c=80
warning_fan_percent=90
unlock_disable_temp_c=85
critical_samples=2
warning_repeat_s=300
```

At the warning threshold the controller raises cooling and sends a rate-limited
journal and desktop warning. Check for dirty fans, blocked vents, poor airflow,
or a workload that exceeds the cooling system. Elevating the rear or using a
capable cooling pad may help.

Two consecutive critical readings cause the controller to:

1. Request maximum fan cooling.
2. Disable the GPU unlock.
3. Write `/run/omen_wmi_boost.thermal`.
4. Keep thermal protection latched and notify logged-in users.

Stop the workload and let the system cool. Inspect and clean the fans and vents
before restoring normal operation. Once temperatures are safe:

```bash
sudo systemctl restart omen-wmi-fan-control.service
echo 1 | sudo tee /sys/kernel/omen_wmi_boost/performance
```

Critical shutdown always overrides custom fan preferences. If telemetry or fan
writes fail near a thermal limit, the controller disables boost and falls back
to firmware automatic fan control.

## Troubleshooting

```bash
journalctl -t omen-wmi-boost -b
journalctl -u omen-wmi-fan-control.service -b
cat /sys/kernel/omen_wmi_boost/last_error
cat /sys/kernel/omen_wmi_boost/fan_state
nvidia-smi
```

After a kernel update, install matching `kernel-devel` if the module could not
be rebuilt, then restart verification:

```bash
sudo systemctl restart omen-wmi-boost-verify.service
```

## Uninstall

The uninstaller disables boost, removes installed modules and services, and
preserves the user-edited fan config:

```bash
sudo ./install.sh --uninstall
```

## Development and testing

Non-destructive checks:

```bash
./run-tests.sh
```

Explicit privileged hardware integration test:

```bash
sudo ./run-tests.sh --hardware
```

See `docs/BOOT-SETUP.md`, `docs/FAN-CONTROL.md`,
`docs/FIRMWARE-CONTROLS.md`, and `docs/REVERSE-ENGINEERING.md` for operational
and firmware details.

## License

The kernel module is GPL-2.0-only. Userspace scripts, configuration,
documentation, and other project material are MIT licensed. See `LICENSE` and
`LICENSES/GPL-2.0-only.txt`.
