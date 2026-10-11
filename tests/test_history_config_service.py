import os
import plistlib
import shutil
import tempfile
import unittest

from hard_drive_monitor.alerts import build_alerts
from hard_drive_monitor.collect import score
from hard_drive_monitor.config import DEFAULTS, SAMPLE_CONFIG, parse_simple_toml, load_config
from hard_drive_monitor.history import History, compare
from hard_drive_monitor.service import (cron_lines, launchd_plist, parse_interval, systemd_units, windows_task_xml)
from hard_drive_monitor.smart import load_json_files
from tests.helpers import FIX


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.db = os.path.join(self.tmp, "h.db")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_records_and_flags_increase(self):
        first = score(load_json_files([FIX]), DEFAULTS)
        later = score(load_json_files([os.path.join(FIX, "later", "hdd_failing.json")]), DEFAULTS)
        with History(self.db) as h:
            c1 = h.record(first, ts="2026-10-10T12:00:00+00:00")
            self.assertTrue(all(v == [] for v in c1.values()))       # first run: nothing to compare
            c2 = h.record(first, ts="2026-10-10T13:00:00+00:00")
            self.assertTrue(all(v == [] for v in c2.values()))       # unchanged: no alerts
            c3 = h.record(later, ts="2026-10-10T14:00:00+00:00")
            key = later[0][0].key
            got = {c["metric"]: (c["old"], c["new"]) for c in c3[key]}
            self.assertEqual(got["reallocated_sectors"], (120, 152))
            self.assertEqual(got["pending_sectors"], (16, 21))
            alerts = build_alerts(later, c3)
            self.assertEqual(len(alerts), 1)
            self.assertIn("reallocated sectors: 120 -> 152 (+32)", alerts[0]["body"])
            self.assertIn("Hard Drive Monitor", alerts[0]["title"])
            summ = h.summary()
            sdb = next(s for s in summ if s["drive_key"] == key)
            self.assertEqual(sdb["n"], 3)
            self.assertTrue(sdb["changes_since_last"])
            self.assertEqual(len(h.drives()), 5)

    def test_no_alert_without_worsening(self):
        self.assertEqual(compare({"status": "WARNING", "health": 50, "metrics": {"reallocated_sectors": 5}},
                                 "WARNING", 50, {"reallocated_sectors": 5}), [])
        ch = compare({"status": "OK", "health": 90, "metrics": {}}, "WARNING", 80, {"wear_percent_used": 5})
        self.assertEqual([c["metric"] for c in ch if c["alert"]], ["status"])
        # wear going up is a trend, not an alert
        ch = compare({"status": "OK", "health": 90, "metrics": {"wear_percent_used": 4}}, "OK", 90,
                     {"wear_percent_used": 5})
        self.assertFalse(any(c["alert"] for c in ch))


class ConfigTests(unittest.TestCase):
    def test_simple_toml_parser_reads_sample(self):
        c = parse_simple_toml(SAMPLE_CONFIG)
        self.assertEqual(c["email"]["port"], 587)
        self.assertEqual(c["email"]["to"], ["you@example.com"])
        self.assertIs(c["alerts"]["desktop"], True)
        self.assertEqual(c["smartctl"]["sudo"], "auto")
        self.assertEqual(c["ignore"]["devices"], [])

    def test_load_merges_defaults(self):
        tmp = tempfile.mkdtemp()
        try:
            p = os.path.join(tmp, "c.toml")
            with open(p, "w") as fh:
                fh.write('[thresholds]\nhdd_temp_warn = 45 # comment\n[email]\nto = ["a@b.c", "d@e.f"]\n')
            c = load_config(p)
            self.assertEqual(c["thresholds"]["hdd_temp_warn"], 45)
            self.assertEqual(c["thresholds"]["hdd_temp_crit"], 55)
            self.assertEqual(c["email"]["to"], ["a@b.c", "d@e.f"])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class ServiceTests(unittest.TestCase):
    def test_interval(self):
        self.assertEqual(parse_interval("1h"), 3600)
        self.assertEqual(parse_interval("2h30m"), 9000)
        self.assertEqual(parse_interval("90s"), 90)
        self.assertEqual(parse_interval("600"), 600)
        with self.assertRaises(ValueError):
            parse_interval("soon")

    def test_systemd(self):
        u = systemd_units(["/usr/bin/hdmon"], {}, 3600)
        self.assertIn("ExecStart=/usr/bin/hdmon check --quiet", u["hard-drive-monitor.service"])
        self.assertIn("OnUnitActiveSec=3600s", u["hard-drive-monitor.timer"])
        self.assertIn("OnStartupSec=2min", u["hard-drive-monitor.timer"])
        self.assertIn("WantedBy=timers.target", u["hard-drive-monitor.timer"])

    def test_launchd(self):
        p = plistlib.loads(launchd_plist(["/opt/homebrew/bin/hdmon"], {}, 3600, "/tmp").encode())
        self.assertEqual(p["Label"], "com.hard-drive-monitor.check")
        self.assertEqual(p["StartInterval"], 3600)
        self.assertEqual(p["ProgramArguments"], ["/opt/homebrew/bin/hdmon", "check", "--quiet"])

    def test_windows_task_xml(self):
        import xml.etree.ElementTree as ET
        x = windows_task_xml([r"C:\Program Files\Py\pythonw.exe", "-m", "hard_drive_monitor"], 3600,
                             highest=True, user="PC\\richard")
        ns = {"t": "http://schemas.microsoft.com/windows/2004/02/mit/task"}
        root = ET.fromstring(x.split("?>", 1)[1])
        self.assertIsNotNone(root.find("t:Triggers/t:LogonTrigger", ns))
        self.assertEqual(root.find("t:Triggers/t:TimeTrigger/t:Repetition/t:Interval", ns).text, "PT1H")
        self.assertEqual(root.find("t:Principals/t:Principal/t:RunLevel", ns).text, "HighestAvailable")
        self.assertEqual(root.find("t:Actions/t:Exec/t:Command", ns).text, r"C:\Program Files\Py\pythonw.exe")
        self.assertEqual(root.find("t:Actions/t:Exec/t:Arguments", ns).text, "-m hard_drive_monitor check --quiet")
        low = windows_task_xml(["hdmon.exe"], 1800)
        self.assertIn("<RunLevel>LeastPrivilege</RunLevel>", low)
        self.assertIn("<Interval>PT30M</Interval>", low)

    def test_cron_fallback(self):
        lines = cron_lines(["/home/r/.local/bin/hdmon"], {}, 3600)
        self.assertTrue(lines[0].startswith("@reboot sleep 120; /home/r/.local/bin/hdmon check --quiet"))
        self.assertTrue(lines[1].startswith("7 * * * * "))
        self.assertTrue(all(l.endswith("# hard-drive-monitor") for l in lines))
        self.assertTrue(cron_lines(["x"], {}, 1800)[1].startswith("*/30 "))


if __name__ == "__main__":
    unittest.main()
