"""smartctl discovery + JSON parsing (all OSes) and the Windows PowerShell fallback."""
from __future__ import annotations

import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from typing import Any, Dict, List, Optional, Tuple

from .paths import os_name

EXTRA_PATHS = [
    "/usr/sbin", "/sbin", "/usr/local/sbin", "/usr/local/bin",
    "/opt/homebrew/bin", "/opt/homebrew/sbin", "/opt/local/sbin", "/opt/local/bin",
    r"C:\Program Files\smartmontools\bin", r"C:\Program Files (x86)\smartmontools\bin",
    r"C:\ProgramData\chocolatey\bin",
]

# Hide console windows for child processes when running from Task Scheduler (pythonw).
_CREATE_NO_WINDOW = 0x08000000 if sys.platform.startswith("win") else 0


class SmartctlMissing(RuntimeError):
    pass


def run_cmd(cmd: List[str], timeout: int = 120, env=None) -> subprocess.CompletedProcess:
    kw: Dict[str, Any] = {}
    if _CREATE_NO_WINDOW:
        kw["creationflags"] = _CREATE_NO_WINDOW
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          env=env, errors="replace", **kw)


def find_smartctl(configured: str = "") -> Optional[str]:
    if configured:
        return configured if os.path.exists(configured) else None
    p = shutil.which("smartctl")
    if p:
        return p
    for d in EXTRA_PATHS:
        for name in ("smartctl", "smartctl.exe"):
            c = os.path.join(d, name)
            if os.path.isfile(c) and os.access(c, os.X_OK):
                return c
    return None


def install_hint(platform: Optional[str] = None) -> str:
    osn = os_name(platform)
    if osn == "macos":
        return "brew install smartmontools"
    if osn == "windows":
        return "winget install smartmontools.smartmontools   (or: choco install smartmontools)"
    for mgr, cmd in (("apt-get", "sudo apt install smartmontools"),
                     ("dnf", "sudo dnf install smartmontools"),
                     ("yum", "sudo yum install smartmontools"),
                     ("pacman", "sudo pacman -S smartmontools"),
                     ("zypper", "sudo zypper install smartmontools"),
                     ("apk", "sudo apk add smartmontools")):
        if platform is None and shutil.which(mgr):
            return cmd
    return ("sudo apt install smartmontools  |  sudo dnf install smartmontools  |  "
            "sudo pacman -S smartmontools")


def parse_version(text: str) -> Optional[Tuple[int, int]]:
    m = re.search(r"smartctl\s+(\d+)\.(\d+)", text or "")
    return (int(m.group(1)), int(m.group(2))) if m else None


PERMISSION_RE = re.compile(r"permission denied|operation not permitted|requires? (root|admin)|"
                           r"access is denied|must be root", re.I)


class Smartctl:
    """Thin runner. sudo: "auto" | True | False (ignored on Windows)."""

    def __init__(self, path: str, sudo: Any = "auto"):
        self.path = path
        self.sudo_mode = sudo
        self.use_sudo = (sudo is True or str(sudo).lower() == "true") and self._can_sudo()
        self.permission_problem = False

    @staticmethod
    def _can_sudo() -> bool:
        return (not sys.platform.startswith("win") and hasattr(os, "geteuid")
                and os.geteuid() != 0 and shutil.which("sudo") is not None)

    def version(self) -> Optional[Tuple[int, int]]:
        try:
            return parse_version(run_cmd([self.path, "--version"], timeout=20).stdout)
        except (OSError, subprocess.SubprocessError):
            return None

    def _exec(self, args: List[str], sudo: bool, timeout: int):
        cmd = ([ "sudo", "-n"] if sudo else []) + [self.path] + args
        p = run_cmd(cmd, timeout=timeout)
        try:
            data = json.loads(p.stdout) if p.stdout.strip() else None
        except ValueError:
            data = None
        return p.returncode, data, p.stderr

    def run(self, args: List[str], timeout: int = 180):
        rc, data, err = self._exec(args, self.use_sudo, timeout)
        if (rc & 0x02) and not self.use_sudo and str(self.sudo_mode).lower() == "auto" \
                and self._can_sudo() and PERMISSION_RE.search(messages_text(data) + " " + err):
            rc2, data2, err2 = self._exec(args, True, timeout)
            if data2 is not None and not (rc2 & 0x02):
                self.use_sudo = True
                return rc2, data2, err2
        if (rc & 0x02) and PERMISSION_RE.search(messages_text(data) + " " + err):
            self.permission_problem = True
        return rc, data, err


def messages_text(data) -> str:
    if not isinstance(data, dict):
        return ""
    msgs = (data.get("smartctl") or {}).get("messages") or []
    return "; ".join(m.get("string", "") for m in msgs if isinstance(m, dict))


# ---------------------------------------------------------------- data model

@dataclass
class DeviceRef:
    name: str
    dev_type: Optional[str] = None
    protocol: Optional[str] = None
    note: Optional[str] = None
    source: str = "smartctl"
    model: Optional[str] = None
    serial: Optional[str] = None
    capacity_bytes: Optional[int] = None
    rotational: Optional[bool] = None
    transport: Optional[str] = None
    volumes: List[str] = field(default_factory=list)


@dataclass
class Drive:
    device: str
    dev_type: Optional[str] = None
    protocol: Optional[str] = None
    model: Optional[str] = None
    serial: Optional[str] = None
    firmware: Optional[str] = None
    capacity_bytes: Optional[int] = None
    kind: str = "Unknown"            # HDD | SSD | NVMe | Unknown
    rotation_rate: Optional[int] = None
    form_factor: Optional[str] = None
    interface: Optional[str] = None
    temperature_c: Optional[int] = None
    temp_limit_warn: Optional[int] = None
    temp_limit_crit: Optional[int] = None
    power_on_hours: Optional[int] = None
    power_cycles: Optional[int] = None
    smart_available: bool = True
    smart_passed: Optional[bool] = None
    metrics: Dict[str, Any] = field(default_factory=dict)
    ata_attributes: List[Dict[str, Any]] = field(default_factory=list)
    nvme_log: Dict[str, Any] = field(default_factory=dict)
    windows_reliability: Dict[str, Any] = field(default_factory=dict)
    self_test: Dict[str, Any] = field(default_factory=dict)
    self_test_log: List[Dict[str, Any]] = field(default_factory=list)
    volumes: List[str] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)
    source: str = "smartctl"

    @property
    def key(self) -> str:
        """Stable identity for history: serial when known, else model+device."""
        if self.serial:
            return f"{(self.model or '').strip()}|{self.serial.strip()}"
        return f"{(self.model or '').strip()}|{self.device}"

    @property
    def short_name(self) -> str:
        return self.device.rsplit("/", 1)[-1]

    def to_dict(self) -> Dict[str, Any]:
        d = asdict(self)
        d["key"] = self.key
        return d


# ---------------------------------------------------------------- smartctl JSON parsing

ATA_KEYS = {
    5: "reallocated_sectors",
    187: "reported_uncorrectable",
    188: "command_timeout",
    197: "pending_sectors",
    198: "offline_uncorrectable",
    199: "udma_crc_errors",
}
# Attributes whose NORMALIZED value counts down from 100 as the SSD wears.
WEAR_REMAINING_ATTRS = {
    231: "SSD_Life_Left", 233: "Media_Wearout_Indicator", 177: "Wear_Leveling_Count",
    202: "Percent_Lifetime_Remain", 169: "Remaining_Lifetime_Perc",
}


def _int(v) -> Optional[int]:
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def ata_raw_count(attr_id: int, raw: Dict[str, Any]) -> Optional[int]:
    """Raw counter value. Some vendors (notably Seagate on 188) pack several
    16-bit counters into the 48-bit raw value; use the low 16 bits there."""
    v = _int((raw or {}).get("value"))
    if v is None:
        return None
    if attr_id == 188 and v > 0xFFFF:
        return v & 0xFFFF
    if v > 0xFFFFFFFF:  # implausible counts are almost always packed fields
        return v & 0xFFFF
    return v


def _ata_wear(data: Dict[str, Any], attrs: Dict[int, Dict[str, Any]]):
    eu = (data.get("endurance_used") or {}).get("current_percent")
    if eu is not None:
        return _int(eu), "endurance_used"
    for page in (data.get("ata_device_statistics") or {}).get("pages", []) or []:
        for row in page.get("table", []) or []:
            if "Percentage Used Endurance" in str(row.get("name", "")) and row.get("value") is not None:
                return _int(row.get("value")), "device statistics"
    for aid in (231, 233, 202, 169, 177):
        a = attrs.get(aid)
        if a and _int(a.get("value")) is not None and 0 <= _int(a.get("value")) <= 100:
            return 100 - _int(a.get("value")), f"attribute {aid} {a.get('name', '')}"
    return None, None


def parse_smartctl(data: Dict[str, Any], device: Optional[str] = None) -> Drive:
    dev = data.get("device") or {}
    name = device or dev.get("name") or "?"
    protocol = dev.get("protocol")
    d = Drive(device=name, dev_type=dev.get("type"), protocol=protocol)
    d.model = data.get("model_name") or data.get("scsi_model_name") or data.get("product")
    d.serial = data.get("serial_number")
    d.firmware = data.get("firmware_version")
    cap = (data.get("user_capacity") or {}).get("bytes") or data.get("nvme_total_capacity")
    d.capacity_bytes = _int(cap)
    d.rotation_rate = _int(data.get("rotation_rate"))
    d.form_factor = (data.get("form_factor") or {}).get("name")
    d.interface = ((data.get("sata_version") or {}).get("string")
                   or ((data.get("interface_speed") or {}).get("current") or {}).get("string"))

    is_nvme = (protocol or "").upper() == "NVME" or "nvme_smart_health_information_log" in data
    if is_nvme:
        d.kind = "NVMe"
    elif d.rotation_rate is not None:
        d.kind = "SSD" if d.rotation_rate == 0 else "HDD"

    temp = data.get("temperature") or {}
    d.temperature_c = _int(temp.get("current"))
    d.temp_limit_warn = _int(temp.get("op_limit_max"))
    d.temp_limit_crit = _int(temp.get("critical_limit_max"))
    d.power_on_hours = _int((data.get("power_on_time") or {}).get("hours"))
    d.power_cycles = _int(data.get("power_cycle_count"))

    support = data.get("smart_support") or {}
    if support.get("available") is False:
        d.smart_available = False
        d.notes.append("Device does not support SMART (common for USB enclosures and virtual disks)")
    ss = data.get("smart_status")
    if isinstance(ss, dict) and "passed" in ss:
        d.smart_passed = bool(ss["passed"])
    elif not is_nvme and "ata_smart_attributes" not in data:
        d.smart_available = d.smart_available and bool(ss)

    m: Dict[str, Any] = {}
    if is_nvme:
        log = data.get("nvme_smart_health_information_log") or {}
        d.nvme_log = dict(log)
        m["wear_percent_used"] = _int(log.get("percentage_used"))
        m["media_errors"] = _int(log.get("media_errors"))
        m["critical_warning"] = _int(log.get("critical_warning"))
        m["available_spare"] = _int(log.get("available_spare"))
        m["available_spare_threshold"] = _int(log.get("available_spare_threshold"))
        m["unsafe_shutdowns"] = _int(log.get("unsafe_shutdowns"))
        m["error_log_entries"] = _int(log.get("num_err_log_entries"))
        m["warning_temp_minutes"] = _int(log.get("warning_temp_time"))
        m["critical_temp_minutes"] = _int(log.get("critical_comp_time"))
        duw = _int(log.get("data_units_written"))
        if duw is not None:
            m["data_written_tb"] = round(duw * 512000 / 1e12, 2)
        if d.temperature_c is None:
            d.temperature_c = _int(log.get("temperature"))
        if d.power_on_hours is None:
            d.power_on_hours = _int(log.get("power_on_hours"))
        if d.power_cycles is None:
            d.power_cycles = _int(log.get("power_cycles"))
        if not log:
            d.smart_available = False
        st = data.get("nvme_self_test_log") or {}
        cur = st.get("current_self_test_operation") or {}
        d.self_test = {
            "in_progress": bool(_int(cur.get("value"))),
            "status": cur.get("string"),
            "remaining_percent": (100 - _int(st.get("current_self_test_completion_percent")))
            if _int(cur.get("value")) and st.get("current_self_test_completion_percent") is not None else None,
            "short_minutes": 2,     # NVMe spec: short test completes within 2 minutes
            "extended_minutes": None,
        }
        for row in st.get("table", []) or []:
            res = row.get("self_test_result") or {}
            d.self_test_log.append({
                "type": (row.get("self_test_code") or {}).get("string"),
                "status": res.get("string"),
                "passed": _int(res.get("value")) == 0,
                "lifetime_hours": _int(row.get("power_on_hours")),
                "lba": row.get("lba"),
            })
    else:
        table = (data.get("ata_smart_attributes") or {}).get("table") or []
        attrs = {}
        for a in table:
            aid = _int(a.get("id"))
            if aid is None:
                continue
            attrs[aid] = a
            d.ata_attributes.append({
                "id": aid, "name": a.get("name"), "value": a.get("value"), "worst": a.get("worst"),
                "thresh": a.get("thresh"), "when_failed": a.get("when_failed") or "",
                "raw": (a.get("raw") or {}).get("string", (a.get("raw") or {}).get("value")),
                "raw_value": (a.get("raw") or {}).get("value"),
                "prefailure": bool((a.get("flags") or {}).get("prefailure")),
            })
        for aid, key in ATA_KEYS.items():
            if aid in attrs:
                m[key] = ata_raw_count(aid, attrs[aid].get("raw") or {})
        if d.temperature_c is None:
            for aid in (194, 190):
                if aid in attrs:
                    v = _int((attrs[aid].get("raw") or {}).get("value"))
                    if v is not None:
                        d.temperature_c = v & 0xFF
                        break
        if d.power_on_hours is None and 9 in attrs:
            d.power_on_hours = _int((attrs[9].get("raw") or {}).get("value"))
        if d.power_cycles is None and 12 in attrs:
            d.power_cycles = _int((attrs[12].get("raw") or {}).get("value"))
        if d.kind != "HDD":
            wear, src = _ata_wear(data, attrs)
            if wear is not None:
                m["wear_percent_used"] = wear
                m["wear_source"] = src
                if d.kind == "Unknown":
                    d.kind = "SSD"
        m["failing_now"] = [f"{a['id']} {a['name']}" for a in d.ata_attributes if a["when_failed"] == "now"]
        m["failed_past"] = [f"{a['id']} {a['name']}" for a in d.ata_attributes if a["when_failed"] == "past"]
        sd = (data.get("ata_smart_data") or {}).get("self_test") or {}
        status = sd.get("status") or {}
        val = _int(status.get("value")) or 0
        pm = sd.get("polling_minutes") or {}
        d.self_test = {
            "in_progress": (val >> 4) == 0xF,
            "status": status.get("string"),
            "remaining_percent": _int(status.get("remaining_percent")),
            "short_minutes": _int(pm.get("short")),
            "extended_minutes": _int(pm.get("extended")),
            "conveyance_minutes": _int(pm.get("conveyance")),
        }
        log = ((data.get("ata_smart_self_test_log") or {}).get("standard") or {}).get("table") or []
        for row in log:
            st = row.get("status") or {}
            d.self_test_log.append({
                "type": (row.get("type") or {}).get("string"),
                "status": st.get("string"),
                "passed": bool(st.get("passed", _int(st.get("value")) == 0)),
                "lifetime_hours": _int(row.get("lifetime_hours")),
                "lba": row.get("lba"),
            })
        if not table and d.smart_passed is None:
            d.smart_available = False
    d.metrics = {k: v for k, v in m.items() if v is not None and v != []}
    return d


# ---------------------------------------------------------------- discovery

def _lsblk() -> List[Dict[str, Any]]:
    if not shutil.which("lsblk"):
        return []
    try:
        p = run_cmd(["lsblk", "-J", "-b", "-o", "NAME,TYPE,TRAN,ROTA,SIZE,MODEL,SERIAL,MOUNTPOINT"], 20)
        return json.loads(p.stdout).get("blockdevices", []) if p.returncode == 0 else []
    except (OSError, ValueError, subprocess.SubprocessError):
        return []


def _lsblk_mounts(bd: Dict[str, Any]) -> List[str]:
    out = []
    for c in bd.get("children", []) or []:
        if c.get("mountpoint"):
            out.append(c["mountpoint"])
        out.extend(_lsblk_mounts(c))
    return out


def _nvme_ctrl(name: str) -> str:
    """/dev/nvme0n1 -> /dev/nvme0 (smartctl reports controllers)."""
    m = re.match(r"(/dev/nvme\d+)n\d+$", name)
    return m.group(1) if m else name


SKIP_LINUX = re.compile(r"^(loop|zram|ram|sr|fd|md|dm-|nbd)")


def discover_linux(sc: Optional[Smartctl]) -> List[DeviceRef]:
    refs: List[DeviceRef] = []
    seen = set()
    if sc:
        _, data, _ = sc.run(["--scan-open", "--json"], timeout=60)
        for dv in (data or {}).get("devices", []) or []:
            r = DeviceRef(dv.get("name"), dv.get("type"), dv.get("protocol"), dv.get("open_error"))
            refs.append(r)
            seen.add(_nvme_ctrl(r.name))
    for bd in _lsblk():
        if bd.get("type") != "disk" or SKIP_LINUX.match(bd.get("name", "")):
            continue
        path = "/dev/" + bd["name"]
        info = dict(model=(bd.get("model") or "").strip() or None, serial=bd.get("serial"),
                    capacity_bytes=_int(bd.get("size")), rotational=bool(bd.get("rota")) if bd.get("rota") is not None else None,
                    transport=bd.get("tran"), volumes=_lsblk_mounts(bd))
        match = next((r for r in refs if r.name in (path, _nvme_ctrl(path))), None)
        if match:
            for k, v in info.items():
                if getattr(match, k) in (None, []):
                    setattr(match, k, v)
        else:
            refs.append(DeviceRef(path, None, None, source="lsblk", **info))
    return refs


def _diskutil_plist(args: List[str]) -> Dict[str, Any]:
    try:
        p = subprocess.run(["diskutil"] + args, capture_output=True, timeout=30)
        return plistlib.loads(p.stdout) if p.returncode == 0 and p.stdout else {}
    except (OSError, ValueError, subprocess.SubprocessError, plistlib.InvalidFileException):
        return {}


def discover_macos(sc: Optional[Smartctl]) -> List[DeviceRef]:
    refs: List[DeviceRef] = []
    if sc:
        _, data, _ = sc.run(["--scan", "--json"], timeout=60)
        for dv in (data or {}).get("devices", []) or []:
            refs.append(DeviceRef(dv.get("name"), dv.get("type"), dv.get("protocol")))
    listing = _diskutil_plist(["list", "-plist", "physical"])
    vols: Dict[str, List[str]] = {}
    for entry in listing.get("AllDisksAndPartitions", []) or []:
        vols[entry.get("DeviceIdentifier", "")] = [p.get("VolumeName") for p in entry.get("Partitions", []) or []
                                                   if p.get("VolumeName")]
    for ident in listing.get("WholeDisks", []) or []:
        path = "/dev/" + ident
        info = _diskutil_plist(["info", "-plist", ident])
        extra = dict(model=info.get("MediaName") or info.get("IORegistryEntryName"),
                     capacity_bytes=_int(info.get("TotalSize") or info.get("Size")),
                     rotational=(not info["SolidState"]) if "SolidState" in info else None,
                     transport=info.get("BusProtocol"), volumes=vols.get(ident, []))
        match = next((r for r in refs if r.name == path), None)
        if match:
            for k, v in extra.items():
                if getattr(match, k) in (None, []):
                    setattr(match, k, v)
        else:
            refs.append(DeviceRef(path, None, None, source="diskutil", **extra))
    return refs


# ---------------------------------------------------------------- Windows

PS_LETTERS = r"""
$ErrorActionPreference = 'SilentlyContinue'
$parts = @(Get-Partition | Where-Object { $_.DriveLetter } | ForEach-Object {
  [pscustomobject]@{ DiskNumber = $_.DiskNumber; DriveLetter = [string]$_.DriveLetter } })
$disks = @(Get-Disk | ForEach-Object {
  [pscustomobject]@{ Number = $_.Number; SerialNumber = $_.SerialNumber; FriendlyName = $_.FriendlyName;
                     BusType = [string]$_.BusType; Size = $_.Size } })
[pscustomobject]@{ Partitions = $parts; Disks = $disks } | ConvertTo-Json -Depth 5 -Compress
"""

PS_PHYSICAL = r"""
$ErrorActionPreference = 'SilentlyContinue'
$pd = @(Get-PhysicalDisk | ForEach-Object {
  $r = $_ | Get-StorageReliabilityCounter
  $rel = $null
  if ($r) { $rel = [pscustomobject]@{
      Temperature = $r.Temperature; TemperatureMax = $r.TemperatureMax; Wear = $r.Wear;
      PowerOnHours = $r.PowerOnHours; StartStopCycleCount = $r.StartStopCycleCount;
      ReadErrorsTotal = $r.ReadErrorsTotal; ReadErrorsCorrected = $r.ReadErrorsCorrected;
      ReadErrorsUncorrected = $r.ReadErrorsUncorrected; WriteErrorsTotal = $r.WriteErrorsTotal;
      WriteErrorsCorrected = $r.WriteErrorsCorrected; WriteErrorsUncorrected = $r.WriteErrorsUncorrected;
      ReadLatencyMax = $r.ReadLatencyMax; WriteLatencyMax = $r.WriteLatencyMax } }
  [pscustomobject]@{ DeviceId = $_.DeviceId; FriendlyName = $_.FriendlyName; SerialNumber = $_.SerialNumber;
    MediaType = [string]$_.MediaType; BusType = [string]$_.BusType; Size = $_.Size;
    HealthStatus = [string]$_.HealthStatus; OperationalStatus = ([string[]]$_.OperationalStatus) -join ', ';
    FirmwareVersion = $_.FirmwareVersion; Reliability = $rel } })
$parts = @(Get-Partition | Where-Object { $_.DriveLetter } | ForEach-Object {
  [pscustomobject]@{ DiskNumber = $_.DiskNumber; DriveLetter = [string]$_.DriveLetter } })
$disks = @(Get-Disk | ForEach-Object {
  [pscustomobject]@{ Number = $_.Number; SerialNumber = $_.SerialNumber; FriendlyName = $_.FriendlyName;
                     BusType = [string]$_.BusType; Size = $_.Size } })
[pscustomobject]@{ PhysicalDisks = $pd; Partitions = $parts; Disks = $disks } | ConvertTo-Json -Depth 6 -Compress
"""


def powershell_exe() -> Optional[str]:
    return shutil.which("powershell") or shutil.which("powershell.exe") or shutil.which("pwsh")


def run_powershell_json(script: str, timeout: int = 90) -> Dict[str, Any]:
    ps = powershell_exe()
    if not ps:
        return {}
    try:
        p = run_cmd([ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                     "-Command", script], timeout)
        return json.loads(p.stdout) if p.stdout.strip() else {}
    except (OSError, ValueError, subprocess.SubprocessError):
        return {}


def _as_list(v) -> List[Any]:
    if v is None:
        return []
    return v if isinstance(v, list) else [v]   # ConvertTo-Json unwraps 1-element arrays


def norm_serial(s: Optional[str]) -> str:
    return re.sub(r"[^0-9A-Z]", "", (s or "").upper())


def windows_letter_map(inv: Dict[str, Any]) -> Dict[int, List[str]]:
    """{disk number: ["C:", "D:"]} from Get-Partition output."""
    out: Dict[int, List[str]] = {}
    for p in _as_list(inv.get("Partitions")):
        n, letter = _int(p.get("DiskNumber")), (p.get("DriveLetter") or "").strip()
        if n is None or not letter:
            continue
        out.setdefault(n, []).append(letter.rstrip(":") + ":")
    for v in out.values():
        v.sort()
    return out


def windows_disk_number(device: str, serial: Optional[str], inv: Dict[str, Any]) -> Optional[int]:
    """Map a smartctl device to a Windows disk number: serial match first, then
    smartctl's naming (/dev/sda = PhysicalDrive0, /dev/pd3 = PhysicalDrive3)."""
    ns = norm_serial(serial)
    if ns:
        for d in _as_list(inv.get("Disks")) + _as_list(inv.get("PhysicalDisks")):
            ds = norm_serial(d.get("SerialNumber"))
            if ds and (ds == ns or ds.endswith(ns) or ns.endswith(ds)):
                n = _int(d.get("Number", d.get("DeviceId")))
                if n is not None:
                    return n
    m = re.match(r"/dev/pd(\d+)$", device or "")
    if m:
        return int(m.group(1))
    m = re.match(r"/dev/sd([a-z]+)$", device or "")
    if m:
        n = 0
        for ch in m.group(1):
            n = n * 26 + (ord(ch) - 96)
        return n - 1
    return None


def parse_windows_physical(inv: Dict[str, Any]) -> List[Drive]:
    """Fallback when smartctl is absent: Get-PhysicalDisk + Get-StorageReliabilityCounter."""
    letters = windows_letter_map(inv)
    drives = []
    for pd in _as_list(inv.get("PhysicalDisks")):
        num = _int(pd.get("DeviceId"))
        bus = (pd.get("BusType") or "").strip()
        media = (pd.get("MediaType") or "").strip()
        kind = "NVMe" if bus.upper() == "NVME" else ("HDD" if media == "HDD" else ("SSD" if media == "SSD" else "Unknown"))
        d = Drive(device=f"PhysicalDrive{num}" if num is not None else (pd.get("FriendlyName") or "?"),
                  protocol=bus or None, model=(pd.get("FriendlyName") or "").strip() or None,
                  serial=(pd.get("SerialNumber") or "").strip() or None, firmware=pd.get("FirmwareVersion"),
                  capacity_bytes=_int(pd.get("Size")), kind=kind, interface=bus or None,
                  source="windows-storage")
        d.volumes = letters.get(num, []) if num is not None else []
        hs = (pd.get("HealthStatus") or "").strip()
        d.smart_passed = {"Healthy": True, "Unhealthy": False}.get(hs)
        m: Dict[str, Any] = {"windows_health_status": hs or None,
                             "windows_operational_status": pd.get("OperationalStatus") or None}
        rel = pd.get("Reliability") or {}
        d.windows_reliability = dict(rel)
        if rel:
            d.temperature_c = _int(rel.get("Temperature")) or None   # 0 = not reported
            d.power_on_hours = _int(rel.get("PowerOnHours"))
            d.power_cycles = _int(rel.get("StartStopCycleCount"))
            if kind != "HDD" and rel.get("Wear") is not None:
                m["wear_percent_used"] = _int(rel.get("Wear"))
                m["wear_source"] = "Get-StorageReliabilityCounter"
            ru, wu = _int(rel.get("ReadErrorsUncorrected")), _int(rel.get("WriteErrorsUncorrected"))
            if ru is not None or wu is not None:
                m["uncorrected_errors"] = (ru or 0) + (wu or 0)
        else:
            d.notes.append("Reliability counters unavailable (run as Administrator, or the driver does not expose them)")
        if not rel and d.smart_passed is None:
            d.smart_available = False
        d.notes.append("Windows Storage fallback: fewer attributes than smartctl. "
                       "Install smartmontools for the full SMART view.")
        d.metrics = {k: v for k, v in m.items() if v is not None}
        drives.append(d)
    return drives


def discover_windows(sc: Optional[Smartctl]) -> List[DeviceRef]:
    refs: List[DeviceRef] = []
    if sc:
        _, data, _ = sc.run(["--scan", "--json"], timeout=60)
        for dv in (data or {}).get("devices", []) or []:
            refs.append(DeviceRef(dv.get("name"), dv.get("type"), dv.get("protocol")))
    return refs


def discover(sc: Optional[Smartctl]) -> List[DeviceRef]:
    osn = os_name()
    if osn == "macos":
        return discover_macos(sc)
    if osn == "windows":
        return discover_windows(sc)
    return discover_linux(sc)


def read_drive(sc: Smartctl, ref: DeviceRef) -> Drive:
    args = ["-x", "--json"] + (["-d", ref.dev_type] if ref.dev_type else []) + [ref.name]
    rc, data, err = sc.run(args)
    if not isinstance(data, dict) or (rc & 0x03) or "device" not in data:
        d = Drive(device=ref.name, dev_type=ref.dev_type, protocol=ref.protocol or ref.transport,
                  model=ref.model, serial=ref.serial, capacity_bytes=ref.capacity_bytes,
                  smart_available=False, source=ref.source)
        if ref.rotational is not None:
            d.kind = "HDD" if ref.rotational else "SSD"
        msg = messages_text(data) or (err or "").strip() or ref.note or "smartctl could not read this device"
        d.notes.append(msg)
        if sc.permission_problem:
            d.notes.append("Needs elevated rights: " + elevation_hint())
        d.volumes = list(ref.volumes)
        return d
    d = parse_smartctl(data, device=ref.name)
    if not d.volumes:
        d.volumes = list(ref.volumes)
    if (rc & 0x04) and messages_text(data):
        d.notes.append("smartctl: " + messages_text(data))
    return d


def elevation_hint() -> str:
    osn = os_name()
    if osn == "windows":
        return "run hdmon from an elevated (Run as Administrator) terminal"
    return ("run with sudo, or add a sudoers rule for smartctl and set sudo = \"auto\" in config.toml "
            "(see README)")


def load_json_files(paths: List[str]) -> List[Drive]:
    """--from-json: smartctl -x --json dumps (or Windows inventory dumps)."""
    files: List[str] = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith(".json"))
        else:
            files.append(p)
    drives: List[Drive] = []
    for f in files:
        with open(f, encoding="utf-8") as fh:
            data = json.load(fh)
        if isinstance(data, dict) and "PhysicalDisks" in data:
            for d in parse_windows_physical(data):
                d.source = f"file:{os.path.basename(f)}"
                drives.append(d)
            continue
        d = parse_smartctl(data)
        d.source = f"file:{os.path.basename(f)}"
        if isinstance(data, dict) and data.get("_volumes"):
            d.volumes = list(data["_volumes"])
        drives.append(d)
    return drives
