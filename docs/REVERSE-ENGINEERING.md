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

That path reaches HP's firmware command dispatcher, where `GC21` reads GPU power state and `GC22` writes the GPU thermal/power flags.

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

The project does not overclock the GPU. It enables the OEM firmware performance state that HP's Windows tooling/platform profile path normally controls.
