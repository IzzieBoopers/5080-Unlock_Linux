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
- `DTGP`: Dynamic Boost GPU permission flag, exposed as `ppab` by the module.
- `DBST`: firmware Dynamic Boost state tied to `DTGP`.
- `OGHP`: OMEN high-performance policy gate. If this is false, firmware may keep Dynamic Boost disabled even when other flags appear writable.

## Commands Used

- `0x21`: read GPU thermal modes.
- `0x22`: write GPU thermal modes.
- `0x1a`: set HP/OMEN performance thermal profile.
- `0x10`: fan/EC trigger used before applying the performance path.
- `0x26`/`0x27`: read and set firmware maximum-fan mode.
- `0x2d`/`0x2e`: read RPM data and set Victus/OMEN fan speeds.
- `0x2f`: read the firmware fan table.

The performance path also evaluates:

```text
\_SB.PCI0.GPPA.VGA.AFNC(0, 0x15e)
```

This matches the tested firmware request associated with the 175 W GPU limit.
It is board-specific and is not a generic NVIDIA power-limit API.

The project does not overclock the GPU. It enables the OEM firmware performance
state that HP's Windows tooling/platform profile path normally controls.

## Validation Boundary

The command set is validated on HP board `8D87`, OMEN MAX Gaming Laptop
16-ak0xxx, through BIOS `F.07`. The common HP WMI GUID appears on many unrelated
systems, so GUID presence alone is not evidence that these command payloads are
safe. The module therefore blocks other boards unless the operator explicitly
uses `force_unsupported=1`.
