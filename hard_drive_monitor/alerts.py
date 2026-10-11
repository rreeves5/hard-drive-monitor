"""Desktop notifications and SMTP e-mail. Alerts fire only on worsening."""
from __future__ import annotations

import os
import shutil
import smtplib
import socket
import ssl
import subprocess
from email.message import EmailMessage
from typing import Any, Dict, List, Tuple

from .health import Assessment
from .paths import os_name
from .smart import Drive, powershell_exe, run_cmd

PRETTY = {
    "reallocated_sectors": "reallocated sectors", "pending_sectors": "pending sectors",
    "offline_uncorrectable": "offline uncorrectable sectors", "reported_uncorrectable": "reported uncorrectable errors",
    "command_timeout": "command timeouts", "udma_crc_errors": "UDMA CRC errors", "media_errors": "NVMe media errors",
    "uncorrected_errors": "uncorrected errors", "status": "status",
}


def drive_label(d: Drive) -> str:
    vol = f" ({' '.join(d.volumes)})" if d.volumes else ""
    return f"{d.device}{vol} {d.model or ''}".strip()


def build_alerts(results: List[Tuple[Drive, Assessment]], changes: Dict[str, List[Dict[str, Any]]]) -> List[Dict[str, str]]:
    alerts = []
    for d, a in results:
        ch = [c for c in changes.get(d.key, []) if c.get("alert")]
        if not ch:
            continue
        lines = []
        for c in ch:
            if c["kind"] == "status":
                lines.append(f"Status worsened: {c['old']} -> {c['new']}")
            else:
                lines.append(f"{PRETTY.get(c['metric'], c['metric'])}: {c['old']} -> {c['new']} (+{c['new'] - c['old']})")
        body = "\n".join(lines + [f"Health {a.health}%  Performance {a.performance}%  Status {a.status}"] +
                         [f"- {r}" for r in a.reasons])
        alerts.append({"severity": a.status, "title": f"Hard Drive Monitor {a.status}: {drive_label(d)}", "body": body})
    return alerts


def notify_desktop(title: str, body: str) -> Tuple[bool, str]:
    osn = os_name()
    try:
        if osn == "macos":
            script = ["-e", "on run argv", "-e",
                      "display notification (item 2 of argv) with title (item 1 of argv) sound name \"Basso\"",
                      "-e", "end run", title, body[:240]]
            p = run_cmd(["osascript"] + script, 30)
            return p.returncode == 0, p.stderr.strip() or "osascript"
        if osn == "windows":
            ps = powershell_exe()
            if not ps:
                return False, "PowerShell not found; skipped desktop notification"
            env = dict(os.environ, HDMON_TITLE=title, HDMON_BODY=body[:240])
            script = (
                "$t=$env:HDMON_TITLE; $b=$env:HDMON_BODY;"
                "if (Get-Module -ListAvailable -Name BurntToast) { Import-Module BurntToast;"
                " New-BurntToastNotification -Text $t, $b; 'burnttoast' } else {"
                " Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing;"
                " $n = New-Object System.Windows.Forms.NotifyIcon; $n.Icon = [System.Drawing.SystemIcons]::Warning;"
                " $n.Visible = $true; $n.ShowBalloonTip(10000, $t, $b, 'Warning'); Start-Sleep -Seconds 10;"
                " $n.Dispose(); 'balloon' }")
            p = run_cmd([ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script], 60, env=env)
            return p.returncode == 0, (p.stdout.strip() or p.stderr.strip())
        if shutil.which("notify-send"):
            p = run_cmd(["notify-send", "-a", "Hard Drive Monitor", "-u", "critical", "-i", "drive-harddisk", title, body], 30)
            return p.returncode == 0, p.stderr.strip() or "notify-send"
        return False, "notify-send not found (install libnotify-bin / libnotify); skipped desktop notification"
    except (OSError, subprocess.SubprocessError) as e:
        return False, str(e)


def send_email(cfg: Dict[str, Any], subject: str, body: str) -> Tuple[bool, str]:
    e = cfg.get("email") or {}
    if not e.get("enabled"):
        return False, "email disabled"
    to = e.get("to") or []
    if isinstance(to, str):
        to = [to]
    if not (e.get("host") and to and e.get("from")):
        return False, "email enabled but host/from/to missing in config"
    password = e.get("password") or os.environ.get(e.get("password_env") or "", "")
    msg = EmailMessage()
    msg["Subject"], msg["From"], msg["To"] = subject, e["from"], ", ".join(to)
    msg.set_content(body + f"\n\n-- Hard Drive Monitor on {socket.gethostname()}")
    try:
        port = int(e.get("port") or (465 if e.get("ssl") else 587))
        if e.get("ssl"):
            s = smtplib.SMTP_SSL(e["host"], port, context=ssl.create_default_context(), timeout=30)
        else:
            s = smtplib.SMTP(e["host"], port, timeout=30)
            if e.get("starttls", True):
                s.starttls(context=ssl.create_default_context())
        with s:
            if e.get("username"):
                s.login(e["username"], password)
            s.send_message(msg)
        return True, "sent"
    except (OSError, smtplib.SMTPException) as ex:
        return False, f"email failed: {ex}"


def dispatch(cfg: Dict[str, Any], alerts: List[Dict[str, str]]) -> List[str]:
    log = []
    if not alerts:
        return log
    if (cfg.get("alerts") or {}).get("desktop", True):
        for a in alerts:
            ok, info = notify_desktop(a["title"], a["body"])
            log.append(f"desktop: {'ok' if ok else 'skipped/failed'} ({info})")
    if (cfg.get("email") or {}).get("enabled"):
        subject = alerts[0]["title"] if len(alerts) == 1 else f"Hard Drive Monitor: {len(alerts)} drives need attention"
        ok, info = send_email(cfg, subject, "\n\n".join(f"{a['title']}\n{a['body']}" for a in alerts))
        log.append(f"email: {info}")
    return log
