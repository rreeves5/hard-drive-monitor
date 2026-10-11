"""Health %, Performance % and status, Hard Disk Sentinel style.

HEALTH (same model as Richard's Windows DiskMonitor, so the numbers match):
  Health starts at 100 and every defect removes a percentage of what is left
  (multiplicative), then hard caps apply. Constants are below and in README.md.

PERFORMANCE (estimate, not a benchmark):
  Starts at 100; fixed deductions for things that make a drive slow even while
  its data is still safe: retries over a bad cable, stalls/timeouts, re-reads of
  weak sectors, remapped-sector seeks, thermal throttling.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from .smart import Drive

# ---- Health: per-unit multiplicative factors (each unit keeps this fraction of the score)
HEALTH_FACTORS = {
    "reallocated_sectors":    0.99,   # ATA 5    -1% per reallocated sector
    "pending_sectors":        0.97,   # ATA 197  -3% per pending (unstable) sector
    "offline_uncorrectable":  0.97,   # ATA 198  -3% per offline-uncorrectable sector
    "reported_uncorrectable": 0.98,   # ATA 187  -2% per reported uncorrectable error
    "command_timeout":        0.995,  # ATA 188  -0.5% per command timeout
    "media_errors":           0.97,   # NVMe     -3% per media/data-integrity error
    "uncorrected_errors":     0.98,   # Windows Storage fallback: uncorrected read+write errors
}
# Wear: score *= (100 - percent_used) / 100

# ---- Health: hard caps
CAP_SMART_FAILED = 5          # drive's own SMART verdict FAILED / Windows "Unhealthy"
CAP_ATTRIBUTE_FAILING_NOW = 10
CAP_NVME_SPARE_LOW = 10       # available_spare below available_spare_threshold
CAP_NVME_CRITICAL_WARNING = 20  # critical_warning bits 0, 2, 3 or 4

# ---- Status thresholds on the Health score
CRITICAL_BELOW = 30
WARNING_BELOW = 70

# ---- Performance: (points per unit, max points)
PERF_DEDUCTIONS = {
    "udma_crc_errors":       (1.0, 30),   # retransmits over cable/port
    "command_timeout":       (2.0, 30),   # stalls
    "pending_sectors":       (1.0, 20),   # repeated re-reads of weak sectors
    "reallocated_sectors":   (0.1, 20),   # extra seeks to the spare area
    "offline_uncorrectable": (0.5, 10),
    "media_errors":          (1.0, 10),
}
PERF_HOT = 10            # currently at/above the warning temperature
PERF_VERY_HOT = 25       # at/above the critical temperature
PERF_THROTTLE_MAX = 15   # NVMe minutes spent above warning/critical temp: 1 + hours, max 15

NVME_CW_BITS = {
    0: ("critical", "available spare space has fallen below the threshold"),
    1: ("warning", "temperature is outside the safe range"),
    2: ("critical", "NVM subsystem reliability is degraded (media or internal errors)"),
    3: ("critical", "drive has switched to READ-ONLY mode"),
    4: ("critical", "volatile memory backup device has failed"),
}

RANK = {"OK": 0, "UNKNOWN": 0, "WARNING": 1, "CRITICAL": 2}


@dataclass
class Finding:
    severity: str        # critical | warning | info
    message: str
    metric: Optional[str] = None


@dataclass
class Assessment:
    health: Optional[int]
    performance: Optional[int]
    status: str
    findings: List[Finding] = field(default_factory=list)

    @property
    def reasons(self) -> List[str]:
        return [f.message for f in self.findings if f.severity in ("critical", "warning")]

    def to_dict(self) -> Dict[str, Any]:
        return {"health": self.health, "performance": self.performance, "status": self.status,
                "reasons": self.reasons,
                "findings": [f.__dict__ for f in self.findings]}


def _n(n: int, one: str, many: Optional[str] = None) -> str:
    return f"{n:,} {one if n == 1 else (many or one + 's')}"


MESSAGES = {
    "reallocated_sectors": lambda n: f"{_n(n, 'reallocated sector')} - the drive is remapping bad spots to spare area",
    "pending_sectors": lambda n: f"{_n(n, 'pending sector')} - unreadable spots waiting to be remapped; data there may already be lost",
    "offline_uncorrectable": lambda n: f"{_n(n, 'offline uncorrectable sector')} - found by the drive's own surface scan and could not be repaired",
    "reported_uncorrectable": lambda n: f"{_n(n, 'reported uncorrectable error')} - reads failed even after error correction",
    "command_timeout": lambda n: f"{_n(n, 'command timeout')} - the drive stalled on operations (failing drive, cable or power)",
    "udma_crc_errors": lambda n: f"{_n(n, 'UDMA CRC error')} - data got garbled on the cable; reseat or replace the SATA cable/port",
    "media_errors": lambda n: f"{_n(n, 'NVMe media error')} - unrecovered data-integrity errors",
    "uncorrected_errors": lambda n: f"{_n(n, 'uncorrected read/write error')} reported by Windows",
}
# Count at/above which a counter alone makes the drive CRITICAL (below: WARNING).
CRITICAL_COUNTS = {"pending_sectors": 10, "offline_uncorrectable": 10, "reallocated_sectors": 100,
                   "reported_uncorrectable": 10, "media_errors": 10, "uncorrected_errors": 10}


def temp_limits(drive: Drive, thresholds: Optional[Dict[str, Any]] = None):
    t = thresholds or {}
    kind = {"HDD": "hdd", "SSD": "ssd", "NVMe": "nvme"}.get(drive.kind, "hdd")
    defaults = {"hdd": (50, 55), "ssd": (60, 70), "nvme": (70, 80)}[kind]
    warn = int(t.get(f"{kind}_temp_warn", defaults[0]))
    crit = int(t.get(f"{kind}_temp_crit", defaults[1]))
    if drive.temp_limit_warn:
        warn = min(warn, drive.temp_limit_warn) if kind == "hdd" else drive.temp_limit_warn
    if drive.temp_limit_crit:
        crit = drive.temp_limit_crit
    return warn, max(crit, warn)


def assess(drive: Drive, thresholds: Optional[Dict[str, Any]] = None) -> Assessment:
    f: List[Finding] = []
    m = drive.metrics or {}
    perf = 100.0

    # temperature (status + performance only, no Health effect)
    tw, tc = temp_limits(drive, thresholds)
    if drive.temperature_c is not None and drive.temperature_c > 0:
        if drive.temperature_c >= tc:
            f.append(Finding("critical", f"Temperature {drive.temperature_c} C is critical (limit {tc} C) - check cooling", "temperature"))
            perf -= PERF_VERY_HOT
        elif drive.temperature_c >= tw:
            f.append(Finding("warning", f"Temperature {drive.temperature_c} C is high (warning at {tw} C)", "temperature"))
            perf -= PERF_HOT

    if not drive.smart_available:
        note = drive.notes[0] if drive.notes else "device did not report SMART data"
        f.append(Finding("info", f"No SMART data: {note}"))
        return Assessment(None, None, "UNKNOWN", f)

    h = 100.0
    cap = 100
    force_critical = False
    defects = False

    for key, factor in HEALTH_FACTORS.items():
        n = m.get(key)
        if not n or n <= 0:
            continue
        h *= math.pow(factor, min(n, 100000))
        if key == "udma_crc_errors":
            continue
        defects = True
        sev = "critical" if n >= CRITICAL_COUNTS.get(key, 10**9) else "warning"
        f.append(Finding(sev, MESSAGES[key](n), key))

    crc = m.get("udma_crc_errors") or 0
    if crc > 0:   # interface problem: no Health penalty, but worth a WARNING
        f.append(Finding("warning", MESSAGES["udma_crc_errors"](crc), "udma_crc_errors"))

    for key, (per, mx) in PERF_DEDUCTIONS.items():
        n = m.get(key) or 0
        if n > 0:
            perf -= min(mx, per * n)

    wear = m.get("wear_percent_used")
    if wear is not None:
        used = max(0, min(100, wear))
        h *= (100 - used) / 100.0
        label = "NVMe" if drive.kind == "NVMe" else "SSD"
        if wear >= 100:
            f.append(Finding("critical", f"{label} wear: {wear}% of rated write endurance used - plan a replacement", "wear_percent_used"))
        elif wear >= 80:
            f.append(Finding("warning", f"{label} wear: {wear}% of rated write endurance used", "wear_percent_used"))
        elif wear >= 10:
            f.append(Finding("info", f"{label} wear: {wear}% of rated write endurance used", "wear_percent_used"))

    if drive.smart_passed is False:
        cap = min(cap, CAP_SMART_FAILED)
        force_critical = True
        what = "Windows reports this disk as Unhealthy" if drive.source.startswith("windows") \
            else "Drive's own SMART self-assessment says FAILED"
        f.append(Finding("critical", f"{what} - back up now", "smart_status"))
    if m.get("windows_health_status") == "Warning":
        defects = True
        f.append(Finding("warning", "Windows reports this disk's health as Warning", "windows_health_status"))

    if m.get("failing_now"):
        cap = min(cap, CAP_ATTRIBUTE_FAILING_NOW)
        force_critical = True
        f.append(Finding("critical", "Attribute below its failure threshold now: " + ", ".join(m["failing_now"]), "failing_now"))
    if m.get("failed_past"):
        defects = True
        f.append(Finding("warning", "Attribute was below its failure threshold in the past: " + ", ".join(m["failed_past"]), "failed_past"))

    cw = m.get("critical_warning") or 0
    if cw:
        for bit, (sev, text) in NVME_CW_BITS.items():
            if cw & (1 << bit):
                f.append(Finding(sev, f"NVMe critical warning (bit {bit}): {text}", "critical_warning"))
        if cw & 0b11101:
            cap = min(cap, CAP_NVME_CRITICAL_WARNING)
            force_critical = True
        else:
            defects = True

    spare, thr = m.get("available_spare"), m.get("available_spare_threshold")
    if spare is not None and thr:
        if spare < thr:
            cap = min(cap, CAP_NVME_SPARE_LOW)
            force_critical = True
            f.append(Finding("critical", f"Available spare {spare}% is below the {thr}% threshold", "available_spare"))
        elif spare < 100 and spare <= thr + 10:
            defects = True
            f.append(Finding("warning", f"Available spare {spare}% is close to the {thr}% threshold", "available_spare"))
        elif spare < 100:
            f.append(Finding("info", f"Available spare {spare}% (threshold {thr}%)", "available_spare"))

    hot_min = (m.get("warning_temp_minutes") or 0) + (m.get("critical_temp_minutes") or 0)
    if hot_min > 0:
        perf -= min(PERF_THROTTLE_MAX, 1 + hot_min / 60.0)
        f.append(Finding("info", f"Has spent {_n(hot_min, 'minute')} above its warning/critical temperature (throttling)", "warning_temp_minutes"))
    us = m.get("unsafe_shutdowns") or 0
    if us:
        f.append(Finding("info", f"{_n(us, 'unsafe shutdown')} (power lost without a clean shutdown); not a defect by itself", "unsafe_shutdowns"))
    if drive.power_on_hours and drive.power_on_hours >= 43800:
        f.append(Finding("info", f"Powered on {drive.power_on_hours:,} hours (~{drive.power_on_hours / 8766:.1f} years)", "power_on_hours"))

    score = max(0, min(cap, int(math.floor(round(h, 6)))))
    perf_i = max(0, min(100, int(round(perf))))

    status = "OK"
    if force_critical or score < CRITICAL_BELOW:
        status = "CRITICAL"
        if score < CRITICAL_BELOW and not force_critical and not any(x.severity == "critical" for x in f):
            f.append(Finding("critical", f"Health score {score}% is below {CRITICAL_BELOW}% - back up and replace", "health"))
    elif score < WARNING_BELOW or defects:
        status = "WARNING"
        if not any(x.severity in ("warning", "critical") for x in f):
            drivers = [x.message for x in f if x.metric in ("wear_percent_used", "available_spare")]
            f.append(Finding("warning", f"Health score {score}% is below {WARNING_BELOW}%"
                             + (" - mainly " + "; ".join(drivers) if drivers else ""), "health"))
    worst = max((RANK[{"critical": "CRITICAL", "warning": "WARNING"}.get(x.severity, "OK")] for x in f), default=0)
    status = max(status, {0: "OK", 1: "WARNING", 2: "CRITICAL"}[worst], key=lambda s: RANK[s])
    order = {"critical": 0, "warning": 1, "info": 2}
    f.sort(key=lambda x: order[x.severity])
    return Assessment(score, perf_i, status, f)
