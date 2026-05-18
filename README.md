# 5080 Unlock Personal Build

Personal HP OMEN RTX 5080 Laptop GPU unlock for Linux. This branch keeps the
original WMI performance unlock and adds direct firmware fan controls plus a
GPU-aware fan controller for AI workloads.

This changes firmware-controlled power and cooling behavior. Use it only on the
target machine and keep an eye on thermals.

## What It Does

- Loads `omen_wmi_boost`, an out-of-tree kernel module that talks to HP WMI.
- Enables the OEM GPU performance state by setting the firmware GPU boost flags.
- Exposes fan sysfs controls under `/sys/kernel/omen_wmi_boost/`.
- Runs `omen-wmi-fan-control`, a userspace daemon that cools aggressively when
  the Nvidia GPU is active and returns to firmware `auto` when idle.

## Install

```bash
./install.sh --dry-run
sudo ./install.sh
sudo reboot
```

Requirements: `gcc`, `make`, `python3`, matching `kernel-devel`, working
Nvidia driver/`nvidia-smi`, and Secure Boot disabled or configured for this
module.

## Fan Policy

The fan daemon polls every 4 seconds. It switches to manual cooling when GPU
utilization is above 5%, graphics clock is at least 500 MHz, or GPU temperature
reaches 45C.

Active fan curve:

```text
<= 32C: 5%
>= 80C: 100%
between: 5 + ((temp - 32) * 95) / 48
active floor: 40%
```

Fan changes are rate-limited to +20 percentage points per tick and -5
percentage points per tick. The daemon returns to firmware `auto` after 180
seconds with utilization at or below 5%, graphics clock below 500 MHz, and GPU
temperature at or below 35C. GPU process count is logged, but does not block
idle handoff.

If telemetry or fan writes fail, the daemon falls back to `fan_mode=auto`.

## Useful Checks

```bash
cat /sys/kernel/omen_wmi_boost/gpu_state
cat /sys/kernel/omen_wmi_boost/fan_state
systemctl status omen-wmi-boost-verify.service
systemctl status omen-wmi-fan-control.service
journalctl -u omen-wmi-fan-control.service -b
/usr/local/sbin/omen-wmi-fan-control --dry-run --once
```

Expected GPU state includes:

```text
ctgp=1 ppab=1
```

## Manual Controls

```bash
echo 1 | sudo tee /sys/kernel/omen_wmi_boost/performance
echo auto | sudo tee /sys/kernel/omen_wmi_boost/fan_mode
echo manual | sudo tee /sys/kernel/omen_wmi_boost/fan_mode
echo max | sudo tee /sys/kernel/omen_wmi_boost/fan_mode
```

## Uninstall

```bash
sudo ./install.sh --uninstall
```

## Development

```bash
make -C omen_wmi_boost
make -C omen_wmi_boost clean
python3 -m py_compile scripts/omen-wmi-fan-control
```

See `docs/REVERSE-ENGINEERING.md` for the WMI command notes.
