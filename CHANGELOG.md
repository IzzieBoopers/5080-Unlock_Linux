# Changelog

## 2.0.1

- Stop calling `GPPA.VGA.AFNC` by default. On AMD `8D87` that ACPI method is
  the iGPU ATIF path, not the NVIDIA 175 W limit. The unlock is WMI `0x10` +
  `0x1a` + GC22 `CTGP`/`DTGP`.
- Run the EC user-define trigger (`0x10`) before GC22 flag writes, including
  the sysfs `boost` path.
- Expose firmware `DTGP` as `dtgp` in `gpu_state` and drop unused
  `slowdown_temp` from that sysfs line.
- Retry GC22 `CTGP`/`DTGP` writes on enable and disable; return `-EIO` if the
  flags do not stick after retries.
- Keep heat-soak bias once earned at `target_temp_c`, skip downshifts while
  the GPU is busy, and slow fan ramp-down so cooling does not crater on a
  single temperature dip.
- Leave Intel `8D87` configurations off the DMI allowlist.

## 2.0.0

- Promote the tested AI-workload build to the public release.
- Add HP firmware fan discovery, automatic/manual/maximum fan controls, and a
  configurable userspace fan controller.
- Add piecewise-linear fan curves, sustained-workload heat-soak bias,
  rate-limited temperature warnings, and latched critical-temperature boost
  shutdown.
- Add a board `8D87` safety guard with an explicit unsupported-hardware
  override.
- Add an experimental ACPI AFNC GPU power request (disabled by default in
  2.0.1 after it was identified as the AMD iGPU ATIF path).
- Restore firmware automatic fan control on normal daemon or module shutdown.
- Repair module recovery when Fedora installs a kernel before matching
  `kernel-devel` is available.
- Add unit tests, static validation, Fedora kernel-module CI, and expanded
  operational documentation.

## 1.x

- Initial public HP WMI GPU performance unlock.
