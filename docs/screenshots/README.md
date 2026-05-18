# Screenshots

# HP OMEN RTX 5080 Linux Dynamic Boost Unlock

## Performance Validation

The screenshots below compare the default Linux firmware behavior against the unlocked HP/OMEN Dynamic Boost performance state on an HP OMEN MAX Gaming Laptop 16-ak0xxx with an RTX 5080 Laptop GPU.

The default Linux firmware state limited the GPU to roughly 80W sustained package power. After enabling the HP/OMEN firmware performance state through the WMI control path, the GPU sustained roughly 170–175W during AI inference workloads.

---

# Test Environment

## Hardware

* HP OMEN MAX Gaming Laptop 16-ak0xxx
* Board: `8D87`
* NVIDIA GeForce RTX 5080 Laptop GPU
* GPU Memory: 16 GB

## Software

* Fedora Linux 44
* NVIDIA Driver 595.71.05
* CUDA 13.2
* ComfyUI workload

---

# Benchmark Results

| Metric              | Default Linux State | Unlocked Dynamic Boost State | Delta                 |
| ------------------- | ------------------- | ---------------------------- | --------------------- |
| Sustained GPU Power | ~79–80W             | ~170–173W                    | +115%                 |
| GPU Temperature     | ~57C                | ~85C                         | +28C                  |
| Secondary GPU Temp  | ~53C                | ~71C                         | +18C                  |
| Inference Time      | ~16.5–17.0s         | ~10.9–11.1s                  | ~35–37% faster        |
| Iteration Rate      | ~2.5 it/s           | ~3.8 it/s                    | ~52% higher           |
| GPU Utilization     | ~100%               | ~95–100%                     | effectively unchanged |
| VRAM Usage          | ~7.2 GB             | ~7.2 GB                      | unchanged             |

---

# Observed Behavior

## Default Linux Firmware State

The GPU remained capped at approximately 80W package power.

Observed characteristics:

* Very low thermals for workload intensity
* GPU maintained near-100% utilization despite restricted power budget
* Stable operation
* Significantly reduced tensor throughput
* Sustained AI inference workloads were heavily power-limited

Typical observed metrics:

```text
79W / 80W
57C
~16.5–17.0 second inference
~2.5 iterations/sec
```

---

## Unlocked HP/OMEN Dynamic Boost State

After enabling the HP firmware-controlled performance state through the WMI control path:

* GPU sustained approximately 170–175W package power
* GPU entered full P0 performance state
* AI throughput increased substantially
* GPU utilization remained saturated
* Cooling system remained stable under sustained tensor workloads

Typical observed metrics:

```text
170–173W / 175W
85C
~10.9–11.1 second inference
~3.8 iterations/sec
```

---

# Thermal Notes

The increased performance substantially raises thermal output.

Under sustained AI inference workloads:

* GPU temperatures increased by approximately 28C
* Secondary GPU temperatures increased by approximately 18C
* The cooling system stabilized without visible thermal runaway
* Sustained boost behavior remained stable during long workloads

This workload profile differs significantly from gaming loads because tensor workloads maintain near-continuous saturation rather than burst-style rendering behavior.

Proper cooling is strongly recommended.

Suggested:

* elevated rear airflow
* cooling pad
* aggressive fan curves
* clean intake/exhaust paths

---

# Technical Summary

The unlock does not directly overclock the GPU.

Instead, the module enables the HP/OMEN firmware-controlled Dynamic Boost and TGP performance state normally unavailable under default Linux firmware behavior.

The implementation uses HP WMI ACPI methods exposed through:

```text
WMAA -> WHCM -> GMCF -> GC21 / GC22
```

Relevant firmware variables include:

```text
CTGP
DTGP
DBST
OGHP
```

Target WMI GUID:

```text
5FB7F034-2C63-45E9-BE91-3D44E2C707E4
```

---

# Safety Notes

This utility modifies firmware-controlled GPU power behavior.

Use at your own risk.

Potential impacts include:

* increased thermals
* higher sustained power draw
* increased fan noise
* increased chassis heat
* higher long-term hardware stress

This project was tested specifically on:

```text
HP OMEN MAX 16-ak0xxx
RTX 5080 Laptop GPU
BIOS F.05
Fedora 44
Kernel 7.0.x
```

Behavior on other HP systems is not guaranteed.

---

# Conclusion

The default Linux firmware state left substantial unused GPU thermal and power headroom on this system.

Enabling the HP/OMEN firmware Dynamic Boost path increased sustained GPU package power from approximately 80W to approximately 175W and reduced the measured AI inference workload runtime from roughly 16.5–17 seconds to roughly 11 seconds.

This represents a substantial real-world improvement for AI inference workloads on Linux-based HP OMEN systems.
