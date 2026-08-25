# Incomplete S5 and the 150 W envelope

This is a hardware-damage hazard, not a convenience warning. Read it before
using the unlock on a laptop you will close, bag, or unplug from AC after
Fedora says it is off.

## What can go wrong

Two independent failures stack.

The unlock writes HP WMI GC22 `CTGP=1` and `DTGP=1`. That is the OEM
high-TGP / Dynamic Boost permission, not an overclock. Under load,
`nvidia-smi` can show a **~150 W** current limit versus the **~80 W** default
(firmware max **175 W**). Linux can still look idle at the last sample (~6 W).
Firmware keeps the **limit**, not the last wattmeter reading.

On a normal `systemctl poweroff`, the kernel module is **not unloaded**.
Before 2.0.2 nothing wrote `CTGP=0 DTGP=0` at shutdown. NVIDIA Dynamic Boost
then tries to set the short-timescale cap to 0 and firmware can refuse with:

```text
nvidia-powerd: ERROR! Error(1f) in setting the short timescale limit (0)
```

That message is the unlock still armed at S5, not proof that the GPU is off.

Separately, ACPI `XHC4` on validated AMD `8D87` is the S5-powered xHCI
(Linux `usb7` / `usb8`). **Any** non-hub USB device on that controller can
keep the USB/EC island alive so PEGP `_OFF` never runs. The chassis can sit
in an incomplete soft-off: power LED still on or cycling, fans in off policy,
dGPU rail still present. The camera and keyboard buses (`XHC1` / `XHC0`) are
not this bug.

If those stack — armed 150 W envelope **and** a USB device holding `XHC4` —
you can bag a machine that Linux already called “off”, then pull AC, and the
still-hot rail is on battery with no useful cooling. That can overheat the
GPU, battery, and chassis. It is not a theoretical corner: treat it as a
condition that can destroy the laptop.

2.0.2 disarms `CTGP`/`DTGP` on poweroff/reboot/halt and can **block**
`systemctl poweroff` while `XHC4` is occupied. That is necessary. It is **not**
sufficient to prove the dGPU rail is off.

## What this software does and does not do

It does:

- Write `boost=0` (`CTGP=0 DTGP=0`) from `omen-wmi-boost-verify` `ExecStop`
  before `nvidia-powerd` stops.
- Repeat that write from a kernel reboot notifier and on `rmmod`.
- Inhibit shutdown/reboot/halt while a non-hub device is on `XHC4`.
- Log an emergency warning and `wall` if you override the inhibit and `XHC4`
  is still occupied.

It does **not**:

- Call PEGP `_OFF` (unsafe while the NVIDIA driver owns the GPU).
- Turn off BIOS “USB power in S5” / always-on USB.
- Cover suspend or hibernate.
- Guarantee the power LED is dark.

## Minimize the risk

Keep these in order. Do not skip the physical checks because the software
path succeeded.

1. Run 2.0.2 or later. Confirm `modinfo omen_wmi_boost | grep version` and
   that `ExecStop=` on `omen-wmi-boost-verify.service` is
   `/usr/local/sbin/omen-wmi-boost-disarm`.
2. Leave the S5-powered USB island empty. That is typically the always-on
   USB-A / USB-C port wired to `XHC4`, not the internal camera or keyboard.
   The default install **refuses** `systemctl poweroff` until that controller
   has no non-hub device.
3. Do not set `usb_s5_inhibit=0` in `/etc/omen-wmi-boost.conf` unless you
   accept incomplete S5. Warn-only mode still logs; it will not stop you from
   bagging a live rail.
4. Do not use `systemctl poweroff -i` / `--ignore-inhibitors` to “make it
   shut down anyway” while a device is in that port.
5. If firmware has **USB Power in Off State**, **Always On USB**, or similar,
   turn it off. Software cannot read that BIOS flag on this platform.
6. After every poweroff, **watch the power LED**. If it is still on after a
   few seconds, hold the power button ~10 s for a hard cut. Do not assume
   Fedora’s “Powered off” screen means S5 completed.
7. Leave the machine **on a hard surface, open enough to vent, until the
   chassis is table-cold**. Then, and only then, put it in a bag or on fabric.
8. Unplug AC only after the LED is off and the machine is cold. Pulling AC
   during incomplete S5 moves a still-hot rail onto battery.
9. Before shutdown you can confirm the software view:

```bash
cat /sys/kernel/omen_wmi_boost/gpu_state
cat /run/omen_wmi_boost.usb-s5
systemctl is-active omen-wmi-usb-s5-inhibit.service
```

Unlocked runtime should show `ctgp=1 dtgp=1`. The USB-S5 state file should
be `state=empty` (or absent after a clean boot with nothing on `XHC4`). The
inhibit unit should be **inactive** when the port is empty and **active**
when occupied.

After the next boot, `gpu_state` should return to `ctgp=1 dtgp=1` via
`auto_boost=1`. Journal should show disarm before `nvidia-powerd` stop:

```bash
journalctl -t omen-wmi-boost -b -1
journalctl -u nvidia-powerd.service -b -1
```

`Error(1f)` at S5 means firmware still refused NVIDIA’s cap-to-0. Disarm is
supposed to happen first; if that line remains after 2.0.2, the envelope may
still have been armed at teardown.

## If poweroff is blocked

Unplug the device on the S5-powered port, then power off normally. Check:

```bash
cat /run/omen_wmi_boost.usb-s5
```

`state=occupied` lists bus-port names only (for example `7-1`). It does not
identify a vendor. Occupied means “something non-hub is on `XHC4`”, including
flash drives and hubs’ downstream devices.

If ACPI `XHC4` cannot be identified (`state=undetectable`), shutdown is **not**
blocked. Disarm still runs. You must use the LED and cold-chassis checks;
the software cannot see that island.

## Overrides (unsafe)

```ini
# /etc/omen-wmi-boost.conf  (copy from /etc/omen-wmi-boost.conf.example)
usb_s5_acpi=XHC4
usb_s5_inhibit=0
```

```bash
systemctl poweroff -i
```

Either override is choosing to ignore a shutdown that this project considers
unsafe. The last-chance log is not a substitute for an empty `XHC4` and a
dark power LED.
