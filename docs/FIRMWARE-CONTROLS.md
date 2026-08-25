# Firmware Controls

This build talks to HP firmware through a small kernel module,
`omen_wmi_boost`. Userspace scripts should not call ACPI/WMI directly; they use
the sysfs files under `/sys/kernel/omen_wmi_boost/`.

The module is validated on AMD system board `8D87` (OMEN MAX 16-ak0xxx) and
refuses other boards unless the operator explicitly sets
`force_unsupported=1`. That override bypasses a safety check; it does not make
the commands portable to other firmware. Intel `8D87` configurations remain
unvalidated.

## Firmware Paths

HP WMI method interface:

```text
GUID: 5FB7F034-2C63-45E9-BE91-3D44E2C707E4
ACPI: \_SB.WMID.WMAA
command: 0x00020008
signature: 0x55434553 ("SECU")
```

GPU power-limit request interface (experimental, off by default):

```text
gpu_power_request=1 gpu_power_path=<ACPI method>
```

On AMD `8D87`, `\_SB.PCI0.GPPA.VGA.AFNC` is the iGPU ATIF method, not a NVIDIA
TGP control. The module does not call it unless `gpu_power_path` is set, and it
refuses a `VGA.AFNC` path unless `gpu_power_allow_igpu=1`. The 175 W unlock
does not depend on AFNC.

## GPU Unlock

`performance` runs the high-performance setup:

```bash
echo 1 | sudo tee /sys/kernel/omen_wmi_boost/performance
```

Internally this:

- Calls HP WMI `0x10` (EC user-define trigger).
- Calls HP WMI command `0x1a` with `thermal_profile`.
- Calls HP WMI `GC22` (`0x22`) to set `CTGP=1` and `DTGP=1`.
- Optionally evaluates an explicit `gpu_power_path` if `gpu_power_request=1`.
- Preserves the firmware-reported `dstate`. GC22's fourth byte is unused padding.

Useful readback:

```bash
cat /sys/kernel/omen_wmi_boost/gpu_state
```

Expected unlocked state:

```text
ctgp=1 dtgp=1
```

`gpu_boost_set` retries the GC22 write up to five times on both enable and
disable. It succeeds only when `CTGP` and `DTGP` match the requested state,
and returns `-EIO` if they do not stick. Thermal `boost=0` therefore does not
report success while the flags remain set.

On `poweroff`, `reboot`, and `halt`, userspace `omen-wmi-boost-disarm` writes
`boost=0` before `nvidia-powerd` stops. The module also registers a reboot
notifier and disarms on `rmmod`, because a normal shutdown does not unload the
module. That clears the 150 W envelope; it does not power-off PEGP if a USB
device is holding the S5-powered `XHC4` island. Incomplete S5 with that
envelope still armed is a bag-and-unplug thermal hazard. Mitigation steps
are in `docs/S5-SAFETY.md`.

Accepted `thermal_profile` values are raw firmware bytes. This build defaults
to `0x01`; `0x31` is also known from HP OMEN paths.

Power request module parameters:

```text
force_unsupported=0
gpu_power_request=0
```

## Fan Control

Fan controls use HP WMI fan commands, mainly:

```text
0x10 EC user-define trigger (also reports fan count)
0x11 legacy RPM read
0x26 max fan read
0x27 max fan set
0x2d Victus/OMEN fan RPM read
0x2e Victus/OMEN fan speed set
0x2f fan table read
```

Primary controls:

```bash
cat /sys/kernel/omen_wmi_boost/fan_state
cat /sys/kernel/omen_wmi_boost/fan_mode
cat /sys/kernel/omen_wmi_boost/fan_speed
```

Accepted `fan_mode` values:

```text
auto   firmware automatic fan control
manual userspace writes fan_speed
max    firmware max-fan mode
```

Accepted `fan_speed` values are `0..255`, but writes only work while
`fan_mode=manual`. The driver clamps to the discovered firmware fan table
range. On the validated AMD `8D87` board that range has usually been around
`19..60`, visible in `fan_state` as `speed_min` and `speed_max`.

For automatic handoff:

```bash
echo auto | sudo tee /sys/kernel/omen_wmi_boost/fan_mode
```

For manual control:

```bash
echo manual | sudo tee /sys/kernel/omen_wmi_boost/fan_mode
echo 40 | sudo tee /sys/kernel/omen_wmi_boost/fan_speed
```

The userspace fan daemon writes `manual` and `fan_speed` while the GPU is active,
then returns to `auto` when idle or on failure.

The kernel module also restores firmware `auto` when unloaded after manual or
maximum fan control.

## Userspace Thermal Policy

The daemon reads `/etc/omen-wmi-fan-control.conf`. Fan curves are ordered
`temperature-C:fan-percent` points:

```ini
fan_curve=32:5,45:50,70:50,76:70,82:100
target_temp_c=70
```

Intermediate values are linearly interpolated. Workload heat-soak bias and
rate limits are applied after interpolation. Soak bias is earned at or above
the configured target and is kept through temperature dips until the soak
score decays. Busy workloads do not lower fan speed because temperature fell.

Thermal protection is independent of the curve:

```ini
warning_temp_c=80
warning_fan_percent=90
unlock_disable_temp_c=85
critical_samples=2
```

At the critical threshold the daemon requests `fan_mode=max` before writing
`boost=0`. The disabled state is latched, and diagnostic details are stored in
`/run/omen_wmi_boost.thermal`.
