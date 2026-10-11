import unittest

from hard_drive_monitor.smart import ata_raw_count, parse_smartctl, parse_version, install_hint
from tests.helpers import load


class FixturesAreSynthetic(unittest.TestCase):
    def test_marked(self):
        for name in ("hdd_healthy.json", "hdd_failing.json", "ssd_sata_healthy.json", "nvme_healthy.json",
                     "nvme_worn.json", "later/hdd_failing.json", "windows/windows_inventory.json"):
            self.assertIn("SYNTHETIC", load(name)["_synthetic"], name)


class AtaParsing(unittest.TestCase):
    def test_healthy_hdd(self):
        d = parse_smartctl(load("hdd_healthy.json"))
        self.assertEqual(d.device, "/dev/sda")
        self.assertEqual(d.kind, "HDD")
        self.assertEqual(d.model, "WDC WD40EFRX-68N32N0")
        self.assertEqual(d.serial, "WD-SYNTH00000001")
        self.assertEqual(d.capacity_bytes, 4000787030016)
        self.assertEqual(d.temperature_c, 34)
        self.assertEqual(d.power_on_hours, 23456)
        self.assertEqual(d.power_cycles, 312)
        self.assertTrue(d.smart_passed)
        for k in ("reallocated_sectors", "pending_sectors", "offline_uncorrectable", "udma_crc_errors"):
            self.assertEqual(d.metrics[k], 0, k)
        self.assertNotIn("wear_percent_used", d.metrics)  # HDDs have no wear
        self.assertEqual(d.self_test["extended_minutes"], 470)
        self.assertEqual(len(d.self_test_log), 2)

    def test_failing_hdd(self):
        d = parse_smartctl(load("hdd_failing.json"))
        m = d.metrics
        self.assertEqual(m["reallocated_sectors"], 120)
        self.assertEqual(m["pending_sectors"], 16)
        self.assertEqual(m["offline_uncorrectable"], 16)
        self.assertEqual(m["reported_uncorrectable"], 3)
        self.assertEqual(m["command_timeout"], 1)  # Seagate packed raw 0x100010001 -> low 16 bits
        self.assertFalse(d.self_test_log[0]["passed"])
        self.assertEqual(d.self_test_log[0]["lba"], 1052763384)

    def test_sata_ssd_wear_from_device_statistics(self):
        d = parse_smartctl(load("ssd_sata_healthy.json"))
        self.assertEqual(d.kind, "SSD")
        self.assertEqual(d.metrics["wear_percent_used"], 6)
        self.assertEqual(d.metrics["wear_source"], "device statistics")

    def test_sata_ssd_wear_from_attribute_fallback(self):
        data = load("ssd_sata_healthy.json")
        del data["ata_device_statistics"]
        d = parse_smartctl(data)
        self.assertEqual(d.metrics["wear_percent_used"], 6)  # 177 normalized 94
        self.assertIn("177", d.metrics["wear_source"])

    def test_temperature_from_attribute_194(self):
        data = load("hdd_healthy.json")
        del data["temperature"]
        self.assertEqual(parse_smartctl(data).temperature_c, 34)

    def test_when_failed(self):
        data = load("hdd_failing.json")
        data["ata_smart_attributes"]["table"][2]["when_failed"] = "now"
        self.assertEqual(parse_smartctl(data).metrics["failing_now"], ["5 Reallocated_Sector_Ct"])

    def test_raw_counts(self):
        self.assertEqual(ata_raw_count(188, {"value": 4295032833}), 1)
        self.assertEqual(ata_raw_count(5, {"value": 12}), 12)
        self.assertIsNone(ata_raw_count(5, {}))


class NvmeParsing(unittest.TestCase):
    def test_healthy(self):
        d = parse_smartctl(load("nvme_healthy.json"))
        self.assertEqual(d.kind, "NVMe")
        m = d.metrics
        self.assertEqual(m["wear_percent_used"], 3)
        self.assertEqual(m["media_errors"], 0)
        self.assertEqual(m["critical_warning"], 0)
        self.assertEqual(m["available_spare"], 100)
        self.assertEqual(m["unsafe_shutdowns"], 14)
        self.assertEqual(d.capacity_bytes, 1000204886016)
        self.assertEqual(d.self_test["short_minutes"], 2)

    def test_worn(self):
        d = parse_smartctl(load("nvme_worn.json"))
        self.assertEqual(d.metrics["wear_percent_used"], 62)
        self.assertEqual(d.metrics["available_spare"], 64)
        self.assertEqual(d.metrics["warning_temp_minutes"], 95)
        self.assertGreater(d.metrics["data_written_tb"], 300)


class Misc(unittest.TestCase):
    def test_unsupported_device(self):
        d = parse_smartctl({"device": {"name": "/dev/sdx", "protocol": "SCSI"}, "smart_support": {"available": False}})
        self.assertFalse(d.smart_available)

    def test_version(self):
        self.assertEqual(parse_version("smartctl 7.4 2023-08-01 r5530 [x86_64-linux]"), (7, 4))
        self.assertIsNone(parse_version("garbage"))

    def test_install_hints(self):
        self.assertIn("brew install smartmontools", install_hint("darwin"))
        self.assertIn("winget install smartmontools.smartmontools", install_hint("win32"))
        self.assertIn("apt install smartmontools", install_hint("linux"))

    def test_drive_key_prefers_serial(self):
        d = parse_smartctl(load("hdd_healthy.json"))
        self.assertTrue(d.key.endswith("WD-SYNTH00000001"))


if __name__ == "__main__":
    unittest.main()
