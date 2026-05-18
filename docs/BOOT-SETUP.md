# Boot Setup

This personal build installs the HP WMI GPU performance unlock and starts a
GPU-aware fan controller at boot.

## Install

```bash
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

## Fan Controller

`omen-wmi-fan-control` leaves firmware in `auto` while the Nvidia GPU is idle.
It switches to manual fan control when utilization is above 5%, graphics clock
is at least 500 MHz, or GPU temperature reaches 45C.

```text
<= 32C: 5%
>= 80C: 100%
between: 5 + ((temp - 32) * 95) / 48
active floor: 40%
poll interval: 4 seconds
```

It returns to firmware `auto` after 180 seconds with utilization at or below 5%,
graphics clock below 500 MHz, and temperature at or below 35C. Attached GPU
processes are logged but do not block idle handoff.

If telemetry or fan writes fail, the controller falls back to `fan_mode=auto`.

## Troubleshooting

```bash
journalctl -t omen-wmi-boost -b
journalctl -u omen-wmi-fan-control.service -b
cat /sys/kernel/omen_wmi_boost/last_error
cat /sys/kernel/omen_wmi_boost/fan_state
nvidia-smi
```

Dry-run one fan-control tick:

```bash
/usr/local/sbin/omen-wmi-fan-control --dry-run --once
```

## Uninstall

```bash
sudo ./install.sh --uninstall
```
