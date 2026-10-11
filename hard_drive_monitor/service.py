"""Scheduling: systemd user timer (Linux), launchd agent (macOS), Task Scheduler (Windows)."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

from .paths import os_name

LABEL = "com.hard-drive-monitor.check"
TASK_NAME = "hard-drive-monitor"


def parse_interval(text: str) -> int:
    """'1h', '30m', '90s', '2h30m', '3600' -> seconds."""
    t = str(text).strip().lower()
    if t.isdigit():
        return int(t)
    parts = re.findall(r"(\d+(?:\.\d+)?)\s*([dhms])", t)
    if not parts or "".join(n + u for n, u in parts) != re.sub(r"\s+", "", t):
        raise ValueError(f"bad interval {text!r} (use e.g. 1h, 30m, 90s, 2h30m)")
    mult = {"d": 86400, "h": 3600, "m": 60, "s": 1}
    secs = int(sum(float(n) * mult[u] for n, u in parts))
    if secs <= 0:
        raise ValueError("interval must be positive")
    return secs


def _pkg_parent() -> str:
    return str(Path(__file__).resolve().parent.parent)


def program_args(platform: Optional[str] = None) -> Dict[str, object]:
    """How the scheduler should start hdmon: {'argv': [...], 'env': {...}}."""
    osn = os_name(platform)
    if osn == "windows":
        exe_dir = Path(sys.executable).parent
        pyw = exe_dir / "pythonw.exe"
        return {"argv": [str(pyw if pyw.exists() else sys.executable), "-m", "hard_drive_monitor"], "env": {}}
    me = sys.argv[0] if sys.argv else ""
    if os.path.basename(me) == "hdmon" and os.path.isfile(me):
        return {"argv": [os.path.abspath(me)], "env": {}}
    exe = shutil.which("hdmon")
    if exe:
        return {"argv": [exe], "env": {}}
    env = {}
    if (Path(_pkg_parent()) / "pyproject.toml").exists():   # running from a source checkout
        env["PYTHONPATH"] = _pkg_parent()
    return {"argv": [sys.executable, "-m", "hard_drive_monitor"], "env": env}


def _quote(a: str) -> str:
    return '"' + a.replace('"', '\\"') + '"' if (" " in a or '"' in a) else a


# ---------------------------------------------------------------- Linux

def systemd_units(argv: List[str], env: Dict[str, str], interval_s: int, system: bool = False) -> Dict[str, str]:
    exec_line = " ".join(_quote(a) for a in argv + ["check", "--quiet"])
    env_lines = "".join(f'Environment="{k}={v}"\n' for k, v in env.items())
    service = (
        "[Unit]\n"
        "Description=Hard Drive Monitor SMART drive health check\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        f"{env_lines}"
        f"ExecStart={exec_line}\n"
        "Nice=10\n"
        "IOSchedulingClass=idle\n")
    timer = (
        "[Unit]\n"
        "Description=Run Hard Drive Monitor (hdmon) periodically\n\n"
        "[Timer]\n"
        "OnStartupSec=2min\n"
        f"OnUnitActiveSec={interval_s}s\n"
        "Persistent=true\n"
        "RandomizedDelaySec=60\n\n"
        "[Install]\n"
        "WantedBy=timers.target\n")
    return {"hard-drive-monitor.service": service, "hard-drive-monitor.timer": timer}


# ---------------------------------------------------------------- macOS

def launchd_plist(argv: List[str], env: Dict[str, str], interval_s: int, log_dir: str) -> str:
    import plistlib
    envd = {"PATH": "/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin"}
    envd.update(env)
    data = {
        "Label": LABEL,
        "ProgramArguments": argv + ["check", "--quiet"],
        "StartInterval": interval_s,
        "RunAtLoad": True,
        "ProcessType": "Background",
        "LowPriorityIO": True,
        "EnvironmentVariables": envd,
        "StandardOutPath": os.path.join(log_dir, "hard-drive-monitor.log"),
        "StandardErrorPath": os.path.join(log_dir, "hard-drive-monitor.log"),
    }
    return plistlib.dumps(data).decode()


# ---------------------------------------------------------------- Windows

def is_windows_admin() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())  # type: ignore[attr-defined]
    except Exception:
        return False


def _xml_escape(t: str) -> str:
    return (t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;"))


def windows_task_xml(argv: List[str], interval_s: int, highest: bool = False, user: str = "") -> str:
    """Task Scheduler definition: at logon (2 min delay) + every interval, highest privileges if requested."""
    minutes = max(1, interval_s // 60)
    period = f"PT{minutes // 60}H" if minutes % 60 == 0 else f"PT{minutes}M"
    args = " ".join(f'"{a}"' if " " in a else a for a in argv[1:] + ["check", "--quiet"])
    user_xml = f"<UserId>{_xml_escape(user)}</UserId>" if user else ""
    return f"""<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Hard Drive Monitor: hourly SMART health check (hdmon check). Remove with: hdmon uninstall-service</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
      {user_xml}
      <Delay>PT2M</Delay>
    </LogonTrigger>
    <TimeTrigger>
      <Enabled>true</Enabled>
      <StartBoundary>2026-01-01T00:00:00</StartBoundary>
      <Repetition>
        <Interval>{period}</Interval>
        <StopAtDurationEnd>false</StopAtDurationEnd>
      </Repetition>
    </TimeTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      {user_xml}
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>{"HighestAvailable" if highest else "LeastPrivilege"}</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <StartWhenAvailable>true</StartWhenAvailable>
    <ExecutionTimeLimit>PT30M</ExecutionTimeLimit>
    <Priority>7</Priority>
    <Enabled>true</Enabled>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{_xml_escape(argv[0])}</Command>
      <Arguments>{_xml_escape(args)}</Arguments>
    </Exec>
  </Actions>
</Task>
"""


CRON_MARK = "# hard-drive-monitor"


def cron_lines(argv: List[str], env: Dict[str, str], interval_s: int) -> List[str]:
    cmd = " ".join(_quote(a) for a in argv + ["check", "--quiet"])
    envs = "".join(f"{k}={_quote(v)} " for k, v in env.items())
    minutes = max(1, interval_s // 60)
    if minutes % 60 == 0:
        hours = minutes // 60
        when = "7 * * * *" if hours == 1 else f"7 */{hours} * * *"
    else:
        when = f"*/{min(minutes, 59)} * * * *"
    return [f"@reboot sleep 120; {envs}{cmd} {CRON_MARK}", f"{when} {envs}{cmd} {CRON_MARK}"]


def _systemd_user_ok() -> bool:
    if not shutil.which("systemctl"):
        return False
    try:
        p = subprocess.run(["systemctl", "--user", "show-environment"], capture_output=True, text=True, timeout=15)
        return p.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _crontab_read() -> Optional[List[str]]:
    if not shutil.which("crontab"):
        return None
    p = subprocess.run(["crontab", "-l"], capture_output=True, text=True)
    return p.stdout.splitlines() if p.returncode == 0 else []


def _crontab_write(lines: List[str]) -> bool:
    p = subprocess.run(["crontab", "-"], input=("\n".join(lines) + "\n") if lines else "", capture_output=True, text=True)
    return p.returncode == 0


def _run(cmd: List[str], out) -> bool:
    out(f"  $ {' '.join(cmd)}")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as e:
        out(f"    failed: {e}")
        return False
    if p.stdout.strip():
        out("    " + p.stdout.strip().replace("\n", "\n    "))
    if p.returncode != 0:
        out(f"    failed (exit {p.returncode}): {p.stderr.strip()}")
    return p.returncode == 0


def install(interval_s: int = 3600, enable: bool = True, system: bool = False, highest: bool = False,
            dry_run: bool = False, out=print, platform: Optional[str] = None) -> int:
    osn = os_name(platform)
    prog = program_args(platform)
    argv, env = prog["argv"], prog["env"]
    if osn == "linux":
        units = systemd_units(argv, env, interval_s, system)
        if system:
            out("System-wide (root) units are NOT installed automatically. To install them yourself:\n")
            for name, text in units.items():
                out(f"# /etc/systemd/system/{name}\n{text}")
            out("Then run:\n  sudo cp hard-drive-monitor.service hard-drive-monitor.timer /etc/systemd/system/\n"
                "  sudo systemctl daemon-reload && sudo systemctl enable --now hard-drive-monitor.timer\n"
                "Note: a root service writes history to /root/.local/share/hard-drive-monitor and cannot show desktop\n"
                "notifications in your session; configure e-mail alerts for it.")
            return 0
        if enable and not dry_run and not _systemd_user_ok():
            cur = _crontab_read()
            if cur is None:
                out("No systemd user session and no crontab found, so nothing was scheduled.\n"
                    "Run `hdmon watch --interval 1h` from your session startup instead, or schedule\n"
                    "`" + " ".join(argv) + " check --quiet` with your system's scheduler.")
                return 1
            lines = [l for l in cur if CRON_MARK not in l] + cron_lines(argv, env, interval_s)
            out("No systemd user session found; using your crontab instead:")
            for l in cron_lines(argv, env, interval_s):
                out("  " + l)
            if not _crontab_write(lines):
                out("  failed to write crontab")
                return 1
            out("Scheduled via cron (at boot + every interval). Turn off with: hdmon uninstall-service")
            return 0
        d = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "systemd" / "user"
        for name, text in units.items():
            out(f"--- {d / name} ---\n{text}")
            if not dry_run:
                d.mkdir(parents=True, exist_ok=True)
                (d / name).write_text(text)
        out(("Wrote" if not dry_run else "Would write") + f" {d / 'hard-drive-monitor.service'} and {d / 'hard-drive-monitor.timer'}")
        cmds = [["systemctl", "--user", "daemon-reload"], ["systemctl", "--user", "enable", "--now", "hard-drive-monitor.timer"]]
        if enable and not dry_run:
            ok = all(_run(c, out) for c in cmds)
            if ok:
                out("Scheduled: hourly + 2 min after you log in (systemd user timer, no root).\n"
                    "  Check:    systemctl --user list-timers hard-drive-monitor.timer\n"
                    "  Turn off: hdmon uninstall-service   (or: systemctl --user disable --now hard-drive-monitor.timer)\n"
                    "  User timers only run while you are logged in; to run them without a login:\n"
                    "            sudo loginctl enable-linger $USER")
            else:
                return 1
        else:
            out("To enable:\n  " + "\n  ".join(" ".join(c) for c in cmds))
        out("smartctl normally needs root: add the sudoers rule from README.md (sudo = \"auto\" in config.toml).")
        return 0
    if osn == "macos":
        log_dir = str(Path.home() / "Library" / "Logs")
        text = launchd_plist(argv, env, interval_s, log_dir)
        if system:
            out(f"System-wide LaunchDaemon is NOT installed automatically. Save this as /Library/LaunchDaemons/{LABEL}.plist:\n")
            out(text)
            out(f"Then: sudo chown root:wheel /Library/LaunchDaemons/{LABEL}.plist && "
                f"sudo launchctl bootstrap system /Library/LaunchDaemons/{LABEL}.plist")
            return 0
        path = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
        out(f"--- {path} ---\n{text}")
        if not dry_run:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        out(("Wrote " if not dry_run else "Would write ") + str(path))
        uid = os.getuid() if hasattr(os, "getuid") else 0
        cmd = ["launchctl", "bootstrap", f"gui/{uid}", str(path)]
        if enable and not dry_run:
            _run(["launchctl", "bootout", f"gui/{uid}/{LABEL}"], lambda *_: None)  # reload if already there
            if not _run(cmd, out) and not _run(["launchctl", "load", "-w", str(path)], out):
                return 1
            out("Scheduled: at login + hourly (launchd agent, runs as you).\n"
                f"  Check:    launchctl print gui/{uid}/{LABEL}\n"
                "  Turn off: hdmon uninstall-service")
        else:
            out("To enable:\n  " + " ".join(cmd))
        out(f"Log: {log_dir}/hard-drive-monitor.log. Internal Apple SSDs usually work without sudo; "
            "for other disks add the sudoers rule from README.md.")
        return 0
    # windows: one task, two triggers (at logon + every interval), via XML
    if not highest and (platform is None) and is_windows_admin():
        highest = True   # elevated terminal: give the task highest privileges so smartctl can read drives
    uname, dom = os.environ.get("USERNAME", ""), os.environ.get("USERDOMAIN", "")
    user = (dom + "\\" + uname) if (uname and dom) else uname
    xml = windows_task_xml(argv, interval_s, highest, user)
    out(f"Task Scheduler task '{TASK_NAME}': at logon (+2 min) and every {interval_s // 60} min, "
        f"run level {'HIGHEST' if highest else 'normal (limited)'}\n  {' '.join(argv)} check --quiet")
    if not highest:
        out("Note: smartctl needs Administrator rights on Windows. Re-run from an elevated (Run as Administrator)\n"
            "terminal:  hdmon install-service   so the task runs with highest privileges. Without it the task\n"
            "falls back to the Windows Storage counters, or reports drives as unreadable.")
    if dry_run or not enable:
        out(xml)
        return 0
    import tempfile
    fd, xml_path = tempfile.mkstemp(suffix=".xml")
    os.close(fd)
    try:
        with open(xml_path, "w", encoding="utf-16") as fh:
            fh.write(xml)
        ok = _run(["schtasks", "/Create", "/TN", TASK_NAME, "/XML", xml_path, "/F"], out)
    finally:
        os.unlink(xml_path)
    if ok:
        out(f"Scheduled. Check: schtasks /Query /TN {TASK_NAME}   Turn off: hdmon uninstall-service")
    return 0 if ok else 1


def uninstall(system: bool = False, out=print, platform: Optional[str] = None) -> int:
    osn = os_name(platform)
    if osn == "linux":
        if system:
            out("Remove the root units yourself:\n  sudo systemctl disable --now hard-drive-monitor.timer\n"
                "  sudo rm /etc/systemd/system/hard-drive-monitor.{service,timer} && sudo systemctl daemon-reload")
            return 0
        d = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "systemd" / "user"
        user_bus = _systemd_user_ok()
        if user_bus and (d / "hard-drive-monitor.timer").exists():
            _run(["systemctl", "--user", "disable", "--now", "hard-drive-monitor.timer"], out)
        for name in ("hard-drive-monitor.timer", "hard-drive-monitor.service"):
            p = d / name
            if p.exists():
                p.unlink()
                out(f"Removed {p}")
        if user_bus:
            _run(["systemctl", "--user", "daemon-reload"], out)
        cur = _crontab_read()
        if cur and any(CRON_MARK in l for l in cur):
            _crontab_write([l for l in cur if CRON_MARK not in l])
            out("Removed hard-drive-monitor lines from your crontab")
        out("Hourly check removed.")
        return 0
    if osn == "macos":
        if system:
            out(f"sudo launchctl bootout system/{LABEL} && sudo rm /Library/LaunchDaemons/{LABEL}.plist")
            return 0
        path = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
        uid = os.getuid() if hasattr(os, "getuid") else 0
        if not _run(["launchctl", "bootout", f"gui/{uid}/{LABEL}"], out):
            _run(["launchctl", "unload", "-w", str(path)], out)
        if path.exists():
            path.unlink()
            out(f"Removed {path}")
        return 0
    return 0 if _run(["schtasks", "/Delete", "/TN", TASK_NAME, "/F"], out) else 1
