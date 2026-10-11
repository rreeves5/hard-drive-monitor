"""Config loading (~/.config/hard-drive-monitor/config.toml, %APPDATA%\\hard-drive-monitor\\config.toml on Windows).

Uses tomllib (Python 3.11+) or tomli when present. On older Pythons (e.g. the
macOS system Python 3.9) a small built-in parser handles the simple
`key = value` / `[section]` subset that the sample config uses.
"""
import copy
import json
import re
from pathlib import Path

from .paths import config_path

DEFAULTS = {
    "smartctl": {
        "path": "",          # empty = auto-detect
        "sudo": "auto",      # "auto" = retry with `sudo -n` on permission errors; true/false
    },
    "alerts": {
        "desktop": True,     # notify-send (Linux) / osascript (macOS)
    },
    "email": {
        "enabled": False,
        "host": "",
        "port": 587,
        "starttls": True,
        "ssl": False,        # implicit TLS (port 465)
        "username": "",
        "password": "",
        "password_env": "HDMON_SMTP_PASSWORD",
        "from": "",
        "to": [],
    },
    "thresholds": {
        "hdd_temp_warn": 50, "hdd_temp_crit": 55,
        "ssd_temp_warn": 60, "ssd_temp_crit": 70,
        "nvme_temp_warn": 70, "nvme_temp_crit": 80,
    },
    "ignore": {
        "devices": [],       # e.g. ["/dev/sdz"]
    },
}

SAMPLE_CONFIG = '''# Hard Drive Monitor (hdmon) configuration
# Location: ~/.config/hard-drive-monitor/config.toml  (Windows: %APPDATA%\\hard-drive-monitor\\config.toml)

[smartctl]
# path = "/usr/sbin/smartctl"   # leave unset to auto-detect
# "auto": if smartctl gets "permission denied", retry once with `sudo -n` (needs a sudoers rule)
sudo = "auto"

[alerts]
desktop = true

[email]
enabled = false
host = "smtp.gmail.com"
port = 587
starttls = true
ssl = false
username = "you@example.com"
# Prefer an environment variable over a password in this file:
password_env = "HDMON_SMTP_PASSWORD"
# password = ""
from = "you@example.com"
to = ["you@example.com"]

[thresholds]
hdd_temp_warn = 50
hdd_temp_crit = 55
ssd_temp_warn = 60
ssd_temp_crit = 70
nvme_temp_warn = 70
nvme_temp_crit = 80

[ignore]
devices = []
'''


def _parse_value(raw: str):
    raw = raw.strip()
    if not raw:
        return ""
    if raw[0] == '"':
        m = re.match(r'"((?:[^"\\]|\\.)*)"', raw)
        if m:
            return json.loads('"' + m.group(1) + '"')
    if raw[0] == "'":
        end = raw.find("'", 1)
        return raw[1:end]
    if raw[0] == "[":
        end = raw.rfind("]")
        inner = raw[1:end]
        items, buf, q = [], "", None
        for ch in inner:
            if q:
                buf += ch
                if ch == q:
                    q = None
            elif ch in "\"'":
                q = ch
                buf += ch
            elif ch == ",":
                if buf.strip():
                    items.append(_parse_value(buf))
                buf = ""
            else:
                buf += ch
        if buf.strip():
            items.append(_parse_value(buf))
        return items
    raw = raw.split("#", 1)[0].strip()
    if raw in ("true", "false"):
        return raw == "true"
    try:
        return int(raw.replace("_", ""))
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw


def parse_simple_toml(text: str) -> dict:
    """Minimal TOML subset parser: [section], key = string/int/float/bool/array."""
    out: dict = {}
    section = out
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("[") and s.endswith("]"):
            section = out
            for part in s[1:-1].strip().split("."):
                section = section.setdefault(part.strip(), {})
            continue
        if "=" in s:
            key, val = s.split("=", 1)
            section[key.strip().strip('"')] = _parse_value(val)
    return out


def _loads(text: str) -> dict:
    try:
        import tomllib  # type: ignore
        return tomllib.loads(text)
    except ImportError:
        pass
    try:
        import tomli  # type: ignore
        return tomli.loads(text)
    except ImportError:
        return parse_simple_toml(text)


def merge(base: dict, override: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = v
    return out


def load_config(path=None) -> dict:
    p = Path(path) if path else config_path()
    if p.exists():
        return merge(DEFAULTS, _loads(p.read_text()))
    return copy.deepcopy(DEFAULTS)


def write_sample(path=None, force=False) -> Path:
    p = Path(path) if path else config_path()
    if p.exists() and not force:
        raise FileExistsError(str(p))
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(SAMPLE_CONFIG)
    try:
        p.chmod(0o600)  # may hold SMTP credentials
    except OSError:
        pass
    return p
