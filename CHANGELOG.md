# Changelog

## 2.0.0

- Promote the tested personal AI-workload build to the public release.
- Add HP firmware fan discovery, automatic/manual/maximum fan controls, and a
  configurable userspace fan controller.
- Add piecewise-linear fan curves, sustained-workload heat-soak bias,
  rate-limited temperature warnings, and latched critical-temperature boost
  shutdown.
- Add a board `8D87` safety guard with an explicit unsupported-hardware
  override.
- Add the tested `AFNC(0, 0x15e)` GPU power request.
- Restore firmware automatic fan control on normal daemon or module shutdown.
- Repair module recovery when Fedora installs a kernel before matching
  `kernel-devel` is available.
- Add unit tests, static validation, Fedora kernel-module CI, and expanded
  operational documentation.

## 1.x

- Initial public HP WMI GPU performance unlock.
