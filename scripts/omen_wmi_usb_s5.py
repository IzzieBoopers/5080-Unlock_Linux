# SPDX-License-Identifier: MIT
"""Detect non-hub USB devices on the ACPI S5-powered xHCI (XHC4)."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

DEFAULT_ACPI_NAME = "XHC4"
DEFAULT_INHIBIT = True
DEFAULT_CONFIG = Path("/etc/omen-wmi-boost.conf")
DEFAULT_WAKEUP = Path("/proc/acpi/wakeup")
DEFAULT_PCI_ROOT = Path("/sys/bus/pci/devices")
USB_HUB_CLASS = 0x09
STATE_FILE = Path("/run/omen_wmi_boost.usb-s5")
UNDETECTABLE_FLAG = Path("/run/omen_wmi_boost.usb-s5.undetectable")
INHIBIT_UNIT = "omen-wmi-usb-s5-inhibit.service"
NOTIFY_TITLE = "Unsafe USB-S5 poweroff risk"
NOTIFY_BODY = (
    "A USB device is on the S5-powered XHC4 controller. Poweroff may leave "
    "the discrete GPU rail on. Unplug that port before shutdown."
)
INHIBIT_WHY = (
    "USB device on S5-powered XHC4; poweroff may leave the dGPU rail on. "
    "Unplug that port, or use poweroff -i after reading docs."
)


@dataclass(frozen=True)
class UsbS5Settings:
    acpi_name: str = DEFAULT_ACPI_NAME
    inhibit: bool = DEFAULT_INHIBIT


@dataclass(frozen=True)
class UsbS5Status:
    state: str
    acpi_name: str
    pci: str | None
    ports: tuple[str, ...]
    reason: str

    @property
    def occupied(self) -> bool:
        return self.state == "occupied"


def parse_config(path: Path | None = None) -> UsbS5Settings:
    acpi_name = DEFAULT_ACPI_NAME
    inhibit = DEFAULT_INHIBIT
    config = DEFAULT_CONFIG if path is None else path
    if not config.is_file():
        return UsbS5Settings(acpi_name=acpi_name, inhibit=inhibit)

    for raw in config.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if key == "usb_s5_acpi" and value:
            acpi_name = value
        elif key == "usb_s5_inhibit":
            inhibit = value not in {"0", "false", "False", "no", "off"}
    return UsbS5Settings(acpi_name=acpi_name, inhibit=inhibit)


def pci_from_wakeup(text: str, acpi_name: str) -> str | None:
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0] != acpi_name:
            continue
        for part in parts[1:]:
            if part.startswith("pci:"):
                return part.removeprefix("pci:")
    return None


def occupied_ports(pci_device: Path) -> tuple[str, ...]:
    if not pci_device.is_dir():
        return ()

    ports: list[str] = []
    for child in pci_device.rglob("*"):
        if not child.is_dir():
            continue
        vendor = child / "idVendor"
        class_path = child / "bDeviceClass"
        if not vendor.is_file() or not class_path.is_file():
            continue
        try:
            device_class = int(class_path.read_text(encoding="utf-8").strip(), 16)
        except ValueError:
            continue
        if device_class == USB_HUB_CLASS:
            continue
        ports.append(child.name)
    return tuple(sorted(set(ports)))


def assess(
    *,
    acpi_name: str = DEFAULT_ACPI_NAME,
    wakeup_path: Path = DEFAULT_WAKEUP,
    pci_root: Path = DEFAULT_PCI_ROOT,
) -> UsbS5Status:
    if not wakeup_path.is_file():
        return UsbS5Status(
            state="undetectable",
            acpi_name=acpi_name,
            pci=None,
            ports=(),
            reason=f"{wakeup_path} is not readable",
        )

    pci = pci_from_wakeup(wakeup_path.read_text(encoding="utf-8"), acpi_name)
    if not pci:
        return UsbS5Status(
            state="undetectable",
            acpi_name=acpi_name,
            pci=None,
            ports=(),
            reason=f"ACPI {acpi_name} is not present in wakeup table",
        )

    pci_device = pci_root / pci
    if not pci_device.is_dir():
        return UsbS5Status(
            state="undetectable",
            acpi_name=acpi_name,
            pci=pci,
            ports=(),
            reason=f"PCI device {pci} is missing under {pci_root}",
        )

    ports = occupied_ports(pci_device)
    if ports:
        joined = ",".join(ports)
        return UsbS5Status(
            state="occupied",
            acpi_name=acpi_name,
            pci=pci,
            ports=ports,
            reason=f"{acpi_name} occupied at {joined}",
        )
    return UsbS5Status(
        state="empty",
        acpi_name=acpi_name,
        pci=pci,
        ports=(),
        reason=f"{acpi_name} has no non-hub USB devices",
    )


def format_status(status: UsbS5Status) -> str:
    pci = status.pci or "-"
    ports = ",".join(status.ports) if status.ports else "-"
    return (
        f"state={status.state} acpi={status.acpi_name} pci={pci} "
        f"ports={ports} reason={status.reason}"
    )
