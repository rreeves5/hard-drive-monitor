import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from tests.helpers import FIX

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(*args):
    env = dict(os.environ, NO_COLOR="1", PYTHONIOENCODING="utf-8",
               PYTHONPATH=ROOT + os.pathsep + os.environ.get("PYTHONPATH", ""))
    p = subprocess.run([sys.executable, "-m", "hard_drive_monitor", *args], capture_output=True, text=True,
                       encoding="utf-8", env=env, cwd=ROOT)
    return p.returncode, p.stdout, p.stderr


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.db = os.path.join(self.tmp, "h.db")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_status_text(self):
        rc, out, err = run("status", "--from-json", FIX)
        self.assertEqual(rc, 0, err)
        self.assertIn("CRITICAL", out)
        self.assertIn("120 reallocated sectors", out)
        self.assertIn("5 drive(s)", out)

    def test_status_json(self):
        rc, out, _ = run("status", "--from-json", FIX, "--json")
        data = json.loads(out)
        st = {d["drive"]["device"]: d["assessment"]["status"] for d in data["drives"]}
        self.assertEqual(st, {"/dev/sda": "OK", "/dev/sdb": "CRITICAL", "/dev/sdc": "OK",
                              "/dev/nvme0": "OK", "/dev/nvme1": "WARNING"})

    def test_detail(self):
        rc, out, _ = run("detail", "nvme1", "--from-json", FIX)
        self.assertEqual(rc, 0)
        self.assertIn("percentage_used", out)
        rc, out, _ = run("detail", "/dev/sdb", "--from-json", FIX, "--json")
        self.assertEqual(json.loads(out)["drive"]["metrics"]["pending_sectors"], 16)
        rc, _, err = run("detail", "nope", "--from-json", FIX)
        self.assertEqual(rc, 3)

    def test_check_history_flow(self):
        rc, _, _ = run("check", "--from-json", FIX, "--record", "--db", self.db, "--no-alert", "--quiet")
        self.assertEqual(rc, 2)  # worst = CRITICAL
        rc, out, _ = run("check", "--from-json", os.path.join(FIX, "later"), "--record", "--db", self.db,
                         "--no-alert", "--json")
        data = json.loads(out)
        self.assertEqual(len(data["alerts"]), 1)
        rc, out, _ = run("history", "--db", self.db)
        self.assertEqual(rc, 0)
        self.assertIn("Worse since the previous run", out)
        rc, out, _ = run("history", "--db", self.db, "--json")
        self.assertEqual(len(json.loads(out)["drives"]), 5)

    def test_from_json_does_not_record_by_default(self):
        run("status", "--from-json", FIX, "--db", self.db)
        self.assertFalse(os.path.exists(self.db))

    def test_selftest_results_from_json(self):
        rc, out, _ = run("selftest", "sdb", "--results", "--from-json", FIX)
        self.assertEqual(rc, 0)
        self.assertIn("read failure", out)
        rc, _, _ = run("selftest", "sdb", "short", "--from-json", FIX)
        self.assertEqual(rc, 3)

    def test_windows_inventory_file(self):
        rc, out, _ = run("status", "--from-json", os.path.join(FIX, "windows", "windows_inventory.json"))
        self.assertIn("PhysicalDrive1 (D:)", out)
        self.assertIn("PhysicalDrive2 (G:)", out)
        rc, out, _ = run("detail", "G:", "--from-json", os.path.join(FIX, "windows", "windows_inventory.json"))
        self.assertIn("WDC WD20EARX", out)

    def test_install_service_dry_run(self):
        rc, out, _ = run("install-service", "--dry-run", "--json")
        self.assertEqual(rc, 0)
        self.assertTrue(json.loads(out)["ok"])

    def test_version(self):
        rc, out, _ = run("--version")
        self.assertIn("Hard Drive Monitor", out)


if __name__ == "__main__":
    unittest.main()
