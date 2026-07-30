# Fan and Thermal Configuration

The fan daemon reads `/etc/omen-wmi-fan-control.conf` once at startup. Restart
the service after editing:

```bash
sudo systemctl restart omen-wmi-fan-control.service
```

Before restarting, validate parsing and view intended writes without changing
hardware:

```bash
sudo /usr/local/sbin/omen-wmi-fan-control --dry-run --once
```

## Fan curve

`fan_curve` is a comma-separated list of `temperature-C:fan-percent` points:

```ini
fan_curve=32:5,45:50,70:50,76:70,82:100
```

Temperatures must be strictly increasing. Fan percentages must be `0..100`.
The daemon linearly interpolates between points and clamps outside the first
and last points.

`target_temp_c` controls when sustained-workload heat-soak bias is allowed:

```ini
target_temp_c=70
```

The target must fall within the curve temperature range.

Example alternatives:

```ini
# Linear
fan_curve=35:20,50:40,65:60,75:80,82:100

# Quiet/balanced
fan_curve=32:10,45:40,68:55,76:75,82:100

# Sharp upper-end climb
fan_curve=32:10,65:45,74:55,78:80,82:100
```

## Activity and idle handoff

Any active threshold enters manual cooling:

```ini
active_temp_c=45
active_util_percent=5
active_clock_mhz=500
```

While active, `active_fan_floor` is the minimum applied curve percentage:

```ini
active_fan_floor=50
```

Firmware automatic control resumes only when utilization and clock are below
the active thresholds and temperature is at or below:

```ini
idle_temp_c=35
```

Short work receives a proportional cooldown hold:

```ini
active_hold_multiplier=3
```

The command-line `--idle-hold` value caps that hold at 180 seconds by default.

## Sustained AI workload bias

A sample counts as sustained workload when any threshold is reached:

```ini
workload_util_percent=30
workload_power_w=45
workload_clock_mhz=1000
```

At or above `target_temp_c`, sustained workload adds fan bias:

```ini
soak_after_s=120
soak_step_s=120
soak_step_percent=5
soak_max_bias_percent=25
soak_gap_hold_s=90
soak_decay_s=600
```

This waits 120 seconds, then adds 5 percentage points every 120 seconds up to
25 points. Brief model-loading gaps retain the score for 90 seconds; true idle
time decays it over 600 seconds.

## Thermal safety

```ini
warning_temp_c=80
warning_fan_percent=90
unlock_disable_temp_c=85
critical_samples=2
warning_repeat_s=300
```

`warning_temp_c` raises cooling to at least `warning_fan_percent` and emits a
warning no more often than `warning_repeat_s`.

`critical_samples` consecutive readings at or above
`unlock_disable_temp_c` request maximum cooling, disable GPU boost, and latch
thermal protection. A sustained unsafe plateau trips protection even if the
temperature is no longer rising.

Safety thresholds override fan-curve preferences. If fan control fails near a
thermal limit, the daemon disables boost and attempts firmware automatic fan
control.

## Rate limiting

```ini
ramp_up_step=20
ramp_down_step=5
```

These values are maximum percentage-point changes per control tick. They smooth
normal curve changes; critical protection bypasses the curve and requests
firmware maximum cooling immediately.

## Upgrade behavior

The installer preserves `/etc/omen-wmi-fan-control.conf`. Current defaults are
always refreshed at:

```text
/etc/omen-wmi-fan-control.conf.example
```

Legacy scalar curve keys and `failsafe_temp_c` remain accepted for existing
personal-build configurations. New configurations should use `fan_curve`,
`warning_temp_c`, and `unlock_disable_temp_c`.
