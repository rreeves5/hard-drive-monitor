"""Terminal rendering (colored tables, detail view, history)."""
from __future__ import annotations

import os
import sys
from typing import Any, Dict, List, Optional, Tuple

from .health import Assessment
from .smart import Drive

RESET = "\033[0m"
COLORS = {"red": "\033[31m", "green": "\033[32m", "yellow": "\033[33m", "blue": "\033[34m",
          "cyan": "\033[36m", "dim": "\033[2m", "bold": "\033[1m", "bred": "\033[1;31m"}
STATUS_COLOR = {"OK": "green", "WARNING": "yellow", "CRITICAL": "bred", "UNKNOWN": "dim"}
SEV_COLOR = {"critical": "red", "warning": "yellow", "info": "dim"}
SPARK = "▁▂▃▄▅▆▇█"


class Painter:
    def __init__(self, mode: str = "auto", stream=None):
        stream = stream or sys.stdout
        if mode == "always":
            self.on = True
        elif mode == "never" or os.environ.get("NO_COLOR"):
            self.on = False
        else:
            self.on = hasattr(stream, "isatty") and stream.isatty()
        if self.on and sys.platform.startswith("win"):
            os.system("")  # enables VT escape processing in legacy Windows consoles

    def __call__(self, text: str, color: Optional[str]) -> str:
        if not self.on or not color:
            return text
        return COLORS[color] + text + RESET


def fmt_size(b: Optional[int]) -> str:
    if not b:
        return "-"
    for unit, f in (("TB", 1e12), ("GB", 1e9), ("MB", 1e6)):
        if b >= f:
            v = b / f
            return f"{v:.1f} {unit}" if v < 100 else f"{v:.0f} {unit}"
    return f"{b} B"


def fmt_hours(h: Optional[int]) -> str:
    if h is None:
        return "-"
    return f"{h:,} h" if h < 8766 else f"{h:,} h ({h / 8766:.1f}y)"


def pct_color(v: Optional[int]) -> Optional[str]:
    if v is None:
        return "dim"
    return "green" if v >= 70 else ("yellow" if v >= 30 else "red")


def bar(v: Optional[int], width: int = 10) -> str:
    if v is None:
        return " " * width
    n = int(round(v / 100 * width))
    return "█" * n + "░" * (width - n)


def sparkline(values: List[Optional[float]], lo: float = 0, hi: float = 100) -> str:
    out = []
    for v in values:
        if v is None:
            out.append(" ")
            continue
        r = 0 if hi == lo else (v - lo) / (hi - lo)
        out.append(SPARK[max(0, min(len(SPARK) - 1, int(round(r * (len(SPARK) - 1)))))])
    return "".join(out)


def table(headers: List[str], rows: List[List[Tuple[str, Optional[str]]]], paint: Painter, right=()) -> str:
    widths = [len(h) for h in headers]
    for r in rows:
        for i, (t, _) in enumerate(r):
            widths[i] = max(widths[i], len(t))
    def cell(i, t, c):
        padded = t.rjust(widths[i]) if i in right else t.ljust(widths[i])
        return paint(padded, c)
    lines = ["  ".join(paint(h.rjust(widths[i]) if i in right else h.ljust(widths[i]), "bold")
                       for i, h in enumerate(headers)).rstrip()]
    lines.append("  ".join("─" * w for w in widths))
    for r in rows:
        lines.append("  ".join(cell(i, t, c) for i, (t, c) in enumerate(r)).rstrip())
    return "\n".join(lines)


def device_label(d: Drive) -> str:
    return d.device + (f" ({' '.join(d.volumes[:3])}{'…' if len(d.volumes) > 3 else ''})" if d.volumes else "")


def status_view(results: List[Tuple[Drive, Assessment]], paint: Painter, meta: Optional[Dict[str, Any]] = None,
                changes: Optional[Dict[str, list]] = None) -> str:
    if not results:
        return "No drives found."
    results = sorted(results, key=lambda r: (r[0].device.startswith("PhysicalDrive"), r[0].device))
    headers = ["DEVICE", "MODEL", "SERIAL", "TYPE", "SIZE", "TEMP", "POWER-ON", "CYCLES", "HEALTH", "PERF", "STATUS"]
    rows = []
    for d, a in results:
        model = (d.model or "-")
        model = model if len(model) <= 26 else model[:25] + "…"
        temp_c = None
        for f in a.findings:
            if f.metric == "temperature":
                temp_c = SEV_COLOR[f.severity]
        rows.append([
            (device_label(d), "cyan"), (model, None), (d.serial or "-", "dim"), (d.kind, None),
            (fmt_size(d.capacity_bytes), None),
            (f"{d.temperature_c} C" if d.temperature_c is not None else "-", temp_c),
            (fmt_hours(d.power_on_hours), None),
            (f"{d.power_cycles:,}" if d.power_cycles is not None else "-", None),
            (f"{bar(a.health, 8)} {a.health:>3}%" if a.health is not None else "n/a", pct_color(a.health)),
            (f"{a.performance}%" if a.performance is not None else "n/a", pct_color(a.performance)),
            (a.status, STATUS_COLOR[a.status]),
        ])
    out = [table(headers, rows, paint, right={4, 5, 6, 7, 9})]
    details = []
    for d, a in results:
        items = [f for f in a.findings if f.severity != "info"] or (
            [f for f in a.findings if f.message.startswith("No SMART")] if a.status == "UNKNOWN" else [])
        ch = [c for c in (changes or {}).get(d.key, []) if c.get("alert")]
        if not items and not ch:
            continue
        details.append(paint(f"{d.short_name}", "cyan") + paint(f"  {a.status}", STATUS_COLOR[a.status]))
        for c in ch:
            details.append("   " + paint(f"▲ {c['metric']}: {c['old']} -> {c['new']} since last run", "red"))
        for f in items:
            mark = {"critical": "✖", "warning": "!", "info": "·"}[f.severity]
            details.append(f"   {paint(mark, SEV_COLOR[f.severity])} {f.message}")
    if details:
        out.append("")
        out.extend(details)
    counts = {s: sum(1 for _, a in results if a.status == s) for s in ("OK", "WARNING", "CRITICAL", "UNKNOWN")}
    summary = "  ".join(paint(f"{n} {s}", STATUS_COLOR[s]) for s, n in counts.items() if n)
    out.append("")
    out.append(f"{len(results)} drive(s): {summary}")
    if meta and meta.get("warning"):
        out.append(paint(meta["warning"], "yellow"))
    return "\n".join(out)


def detail_view(d: Drive, a: Assessment, paint: Painter) -> str:
    L = []
    L.append(paint(f"{d.device}  {d.model or ''}", "bold"))
    kv = [("Serial", d.serial), ("Firmware", d.firmware), ("Type", d.kind),
          ("Protocol", f"{d.protocol or '-'} (smartctl -d {d.dev_type})" if d.dev_type else d.protocol),
          ("Capacity", fmt_size(d.capacity_bytes)), ("Rotation", f"{d.rotation_rate} rpm" if d.rotation_rate else None),
          ("Form factor", d.form_factor), ("Interface", d.interface),
          ("Volumes", " ".join(d.volumes) if d.volumes else None),
          ("Temperature", f"{d.temperature_c} C" if d.temperature_c is not None else None),
          ("Power-on", fmt_hours(d.power_on_hours) if d.power_on_hours is not None else None),
          ("Power cycles", f"{d.power_cycles:,}" if d.power_cycles is not None else None),
          ("SMART verdict", {True: "PASSED", False: "FAILED", None: "-"}[d.smart_passed]),
          ("Source", d.source)]
    for k, v in kv:
        if v:
            L.append(f"  {k + ':':<15}{v}")
    L.append("")
    h = f"{a.health}%" if a.health is not None else "n/a"
    p = f"{a.performance}%" if a.performance is not None else "n/a"
    L.append(f"  Health      {paint(bar(a.health, 20), pct_color(a.health))} {paint(h, pct_color(a.health))}")
    L.append(f"  Performance {paint(bar(a.performance, 20), pct_color(a.performance))} {paint(p, pct_color(a.performance))}")
    L.append(f"  Status      {paint(a.status, STATUS_COLOR[a.status])}")
    if a.findings:
        L.append("")
        for f in a.findings:
            L.append("  " + paint(f.severity.upper().ljust(9), SEV_COLOR[f.severity]) + f.message)
    if d.ata_attributes:
        L.append("")
        L.append(paint("  ATA SMART attributes", "bold"))
        rows = []
        key_ids = {5, 187, 188, 197, 198, 199}
        for at in d.ata_attributes:
            raw_v = at.get("raw_value") or 0
            failing = at.get("when_failed")
            col = "red" if failing == "now" else ("yellow" if (at["id"] in key_ids and raw_v) or failing else None)
            rows.append([(str(at["id"]), col), (str(at.get("name") or ""), col), (str(at.get("value")), None),
                         (str(at.get("worst")), None), (str(at.get("thresh")), None),
                         (("Pre-fail" if at.get("prefailure") else "Old_age"), "dim"),
                         (failing or "-", "red" if failing else "dim"), (str(at.get("raw")), col)])
        L.append(_indent(table(["ID", "ATTRIBUTE", "VALUE", "WORST", "THRESH", "TYPE", "FAILED", "RAW"],
                               rows, paint, right={0, 2, 3, 4}), 2))
    if d.nvme_log:
        L.append("")
        L.append(paint("  NVMe SMART / health log", "bold"))
        hi = {"critical_warning", "media_errors", "percentage_used", "available_spare", "unsafe_shutdowns"}
        for k, v in d.nvme_log.items():
            if isinstance(v, (dict, list)):
                continue
            col = None
            if k in ("critical_warning", "media_errors") and v:
                col = "red"
            label = k + ("  *" if k in hi else "")
            L.append(f"    {label:<34}{paint(str(v), col)}")
    if d.windows_reliability:
        L.append("")
        L.append(paint("  Windows storage reliability counters", "bold"))
        for k, v in d.windows_reliability.items():
            L.append(f"    {k:<28}{v}")
    if d.self_test:
        st = d.self_test
        L.append("")
        L.append(paint("  Self-test", "bold"))
        L.append(f"    Current: {st.get('status') or '-'}" + (" (in progress, %s%% remaining)" % st.get("remaining_percent")
                                                              if st.get("in_progress") else ""))
        if d.self_test_log:
            for row in d.self_test_log[:5]:
                L.append(f"    {str(row.get('type')):<16} {paint(str(row.get('status')), 'green' if row.get('passed') else 'red')}"
                         f"  at {row.get('lifetime_hours')} h" + (f"  LBA {row['lba']}" if row.get("lba") not in (None, "-") else ""))
    if d.notes:
        L.append("")
        for n in d.notes:
            L.append(paint(f"  note: {n}", "dim"))
    return "\n".join(L)


def _indent(text: str, n: int) -> str:
    return "\n".join(" " * n + line for line in text.splitlines())


def local_ts(ts: Optional[str]) -> str:
    """Stored timestamps are UTC ISO-8601; show them in the local time zone."""
    if not ts:
        return "-"
    try:
        from datetime import datetime
        return datetime.fromisoformat(ts).astimezone().strftime("%Y-%m-%d %H:%M %Z")
    except ValueError:
        return ts


def history_view(summary: List[Dict[str, Any]], paint: Painter) -> str:
    if not summary:
        return "No history yet. Run `hdmon status` or `hdmon check` to record a run."
    L = []
    for drv in summary:
        rs = drv["readings"]
        last = rs[-1]
        L.append(paint(f"{drv['device']}  {drv['model'] or ''}  [{drv['serial'] or '-'}]", "bold"))
        L.append(f"  {drv['n']} readings, {local_ts(drv['first_ts'])} -> {local_ts(drv['last_ts'])}")
        hv = [r["health"] for r in rs]
        L.append(f"  Health   {paint(sparkline(hv), pct_color(last['health']))}  "
                 f"{' -> '.join(str(x) for x in hv[-4:] if x is not None) or 'n/a'}%  now {paint(last['status'], STATUS_COLOR.get(last['status']))}")
        temps = [r["temperature"] for r in rs]
        tv = [t for t in temps if t is not None]
        if tv:
            L.append(f"  Temp     {sparkline(temps, min(tv) - 1, max(tv) + 1)}  {min(tv)}-{max(tv)} C")
        if drv["changes_since_last"]:
            L.append(paint("  Worse since the previous run:", "red"))
            for c in drv["changes_since_last"]:
                L.append(f"    ▲ {c['metric']}: {c['old']} -> {c['new']}")
        flagged = [c for c in drv["changes_in_window"] if c["kind"] in ("status", "counter", "health")]
        if flagged:
            L.append("  Changes in this window:")
            for c in flagged[-10:]:
                L.append(f"    {local_ts(c['ts'])}  {c['metric']}: {c['old']} -> {c['new']}")
        elif not drv["changes_since_last"]:
            L.append(paint("  No attribute got worse in this window.", "green"))
        L.append("")
    return "\n".join(L).rstrip()
