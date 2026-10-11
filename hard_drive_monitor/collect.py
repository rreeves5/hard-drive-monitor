"""Gather drives from the live system (or JSON files) and score them."""
from __future__ import annotations

import re
from typing import Any, Dict, List, Optional, Tuple

from .health import Assessment, assess
from .paths import os_name
from .smart import (PS_LETTERS, PS_PHYSICAL, Drive, Smartctl, SmartctlMissing, discover, find_smartctl,
                    install_hint, load_json_files, parse_windows_physical, read_drive, run_powershell_json,
                    windows_disk_number, windows_letter_map)

Result = Tuple[Drive, Assessment]


def score(drives: List[Drive], cfg: Dict[str, Any]) -> List[Result]:
    return [(d, assess(d, cfg.get("thresholds"))) for d in drives]


def get_smartctl(cfg: Dict[str, Any]) -> Optional[Smartctl]:
    sc_cfg = cfg.get("smartctl") or {}
    path = find_smartctl(sc_cfg.get("path") or "")
    if not path:
        return None
    return Smartctl(path, sc_cfg.get("sudo", "auto"))


def apply_windows_letters(drives: List[Drive], inv: Dict[str, Any]) -> None:
    letters = windows_letter_map(inv)
    for d in drives:
        n = windows_disk_number(d.device, d.serial, inv)
        if n is None:
            continue
        d.notes.append(f"Windows disk number {n} (\\\\.\\PhysicalDrive{n})")
        if not d.volumes:
            d.volumes = letters.get(n, [])


def collect(cfg: Dict[str, Any], from_json: Optional[List[str]] = None) -> Tuple[List[Result], Dict[str, Any]]:
    meta: Dict[str, Any] = {"os": os_name()}
    ignore = set((cfg.get("ignore") or {}).get("devices") or [])
    if from_json:
        drives = load_json_files(from_json)
        meta["source"] = "json-files"
        return score([d for d in drives if d.device not in ignore], cfg), meta

    sc = get_smartctl(cfg)
    osn = os_name()
    if sc is None:
        if osn == "windows":
            inv = run_powershell_json(PS_PHYSICAL)
            drives = parse_windows_physical(inv)
            if drives:
                meta.update(source="windows-storage",
                            warning="smartctl not found - using Windows Storage counters. For full SMART data: "
                                    + install_hint())
                return score([d for d in drives if d.device not in ignore], cfg), meta
        raise SmartctlMissing("smartctl (smartmontools) not found. Install it with:\n  " + install_hint())
    ver = sc.version()
    meta.update(source="smartctl", smartctl=sc.path, smartctl_version=".".join(map(str, ver)) if ver else None)
    if ver and ver < (7, 0):
        raise SmartctlMissing(f"smartctl {ver[0]}.{ver[1]} is too old (JSON output needs 7.0+). Upgrade with:\n  "
                              + install_hint())
    refs = [r for r in discover(sc) if r.name not in ignore]
    drives = [read_drive(sc, r) for r in refs]
    if osn == "windows":
        apply_windows_letters(drives, run_powershell_json(PS_LETTERS))
    if sc.use_sudo:
        meta["sudo"] = True
    return score(drives, cfg), meta


def find_drive(results: List[Result], dev: str) -> Optional[Result]:
    want = dev.strip()
    wl = want.rstrip(":").upper() + ":"
    for d, a in results:
        if want in (d.device, d.short_name, d.serial) or (len(want) <= 2 and wl in [v.upper() for v in d.volumes]):
            return d, a
    for d, a in results:
        m = re.match(r"(?:/dev/)?(nvme\d+)n\d+$", want)
        if d.device.endswith("/" + want) or (m and d.short_name == m.group(1)):
            return d, a
    return None
