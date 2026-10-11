import unittest

from hard_drive_monitor.health import assess
from hard_drive_monitor.smart import Drive, parse_smartctl
from tests.helpers import load


def A(name, **patch):
    d = parse_smartctl(load(name))
    d.metrics.update(patch)
    return d, assess(d)


class Scoring(unittest.TestCase):
    def test_healthy_hdd(self):
        _, a = A("hdd_healthy.json")
        self.assertEqual((a.health, a.performance, a.status), (100, 100, "OK"))
        self.assertEqual(a.reasons, [])

    def test_failing_hdd_is_critical_with_reasons(self):
        _, a = A("hdd_failing.json")
        # 100 * .99^120 * .97^16 * .97^16 * .98^3 * .995^1 = 10.58 -> 10
        self.assertEqual(a.health, 10)
        self.assertEqual(a.status, "CRITICAL")
        text = " ".join(a.reasons)
        self.assertIn("120 reallocated sectors", text)
        self.assertIn("remapping bad spots", text)
        self.assertIn("16 pending sectors", text)
        # perf: 100 - 12 (realloc .1*120) - 16 (pending) - 8 (offline .5*16) - 2 (timeout) = 62
        self.assertEqual(a.performance, 62)

    def test_few_reallocated_is_warning(self):
        _, a = A("hdd_healthy.json", reallocated_sectors=12)
        self.assertEqual(a.health, 88)      # 100 * .99^12 = 88.6
        self.assertEqual(a.status, "WARNING")
        self.assertIn("12 reallocated sectors - the drive is remapping bad spots to spare area", a.reasons)

    def test_crc_is_cable_warning_without_health_loss(self):
        _, a = A("hdd_healthy.json", udma_crc_errors=5)
        self.assertEqual(a.health, 100)
        self.assertEqual(a.performance, 95)
        self.assertEqual(a.status, "WARNING")
        self.assertTrue(any("cable" in r for r in a.reasons))

    def test_smart_failed_caps(self):
        d = parse_smartctl(load("hdd_healthy.json"))
        d.smart_passed = False
        a = assess(d)
        self.assertLessEqual(a.health, 5)
        self.assertEqual(a.status, "CRITICAL")

    def test_attribute_failing_now(self):
        _, a = A("hdd_healthy.json", failing_now=["5 Reallocated_Sector_Ct"])
        self.assertLessEqual(a.health, 10)
        self.assertEqual(a.status, "CRITICAL")

    def test_healthy_nvme(self):
        _, a = A("nvme_healthy.json")
        self.assertEqual(a.health, 97)  # 3% used
        self.assertEqual(a.status, "OK")

    def test_worn_nvme(self):
        _, a = A("nvme_worn.json")
        self.assertEqual(a.health, 38)
        self.assertEqual(a.status, "WARNING")
        self.assertTrue(a.reasons and "62%" in a.reasons[0])
        self.assertLess(a.performance, 100)  # time above warning temperature

    def test_nvme_critical_warning_bits(self):
        _, a = A("nvme_healthy.json", critical_warning=0x08)
        self.assertEqual(a.status, "CRITICAL")
        self.assertLessEqual(a.health, 20)
        self.assertTrue(any("READ-ONLY" in r for r in a.reasons))
        _, a = A("nvme_healthy.json", critical_warning=0x02)  # temperature only
        self.assertEqual(a.status, "WARNING")

    def test_nvme_spare_below_threshold(self):
        _, a = A("nvme_healthy.json", available_spare=5)
        self.assertEqual(a.status, "CRITICAL")
        self.assertLessEqual(a.health, 10)

    def test_nvme_media_errors(self):
        _, a = A("nvme_healthy.json", media_errors=12)
        self.assertEqual(a.status, "CRITICAL")
        _, a = A("nvme_healthy.json", media_errors=1)
        self.assertEqual(a.status, "WARNING")

    def test_wear_100_is_critical(self):
        _, a = A("ssd_sata_healthy.json", wear_percent_used=100)
        self.assertEqual(a.health, 0)
        self.assertEqual(a.status, "CRITICAL")

    def test_temperature(self):
        d = parse_smartctl(load("hdd_healthy.json"))
        d.temperature_c = 52
        self.assertEqual(assess(d).status, "WARNING")
        d.temperature_c = 58
        a = assess(d)
        self.assertEqual(a.status, "CRITICAL")
        self.assertEqual(a.health, 100)  # temperature never lowers Health
        d.temperature_c = 52
        self.assertEqual(assess(d, {"hdd_temp_warn": 55, "hdd_temp_crit": 60}).status, "OK")

    def test_unknown_when_no_smart(self):
        a = assess(Drive(device="/dev/vda", smart_available=False, notes=["virtio disk"]))
        self.assertEqual(a.status, "UNKNOWN")
        self.assertIsNone(a.health)


if __name__ == "__main__":
    unittest.main()
