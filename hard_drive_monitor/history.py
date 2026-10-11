"""SQLite run log and change detection."""
from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .health import RANK, Assessment
from .smart import Drive

SCHEMA = """
CREATE TABLE IF NOT EXISTS runs(
  id INTEGER PRIMARY KEY, ts TEXT NOT NULL, source TEXT);
CREATE TABLE IF NOT EXISTS readings(
  id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL REFERENCES runs(id),
  drive_key TEXT NOT NULL, device TEXT, model TEXT, serial TEXT,
  status TEXT, health INTEGER, performance INTEGER, temperature INTEGER,
  power_on_hours INTEGER, metrics TEXT);
CREATE INDEX IF NOT EXISTS ix_readings_drive ON readings(drive_key, run_id);
"""

# Counters that must never go up on a healthy drive. An increase triggers an alert.
ALERT_COUNTERS = [
    "reallocated_sectors", "pending_sectors", "offline_uncorrectable", "reported_uncorrectable",
    "command_timeout", "udma_crc_errors", "media_errors", "uncorrected_errors",
]
# Tracked and shown as trends, but they rise normally, so they do not alert by themselves.
TREND_METRICS = ["wear_percent_used", "unsafe_shutdowns", "error_log_entries"]


def compare(prev: Optional[Dict[str, Any]], status: str, health: Optional[int],
            metrics: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Changes since the previous reading. kind: status | counter | health | trend."""
    if not prev:
        return []
    out = []
    pm = prev.get("metrics") or {}
    if RANK.get(status, 0) > RANK.get(prev.get("status") or "OK", 0):
        out.append({"kind": "status", "metric": "status", "old": prev.get("status"), "new": status, "alert": True})
    for k in ALERT_COUNTERS:
        old, new = pm.get(k), metrics.get(k)
        if new is not None and new > (old or 0):
            out.append({"kind": "counter", "metric": k, "old": old or 0, "new": new, "alert": True})
    if health is not None and prev.get("health") is not None and health < prev["health"]:
        out.append({"kind": "health", "metric": "health", "old": prev["health"], "new": health, "alert": False})
    for k in TREND_METRICS:
        old, new = pm.get(k), metrics.get(k)
        if new is not None and old is not None and new > old:
            out.append({"kind": "trend", "metric": k, "old": old, "new": new, "alert": False})
    return out


class History:
    def __init__(self, path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(str(self.path))
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)

    def close(self):
        self.conn.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @staticmethod
    def _row(r) -> Dict[str, Any]:
        d = dict(r)
        d["metrics"] = json.loads(d.get("metrics") or "{}")
        return d

    def last_reading(self, drive_key: str) -> Optional[Dict[str, Any]]:
        r = self.conn.execute(
            "SELECT r.*, runs.ts FROM readings r JOIN runs ON runs.id = r.run_id "
            "WHERE drive_key = ? ORDER BY r.run_id DESC LIMIT 1", (drive_key,)).fetchone()
        return self._row(r) if r else None

    def record(self, results: List[Tuple[Drive, Assessment]], source: str = "live",
               ts: Optional[str] = None) -> Dict[str, List[Dict[str, Any]]]:
        """Store one run; return {drive_key: [changes vs. previous reading]}."""
        ts = ts or datetime.now(timezone.utc).isoformat(timespec="seconds")
        changes: Dict[str, List[Dict[str, Any]]] = {}
        with self.conn:
            run_id = self.conn.execute("INSERT INTO runs(ts, source) VALUES (?, ?)", (ts, source)).lastrowid
            for d, a in results:
                prev = self.last_reading(d.key)
                changes[d.key] = compare(prev, a.status, a.health, d.metrics)
                self.conn.execute(
                    "INSERT INTO readings(run_id, drive_key, device, model, serial, status, health, performance,"
                    " temperature, power_on_hours, metrics) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    (run_id, d.key, d.device, d.model, d.serial, a.status, a.health, a.performance,
                     d.temperature_c, d.power_on_hours, json.dumps(d.metrics, default=str)))
        return changes

    def drives(self) -> List[Dict[str, Any]]:
        rows = self.conn.execute(
            "SELECT drive_key, MAX(device) device, MAX(model) model, MAX(serial) serial, COUNT(*) n, "
            "MIN(runs.ts) first_ts, MAX(runs.ts) last_ts FROM readings JOIN runs ON runs.id = readings.run_id "
            "GROUP BY drive_key ORDER BY device").fetchall()
        return [dict(r) for r in rows]

    def readings(self, drive_key: str, limit: int = 50) -> List[Dict[str, Any]]:
        rows = self.conn.execute(
            "SELECT r.*, runs.ts FROM readings r JOIN runs ON runs.id = r.run_id WHERE drive_key = ? "
            "ORDER BY r.run_id DESC LIMIT ?", (drive_key, limit)).fetchall()
        return [self._row(r) for r in reversed(rows)]

    def summary(self, limit: int = 20, drive_filter: Optional[str] = None) -> List[Dict[str, Any]]:
        out = []
        for drv in self.drives():
            if drive_filter and drive_filter not in (drv["device"] or "") and drive_filter != drv["serial"] \
                    and not (drv["device"] or "").endswith("/" + drive_filter):
                continue
            rs = self.readings(drv["drive_key"], limit)
            flagged = []
            for prev, cur in zip(rs, rs[1:]):
                for c in compare(prev, cur["status"], cur["health"], cur["metrics"]):
                    c["ts"] = cur["ts"]
                    flagged.append(c)
            latest = compare(rs[-2], rs[-1]["status"], rs[-1]["health"], rs[-1]["metrics"]) if len(rs) > 1 else []
            drv.update(readings=rs, changes_in_window=flagged, changes_since_last=latest)
            out.append(drv)
        return out
