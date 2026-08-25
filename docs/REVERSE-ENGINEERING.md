# Reverse Engineering Notes

This is the short version of the firmware path used by `omen_wmi_boost`.

## Firmware Path

```text
WMAA -> WHCM -> GMCF -> GC21/GC22
```

The module calls the HP WMI BIOS GUID:

```text
5FB7F034-2C63-45E9-BE91-3D44E2C707E4
```

That path reaches HP's firmware command dispatcher, where `GC21` reads GPU
power state and `GC22` writes the GPU thermal/power flags.

## Important Variables

- `CTGP`: configurable GPU TGP permission flag.
- `DTGP`: Dynamic Boost GPU permission flag (GC22 byte 1; upstream hp-wmi called this `ppab`).
- `DBST`: firmware Dynamic Boost state tied to `DTGP`.
- `OGHP`: OMEN high-performance policy gate. If this is false, firmware may keep Dynamic Boost disabled even when other flags appear writable.

## Commands Used

- `0x21`: read GPU thermal modes.
- `0x22`: write GPU thermal modes.
- `0x1a`: set HP/OMEN performance thermal profile.
- `0x10`: EC user-define trigger; required before GC22 flag writes. Skipping it
  can produce no TGP change. Upstream hp-wmi names this
  `hp_wmi_get_fan_count_userdefine_trigger`.
- `0x26`/`0x27`: read and set firmware maximum-fan mode.
- `0x2d`/`0x2e`: read RPM data and set Victus/OMEN fan speeds.
- `0x2f`: read the firmware fan table.

The 80 W to 175 W unlock on AMD `8D87` is `0x10` + `0x1a` + GC22 (`CTGP`/`DTGP`).
It does not use ACPI AFNC.

`\_SB.PCI0.GPPA.VGA.AFNC` exists on this board as the AMD iGPU ATIF method under
`GPPA.VGA` (Radeon 890M). It is a no-op unless firmware flag `M101 & 0x1000` is
set, and it is not a NVIDIA power-limit API. `GPP9.PEGP` AFNC only exists as an
unresolved `External(...)`. The module therefore skips AFNC unless
`gpu_power_request=1` and `gpu_power_path` is set to a non-iGPU method.

GC22 is a 4-byte payload: `CTGP`, `DTGP`, `dstate`, and an unused fourth byte
that firmware does not read or write.

The project does not overclock the GPU. It enables the OEM firmware performance
state that HP's Windows tooling/platform profile path normally controls.

That envelope must be disarmed at S5. A USB device on ACPI `XHC4` can still
leave the dGPU rail up. See `docs/S5-SAFETY.md`.

## Validation Boundary

The command set is validated on HP board `8D87`, OMEN MAX Gaming Laptop
16-ak0xxx (AMD Ryzen AI + RTX 5080), through BIOS `F.07`. The same board ID
also ships in Intel configurations; those remain unvalidated. The common HP WMI
GUID appears on many unrelated systems, so GUID presence alone is not evidence
that these command payloads are safe. The module therefore blocks other boards
unless the operator explicitly uses `force_unsupported=1`.
