#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from omen_wmi_usb_s5 import (
    UsbS5Settings,
    assess,
    occupied_ports,
    parse_config,
    pci_from_wakeup,
)


WAKEUP = """\
Device	S-state	  Status   Sysfs node
GPP0	  S4	*enabled   pci:0000:00:01.1
XHC1	  S4	*enabled   pci:0000:c4:00.4
XHC0	  S3	*enabled   pci:0000:c6:00.0
XHC4	  S4	*enabled   pci:0000:c6:00.4
"""


def write_usb_node(root: Path, name: str, vendor: str, device_class: str) -> None:
    node = root / name
    node.mkdir(parents=True)
    (node / "idVendor").write_text(f"{vendor}\n", encoding="utf-8")
    (node / "bDeviceClass").write_text(f"{device_class}\n", encoding="utf-8")


class UsbS5Tests(unittest.TestCase):
    def test_pci_from_wakeup(self):
        self.assertEqual(pci_from_wakeup(WAKEUP, "XHC4"), "0000:c6:00.4")
        self.assertIsNone(pci_from_wakeup(WAKEUP, "XHC9"))

    def test_parse_config_defaults_and_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing.conf"
            self.assertEqual(
                parse_config(missing),
                UsbS5Settings(acpi_name="XHC4", inhibit=True),
            )
            path = Path(tmp) / "omen-wmi-boost.conf"
            path.write_text(
                "# comment\nusb_s5_acpi=XHC4\nusb_s5_inhibit=0\n",
                encoding="utf-8",
            )
            self.assertEqual(
                parse_config(path),
                UsbS5Settings(acpi_name="XHC4", inhibit=False),
            )

    def test_occupied_ignores_hubs(self):
        with tempfile.TemporaryDirectory() as tmp:
            pci = Path(tmp) / "0000:c6:00.4"
            write_usb_node(pci, "usb7", "1d6b", "09")
            write_usb_node(pci / "usb7", "7-1", "1234", "00")
            write_usb_node(pci, "usb8", "1d6b", "09")
            self.assertEqual(occupied_ports(pci), ("7-1",))

    def test_assess_occupied_empty_and_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            wakeup = root / "wakeup"
            wakeup.write_text(WAKEUP, encoding="utf-8")
            pci_root = root / "pci"
            pci = pci_root / "0000:c6:00.4"
            write_usb_node(pci, "usb7", "1d6b", "09")

            empty = assess(
                acpi_name="XHC4",
                wakeup_path=wakeup,
                pci_root=pci_root,
            )
            self.assertEqual(empty.state, "empty")
            self.assertEqual(empty.pci, "0000:c6:00.4")
            self.assertFalse(empty.occupied)

            write_usb_node(pci / "usb7", "7-1", "abcd", "00")
            occupied = assess(
                acpi_name="XHC4",
                wakeup_path=wakeup,
                pci_root=pci_root,
            )
            self.assertEqual(occupied.state, "occupied")
            self.assertEqual(occupied.ports, ("7-1",))

            missing = assess(
                acpi_name="XHC9",
                wakeup_path=wakeup,
                pci_root=pci_root,
            )
            self.assertEqual(missing.state, "undetectable")
            self.assertFalse(missing.occupied)


if __name__ == "__main__":
    unittest.main()
