"""Windows code paths, tested on any OS with SYNTHETIC PowerShell JSON fixtures."""
import unittest

from hard_drive_monitor.collect import apply_windows_letters, score
from hard_drive_monitor.config import DEFAULTS
from hard_drive_monitor.smart import (Drive, norm_serial, parse_smartctl, parse_windows_physical,
                                      windows_disk_number, windows_letter_map)
from tests.helpers import load


class WindowsInventory(unittest.TestCase):
    def setUp(self):
        self.inv = load("windows/windows_inventory.json")

    def test_letter_map(self):
        self.assertEqual(windows_letter_map(self.inv), {0: ["C:"], 1: ["D:"], 2: ["G:"]})

    def test_fallback_parse(self):
        drives = parse_windows_physical(self.inv)
        self.assertEqual([d.device for d in drives], ["PhysicalDrive0", "PhysicalDrive1", "PhysicalDrive2"])
        nvme, hdd, usb = drives
        self.assertEqual((nvme.kind, nvme.volumes, nvme.metrics["wear_percent_used"]), ("NVMe", ["C:"], 4))
        self.assertEqual((hdd.kind, hdd.volumes, hdd.temperature_c, hdd.power_on_hours), ("HDD", ["D:"], 44, 41877))
        self.assertEqual(hdd.metrics["uncorrected_errors"], 7)
        self.assertEqual(usb.volumes, ["G:"])
        self.assertFalse(usb.smart_passed)

    def test_fallback_scoring(self):
        res = {d.device: a for d, a in score(parse_windows_physical(self.inv), DEFAULTS)}
        self.assertEqual(res["PhysicalDrive0"].status, "OK")
        self.assertEqual(res["PhysicalDrive1"].status, "WARNING")   # 7 uncorrected + Windows "Warning"
        self.assertEqual(res["PhysicalDrive2"].status, "CRITICAL")  # Windows "Unhealthy"
        self.assertTrue(any("Unhealthy" in r for r in res["PhysicalDrive2"].reasons))

    def test_smartctl_device_to_disk_number(self):
        self.assertEqual(windows_disk_number("/dev/sdc", None, {}), 2)
        self.assertEqual(windows_disk_number("/dev/sdaa", None, {}), 26)
        self.assertEqual(windows_disk_number("/dev/pd3", None, {}), 3)
        # serial match wins over the device-name guess (NVMe serial with Windows punctuation)
        self.assertEqual(windows_disk_number("/dev/sdb", "0025 3852 91B0 SYN1", self.inv), 0)
        self.assertEqual(norm_serial("0025_3852_91B0_SYN1."), "0025385291B0SYN1")

    def test_single_object_json_and_letters_applied(self):
        inv = load("windows/windows_letters_single.json")   # ConvertTo-Json collapsed arrays
        d = parse_smartctl(load("hdd_failing.json"))         # /dev/sdb, serial ZFL0SYNTH02
        apply_windows_letters([d], inv)
        self.assertEqual(d.volumes, ["D:"])
        self.assertTrue(any("PhysicalDrive1" in n for n in d.notes))

    def test_no_reliability_counters(self):
        inv = {"PhysicalDisks": {"DeviceId": "5", "FriendlyName": "X", "MediaType": "Unspecified",
                                 "BusType": "USB", "HealthStatus": "Unknown", "Reliability": None}}
        d = parse_windows_physical(inv)[0]
        self.assertFalse(d.smart_available)


if __name__ == "__main__":
    unittest.main()
